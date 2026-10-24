#pragma once

///////////////////////////////////////////////////////////////////////////////
// Arduino Library for QMC5833P 3-Axis Magnetometer
// Copyright (C) https://muman.ch + https://github.com/mumanchu, 2026.10.24
// If you re-use this software, please include the above copyright notice
// For details see,
// https://github.com/mumanchu/QMC5883PMagnetometer
/*
QMC5883P Data Sheet, Rev.E
The page numbers refer to this version of the data sheet,
https://www.qstcorp.com/upload/pdf/202512/2C939E5AA0704285BC3BE71132B8629B.pdf

NOAA Magnetic Declination Calculator
https://www.ngdc.noaa.gov/geomag/calculators/magcalc.shtml#declination
*/

#include <Wire.h>
#include "RollingAverage.h"


class QMC5883PMagnetometer
{
	TwoWire* wire;
	const int i2cAdds = 0x2C;	// hard wired I2C address
	byte control1Shadow;		// shadow registers
	byte control2Shadow;

	// Averaging for getDegreesFromNorth()
	RollingAverage<short, long> aveDegX;
	RollingAverage<short, long> aveDegY;

	// Register Numbers
	typedef enum : byte
	{
		ChipId = 0,
		DataX = 1,
		DataY = 3,
		DataZ = 5,
		Status = 9,
		Control1 = 10,			// 0x0A
		Control2 = 11,			// 0x0B	
		AxisCfg = 0x29			// not fully documented

	} QMC5883P_REG;

	// Number of counts-per-Gauss, according to the range RNG, p4
	static const short countsPerGauss[4];

public:
	// Range values from selfTest()
	int testRangeX, testRangeY, testRangeZ;

	// Calibration factors from rotationCalibration()
	float offsetX, offsetY, offsetZ;
	float scaleX, scaleY, scaleZ;

	bool begin(TwoWire* twoWire, uint degreesFromNorthSamples = 0);
	bool softReset();
	bool selfTest();
	bool configure(uint outputDataRate, uint overSampling, uint downSampling);
	bool setRange(uint range);
	bool setOperatingMode(byte operatingMode);
	bool readOperatingMode(byte* operatingMode);
	bool readStatus(bool* rdy, bool* ovf);
	bool readMeasurements(int* x, int* y, int* z);
	float getGauss(int measurement);
	uint getDegreesFromNorth(int x, int y, int z, 
		uint declinationDegrees, uint declinationMinutes);
	bool rotationCalibration();
	bool applyCalibration(int x, int y, int z, int* xc, int* yc, int* zc);

private:
	bool readShadowRegisters();
	bool readReg(QMC5883P_REG reg, byte* data, uint length = 1);
	bool writeReg(QMC5883P_REG reg, byte data);
};

// Number of counts-per-Gauss according to the range RNG, p4
const short QMC5883PMagnetometer::countsPerGauss[4] = 
	{ 1000, 2500, 3750, 15000 };


// Startup
// degreesFromNorthSamples : number of samples for getDegreesFromNorth() smoothing
//  0 = no smoothing, typical = 10, bytes used = degreesFromNorthSamples * 4
bool QMC5883PMagnetometer::begin(TwoWire* twoWire, uint degreesFromNorthSamples /*=0*/)
{
	wire = twoWire;
	control1Shadow = 0;
	control2Shadow = 0;
	testRangeX = 0;
	testRangeY = 0;
	testRangeZ = 0;

	// from rotationCalibration();
	offsetX = 0.0f;
	offsetY = 0.0f;
	offsetZ = 0.0f;
	scaleX = 0.0f;
	scaleY = 0.0f;
	scaleZ = 0.0f;

	// averaging for getDegreesFromNorth()
	if (degreesFromNorthSamples != 0) {
		if (!aveDegX.begin(degreesFromNorthSamples) || 
			!aveDegY.begin(degreesFromNorthSamples)) {
			LOGERROR("out of memory");
			return false;
		}
	}

	// just in case, needs 50mS startup time
	while (millis() < 50)
		yield();

	// is it really a QMC5883P?
	byte chipId;
	if (!readReg(ChipId, &chipId))
		return false;				// not responding
	if (chipId != 0x80) {
		LOGERROR("not a QMC5883P");
		return false;
	}

	// reset the chip to default settings
	return softReset();
}

// Software reset
// chip remains in Suspended mode
bool QMC5883PMagnetometer::softReset()
{
	control1Shadow = 0;
	control2Shadow = 0;
	if (!writeReg(Control2, 0x80))
		return false;
	delay(100);
	// the SOFT_RST bit remains set?
	if (!writeReg(Control2, 0))
		return false;
	// write Register 0x29 with 0x06 (define the sign for X Y and Z axis), p12
	if (!writeReg(AxisCfg, 0x06))
		return false;
	if (!readShadowRegisters())
		return false;
	return true;
}

