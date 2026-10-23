///////////////////////////////////////////////////////////////////////////////
// Example Sketch for Arduino QMC5883PMagnetometer Library
// Copyright (C) https://muman.ch + https://github.com/mumanchu, 2026.10.24
// If you re-use this software, please include the above copyright notice
// For details see,
// https://github.com/mumanchu/QMC5883PMagnetometer

#include <Wire.h>

// Comment this out for release mode, to remove the LOGERROR() output
#define DEBUG

#ifdef DEBUG
#define LOGERROR(s) { Serial.println(s); Serial.flush(); }
#else
#define LOGERROR(s)
#endif

typedef unsigned long ulong;    // 32 bits
typedef unsigned int uint;      // 16 or 32 bits
typedef unsigned short ushort;  // 16 bits

// true if sprintf() supports floats
bool floatSupportedInSprintf = false;

// Un-patch this to run the rotationalCalibration() tests
//#define QMC5883P_ENABLE_CALIBRATION

#include "QMC5883PMagnetometer.h"
QMC5883PMagnetometer compass;


void setup()
{
	// Experimental, there's no #define for this
	// does sprintf() support floats on this platform?
	char buf[256];
	sprintf(buf, "%f", 1.234f);
	floatSupportedInSprintf = strlen(buf) > 5;

	// I use different pins for Serial logging
	// this is only for the STM32 Nucleo-64 boards
	#ifdef ARDUINO_NUCLEO_64
	Serial.setTx(PC_10);
	Serial.setRx(PC_11);
	#endif

	Serial.begin(115200);
	delay(3000);

	// PuTTY clear screen and scrollback
	// patch out if not using PuTTY
	Serial.print("\033[2J\033[H\033[3J");

	Serial.println("\n\rStarted\n\r");
	Serial.flush();

	// Some boards have no user LED
	#ifdef LED_BUILTIN
	pinMode(LED_BUILTIN, OUTPUT);
	#endif

	Wire.begin();
	Wire.setClock(400000);
	Wire.setTimeout(100);

	if (!compass.begin(&Wire, 20)) {
		LOGERROR("compass.begin() failed");
		while (1)
			yield();
	}
	if (!compass.softReset()) {
		LOGERROR("compass.softReset() failed");
		while (1)
			yield();
	}

	// configure the QMC5333P before running the self-test
	compass.configure(2, 0, 0);		// 100Hz sampling
	compass.setRange(3);			// 2 Gauss range
	compass.setOperatingMode(1);	// normal mode

	if (!compass.selfTest()) {
		LOGERROR("compass.selfTest() failed");
		while (1)
			yield();
	}


	// This calls rotationalCalibration() continuously,
	// so you can see if it works, or not
	#ifdef QMC5883P_ENABLE_CALIBRATION

	// configure the QMC5333P before running the calibration
	compass.configure(3, 0, 0);		// 200Hz sampling
	compass.setRange(3);			// 2 Gauss range
	compass.setOperatingMode(1);	// normal mode

	while (1) {
		Serial.println();
		Serial.println("Calibration in progress...");
		Serial.flush();

		// un-patch this in QMC5882PMagnetometer.h
		if (!compass.rotationalCalibration()) {
			LOGERROR("compass.rotationalCalibration() failed");
			delay(3000);
			continue;
		}

		// in case I2C hangs
		void resetI2C(TwoWire * wire);
		resetI2C(&Wire);

		delay(500);
	}
	#endif

	// re-configure the QMC5333P for normal use
	compass.configure(2, 0, 0);		// 100Hz sampling
	compass.setRange(3);			// 2 Gauss range
	compass.setOperatingMode(1);	// normal mode

	// clear status, so the 1st reading is ignored
	bool rdy, ovf;
	compass.readStatus(&rdy, &ovf);
}


void loop()
{
	char buf[256];

	// readings ready yet?
	bool rdy, ovf;
	compass.readStatus(&rdy, &ovf);

	if (ovf) {
		Serial.println("*** overflow");
	}
	if (rdy) {
		int x, y, z;
		compass.readMeasurements(&x, &y, &z);

		// degress from magnetic north
		uint degrees = compass.getDegreesFromNorth(x, y, z, 0, 0);

		#if 0
		// for CSV file
		sprintf(buf, "%i,%i,%i,%i", degrees, x, y, z);
		
		#else 
		// for normal display
		if (floatSupportedInSprintf) {
			float gaussx = compass.getGauss(x);
			float gaussy = compass.getGauss(y);
			float gaussz = compass.getGauss(z);

			sprintf(buf, "degrees=%-3i    x=%-4i  y=%-4i  z=%-4i    gauss  x=%.3f  y=%.3f  z=%.3f",
				degrees, x, y, z, gaussx, gaussy, gaussz);
		}
		else {
			sprintf(buf, "degrees=%-3i    x=%-4i  y=%-4i  z=%-4i", degrees, x, y, z);
		}
		#endif

		Serial.println(buf);
		Serial.flush();

		// new readings available, toggle the LED
		#ifdef LED_BUILTIN
		digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
		#endif
	}
}

// Unlock the I2C bus, the QMC5883P chip sometimes hangs the bus 
// when calibrating, could be a bad I2C connection
void resetI2C(TwoWire* wire)
{
	wire->end();
	delay(100);
	wire->begin();
	wire->setClock(400000);
	wire->setTimeout(100);
}