// Run self test as described on p12
// After the test the chip remains in Suspended mode
// The numbered comments (1) .. (8) are taken directly from the data sheet
// Test results are saved in in testRangeX, testRangeY and testRangeZ.
// The differential for each axis is about 1 Gauss, so the results should
// be close to the 'countsPerGauss' value (+-15%) unless the chip is 
// faulty or there is an overwhelmingly strong external magnetic field.
// 
// p16 states, "an inner-built current is generated and an additional signal 
// is added to the sensor, generating a difference in the 3 axis value. User 
// should record the value before and after the self-test and compare with 
// a threshold value."
bool QMC5883PMagnetometer::selfTest()
{
	// zeros if it fails
	testRangeX = 0;
	testRangeY = 0;
	testRangeZ = 0;

	control2Shadow &= 0x0C;

	// (1) Write Register 29H by 0x06 (define the sign for X Y and Z axis)
	// matt: already done in begin() / softwareReset(), do we need this again?
	if (!writeReg(AxisCfg, 0x06))
		return false;

	const int nRuns = 10;	// number of results to average
	long dx = 0;
	long dy = 0;
	long dz = 0;

	for (int i = 0; i < nRuns; ++i) {

		// clear status bits
		bool rdy, ovf;
		if (!readStatus(&rdy, &ovf))
			return false;

		// (2) Write Register 0AH by 0x03 (set continuous mode)
		if (!setOperatingMode(3))
			return false;

		// (3) Check status register 09H[0], "1" means ready
		ulong t = micros();
		while (1) {
			if (!readStatus(&rdy, &ovf))
				return false;
			if (ovf) {
				LOGERROR("overflow");
				return false;
			}
			if (rdy)
				break;
			if ((micros() - t) > 1000000) {		// timeout after 1 second
				LOGERROR("timeout");
				return false;
			}
		}

		// (4) Read data Register 01H ~06H, recording as datax1 / datay1 / dataz1
		int x1, y1, z1;
		if (!readMeasurements(&x1, &y1, &z1))
			return false;

		// (5) Write Register 0BH by 0x40 (enter self-test function)
		// matt: this code keeps the selected range RNG in control2Shadow
		if (!writeReg(Control2, 0x40 + control2Shadow))
			return false;
		delay(1);

		// (6) Waiting 5 millisecond until measurement ends
		//delay(5);
		// matt: wait until the SELF_TEST bit is reset
		// self-test bit auto-clears after the data is updated, p17
		t = micros();
		while (1) {
			byte ctrl2;
			if (!readReg(Control2, &ctrl2))
				return false;
			if ((ctrl2 & 0x40) == 0)		// test complete
				break;
			if ((micros() - t) > 10000) {	// timeout after 10mS
				// reset the self-test bit
				writeReg(Control2, control2Shadow);
				LOGERROR("timeout");
				return false;
			}
		}

		// (7) Read data Register 01H ~06H, recording as datax2 / datay2 / dataz2
		int x2, y2, z2;
		if (!readMeasurements(&x2, &y2, &z2))
			return false;

		// (8) Calculate the deltas (datax1 - datax2), (datay2 - datay1), (dataz2 - dataz1)
		// matt: these values depend on the range (RNG), outputDataRate, overSampling
		// and downSampling, see setRange() and configure()
		// these are also affected by the chip's motion, orientation and external fields
		// THESE DELTAS ARE FOR THE CURRENT CONFIGURATION ONLY!

		// sum the deltas
		dx += x1 - x2;
		dy += y2 - y1;
		dz += z2 - z1;
	}

	// the test results are the ranges for a change of about 1Gauss, the test's
	// 'max - min' range
	// they should be within +-15% of the countsPerGauss value for the range
	testRangeX = dx / nRuns;
	testRangeY = dy / nRuns;
	testRangeZ = dz / nRuns;

	#ifdef DEBUG
	char buf[128];
	sprintf(buf, "selfTest results : testRangeX=%d testRangeY=%d testRangeZ=%d", 
		testRangeX, testRangeY, testRangeZ);
	Serial.println(buf);
	Serial.flush();
	#endif

	return true;
}

// Configure the chip, p16
// outputDataRate : 0 = 10Hz, 1 = 50Hz, 2 = 100Hz, 3 = 200Hz
// overSampling   : 0 = 8x, 1 = 4x, 2 = 2x, 3 = 1x
// downSampling   : 0 = 1x, 1 = 2x, 2 = 4x, 3 = 8x
// leaves the chip in 'Suspended' mode
bool QMC5883PMagnetometer::configure(uint outputDataRate, uint overSampling, uint downSampling)
{
	if (outputDataRate > 3 || overSampling > 3 || downSampling > 3) {
		LOGERROR("bad param");
		return false;
	}
	control1Shadow = (outputDataRate << 2) + (overSampling << 4) + (downSampling << 6);
	return writeReg(Control1, control1Shadow);
}

// range (RNG) : 0 = 30Gauss, 1 = 12Gauss, 2 = 8Gauss, 3 = 2Gauss
bool QMC5883PMagnetometer::setRange(uint range)
{
	if (range > 3) {
		LOGERROR("bad param");
		return false;
	}
	// SET/RESET MODE is always 0 = Set and Reset ON
	control2Shadow = range << 2;
	return writeReg(Control2, control2Shadow);
}

// See p11 for description of operating modes
// operatingMode : 0 = Suspended, 1 = Normal, 2 = Single, 3 = Continuous
bool QMC5883PMagnetometer::setOperatingMode(byte operatingMode)
{
	if (operatingMode > 3) {
		LOGERROR("bad param");
		return false;
	}

	// "Suspend Mode should be added in the middle of mode shifting
	// between Continuous Mode, Single Mode and Normal Mode", p16
	if (operatingMode != 0) {
		// set Suspend mode, keeping the OSR2, OSR1 and ODR bits
		control1Shadow &= ~0x03;
		if (!writeReg(Control1, control1Shadow))
			return false;
	}
	
	// set the new mode, keeping the OSR2, OSR1 and ODR bits
	control1Shadow = (control1Shadow & ~0x03) + operatingMode;
	return writeReg(Control1, control1Shadow);
}

// *operatingMode : 0 = Suspended, 1 = Normal, 2 = Single, 3 = Continuous
bool QMC5883PMagnetometer::readOperatingMode(byte* operatingMode)
{
	byte b;
	bool ok = readReg(Control1, &b);
	*operatingMode = b & 0x03;
	return ok;
}

// rdy : data ready, data written to X Y Z registers
// ovf : data overflow, outside range -30000..+30000
// bits are cleared when read
inline bool QMC5883PMagnetometer::readStatus(bool* rdy, bool* ovf)
{
	byte status;
	bool ok = readReg(Status, &status);
	*rdy = (status & 0x01) != 0;
	*ovf = (status & 0x02) != 0;
	return ok;
}

// Read the latest raw X, Y, Z values
// range -30000 .. +30000 (averflow at or above this)
bool QMC5883PMagnetometer::readMeasurements(int* x, int* y, int* z)
{
	byte data[6];
	bool ok = readReg(DataX, data, 6);
	*x = *(short*)data;
	*y = *(short*)(data + 2);
	*z = *(short*)(data + 4);
	return ok;
}

// Return the Gauss equivalent of a raw value according to the selected range
// The countsPerGauss values are from the data sheet, p4
float QMC5883PMagnetometer::getGauss(int raw)
{
	if (raw < 0)
		raw = -raw;
	return (float)raw / (float)countsPerGauss[(control2Shadow >> 2) & 0x03];
}


// Return the number of degrees clockwise from true north or from magnetic 
// north. 'declinationDegrees' and 'declinationMinutes' define the magnetic 
// declination of your location, or use '0, 0' for magnetic north. These 
// can also be used for simple calibration. 
// The magnetic declination is the angle between magnetic north and true 
// north on the Earth's surface. The angle can change over time due to 
// "polar wandering". If you know where you are (on the Earth's surface), 
// you can find your current magnetic declination with this NOAA calculator, 
// https://www.ngdc.noaa.gov/geomag/calculators/magcalc.shtml#declination
// Smoothing is done according to 'degreesFromNorthSamples' in begin().
uint QMC5883PMagnetometer::getDegreesFromNorth(int x, int y, int z, 
	uint declinationDegrees, uint declinationMinutes)
{
	if (declinationDegrees > 359 || declinationMinutes > 59)
		return -1;		// 0xFFFF or 0xFFFFFFFF

	float declination = 0.0f;
	if (declinationDegrees != 0 || declinationMinutes != 0)
		declination = declinationDegrees + (declinationMinutes / 60.0f);

	// averaging, see 'degreesFromNorthSamples'
	if (aveDegX.getBufferLength() > 0) {
		aveDegX.addSample(x);
		aveDegY.addSample(y);
		x = aveDegX.getAverage();
		y = aveDegY.getAverage();
	}

	// get heading in degrees, (180.0f / PI) converts radians to degrees
	float h = ((atan2(y, x) * (180.0f / PI))) + declination;
	h += (h < 0.0f ? -0.5f : +0.5f);	// round to nearest int
	int i = (int)h;
	if (i < 0)
		i += 360;
	return (uint)i % 360;
}


// Simple rotation calibration, where each axis of the sensor is rotated 
// through 360 degrees. Returns true when the calibration is complete, or 
// false if it fails (insufficient movement), or times out. 
// NOTES 
// This procedure saves the max and min values (OUTLIERS) for each axis. 
// The difficult bit is determining when the calibration is complete. One 
// way to do that is to check the range between min and max (maybe compare 
// with the selfTest() results). 
// This version just uses a timer, assuming calibration is complete if no 
// change has occured for (say) 10 seconds.
// After calibration, the offsetX/Y/Z and scaleX/Y/Z values should be saved 
// in nonvolatile memory and restored on startup, to save re-calibration. 
// The results are all over the place (not repeatable), OUTLIERS problem? 

//TODO implement an error number return value instead of just bool

// Normally patched out, unless you want to experiment
#ifdef QMC5883P_ENABLE_CALIBRATION

bool QMC5883PMagnetometer::rotationCalibration()
{
	const uint timeoutInSeconds = 60;
	const uint noChangesTimeInSeconds = 10;

	#ifdef DEBUG
	char buf[256];		// for sprintf()
	#endif

	// calibration values are all zeros if it fails
	offsetX = 0.0f;
	offsetY = 0.0f;
	offsetZ = 0.0f;
	scaleX = 0.0f;
	scaleY = 0.0f;
	scaleZ = 0.0f;

	// selfTest() should have been run first to set up the testRange values
	if (testRangeX == 0 || testRangeY == 0 || testRangeZ == 0) {
		LOGERROR("run setfTest() first");
		return false;
	}

	// max and min measurements
	int minX = 0;
	int maxX = 0;
	int minY = 0;
	int maxY = 0;
	int minZ = 0;
	int maxZ = 0;

	if (!setOperatingMode(1))		// normal mode
		return false;

	// clear status so the 1st reading is ignored
	bool rdy, ovf;
	if (!readStatus(&rdy, &ovf))
		return false;

	ulong tstart = millis();
	ulong timeout = timeoutInSeconds * 1000;

	// timer for detecting when calibration is complete
	ulong tlastChange = tstart;

	while (1) {
		// check for timeout, the sensor may not be being rotated
		ulong t = millis();

		if ((t - tstart) > timeout) {
			LOGERROR("\n\rtimeout");
			#ifdef DEBUG
			break;		// when debugging, show the values
			#else
			return false;
			#endif
		}

		// wait until next reading is ready
		if (!readStatus(&rdy, &ovf))
			return false;
		if (!rdy)
			continue;
		if (ovf) {
			LOGERROR("\n\roverflow");
			return false;
		}

		// save max and min values for each axis
		// (these could be OUTLIERS, not good)
		int x, y, z;
		if (!readMeasurements(&x, &y, &z))
			return false;

		bool changed = false;
		if (x < minX) {
			minX = x;
			changed = true;
		}
		if (x > maxX) {
			maxX = x;
			changed = true;
		}
		if (y < minY) {
			minY = y;
			changed = true;
		}
		if (y > maxY) {
			maxY = y;
			changed = true;
		}
		if (z < minZ) {
			minZ = z;
			changed = true;
		}
		if (z > maxZ) {
			maxZ = z;
			changed = true;
		}
		if (changed)
			tlastChange = t;

		/*
		// create data for a CSV file
		sprintf(buf, "%i,%i,%i", x, y, z);
		Serial.println(buf);
		Serial.flush();
		*/

		#ifdef DEBUG
		// show the current readings
		// NOTE! the Arduino IDE Serial Monitor does not handle '\r' (carriage return)
		// correctly. It always does a line feed, which is very annoying, so you can't 
		// display repeated values on the same line. That's why I always use PuTTY.
		sprintf(buf, "minX=%-5i maxX=%-5i  minY=%-5i maxY=%-5i  minZ=%-5i maxZ=%-5i   t=%lu   \r",
			minX, maxX, minY, maxY, minZ, maxZ, (t - tlastChange) / 1000);
		Serial.print(buf);
		Serial.flush();
		#endif

		// if no changes for some time, assume it's done
		if ((t - tlastChange) > (noChangesTimeInSeconds * 1000)) {
			LOGERROR("\n\rcalibration complete");
			break;
		}

		// -OR-  you could check for a full range of values
		/*
		// detect one full rotation of the axis by comparing with the
		// selfTest range values
		if (maxX - minX >= testRangeX)
			xDone = true;
		if (maxY - minY >= testRangeY)
			yDone = true;
		if (maxZ - minZ >= testRangeZ)
			zDone = true;
		if (xDone && yDone && zDone) {
			LOGERROR("\n\rcalibration complete");
			break;
		}
		*/
	}

	// an axis did not move
	if (minX == 0 || maxX == 0 || minY == 0 || maxY == 0 || minZ == 0 || maxZ == 0) {
		LOGERROR("insufficient motion 1");
		#ifndef DEBUG	// if debugging, we still want to see the values
		return false;
		#endif
	}

	// check the readings span the expected range
	uint cpg = countsPerGauss[(control2Shadow >> 2) & 0x03];
	int minRange = cpg - (cpg / 10);
	if (maxX - minX < minRange || maxY - minY < minRange || maxZ - minZ < minRange) {
		LOGERROR("insufficient motion 2");
		#ifndef DEBUG	// if debugging, we still want to see the values
		return false;
		#endif
	}

	// save the calibration factors
	// offsets to zero point
	offsetX = (maxX + minX) / 2.0f;
	offsetY = (maxY + minY) / 2.0f;
	offsetZ = (maxZ + minZ) / 2.0f;
	// scale multipliers
	float avgX = (maxX - minX) / 2.0f;
	float avgY = (maxY - minY) / 2.0f;
	float avgZ = (maxZ - minZ) / 2.0f;
	float avg = (avgX + avgY + avgZ) / 3.0f;
	scaleX = avg / avgX;
	scaleY = avg / avgY;
	scaleZ = avg / avgZ;

	#ifdef DEBUG
	// if debugging, show the results
	if (floatSupportedInSprintf) {	// %f is not always supported
		sprintf(buf, "testRangeX=%i testRangeY=%i testRangeZ=%i",
			testRangeX, testRangeY, testRangeZ);
		Serial.println(buf);
		Serial.flush();
		sprintf(buf, "rangeX=%-5i   rangeY=%-5i   rangeZ=%-5i",
			maxX - minX, maxY - minY, maxZ - minZ);
		Serial.println(buf);
		Serial.flush();
		sprintf(buf, "offsetX=%.4f  offsetY=%.4f  offsetZ=%.4f",
			offsetX, offsetY, offsetZ);
		Serial.println(buf);
		Serial.flush();
		sprintf(buf, "scaleX=%.4f  scaleY=%.4f  scaleZ=%.4f",
			scaleX, scaleY, scaleZ);
		Serial.println(buf);
		Serial.flush();
	}
	#endif

	return true;
}

// Apply calibration factors to the raw values
bool QMC5883PMagnetometer::applyCalibration(int x, int y, int z, int* xc, int* yc, int* zc)
{
	if (offsetX == 0) {
		// they will all be 0
		LOGERROR("not calibrated");
		return false;
	}
	*xc = (int)((x - offsetX) * scaleX);
	*yc = (int)((y - offsetY) * scaleY);
	*zc = (int)((z - offsetZ) * scaleZ);
	return true;
}
#endif


// Internal Methods

// The Control1 and Control2 registers are partially updated by some of the
// methods. To save losing settings or re-reading the register each time,
// shadow copies of these registers are maintained.
bool QMC5883PMagnetometer::readShadowRegisters()
{
	bool ok;
	ok = readReg(Control1, &control1Shadow);
	ok &= readReg(Control2, &control2Shadow);
	// mask out unwanted bits (why does SOFT_RST remain set?)
	control2Shadow &= 0x0F;
	return ok;
}

// Read one or more registers
bool QMC5883PMagnetometer::readReg(QMC5883P_REG reg, byte* data, uint length /*=1*/)
{
	memset(data, 0, length);		// return 0s if it fails

	wire->beginTransmission(i2cAdds);
	wire->write((byte)reg);
	if (wire->endTransmission(false) != 0) {
		LOGERROR("endTransmission() failed 1");
		return false;
	}
	wire->requestFrom(i2cAdds, length);
	if (wire->readBytes((byte*)data, length) != length) {
		LOGERROR("readBytes() failed");
		return false;
	}
	return true;
}

// Write a single register
bool QMC5883PMagnetometer::writeReg(QMC5883P_REG reg, byte data)
{
	wire->beginTransmission(i2cAdds);
	wire->write((byte)reg);
	wire->write(data);
	if (wire->endTransmission() != 0) {
		LOGERROR("endTransmission() failed 2");
		return false;
	}
	return true;
}
