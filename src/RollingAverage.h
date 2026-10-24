#pragma once

////////////////////////////////////////////////////////////////////////////////
// Optimized Rolling Average Filter
// Copyright (C) https://muman.ch + https://github.com/mumanchu, 2026.10.24
// If you re-use this software, please include the above copyright notice
/*
Averages values of any numerical data type. The data type of the sum value 
SUMTYPE can also be defined.

For float and double, SUMTYPE can be omitted, so SUMTYPE is float or double 
by default.

For integer types, the SUMTYPE must be able to hold 'MAXVALUE * numberOfSamples' 
without overflowing or underflowing. e.g. for a 16-bit 'int' you should use a 
32-bit 'long', else the sum may overflow/underflow. This is not checked by the 
compiler. On a 32-bit machine, 'int' and 'long' are both 32 bits, so you may 
need to use 'long long' (64 bits) or int64_t if the sum is very large.

If you are averaging 10-bit analog input values (0..1023) on an 8 or 16-bit 
processor (16-bit integers), then you can use 'unsigned int' for both because 
it will not overflow if you don't sum more than 64 values. This is faster than 
using 'long'.

But if you use 'float' for averaging integer values you will get a much smoother 
result. This removes the 'step effect' you can get when reading narrow-range 
analogue inputs.

As usual, all methods must be called from the same thread (i.e. not from an 
interrupt), else use some kind of mutex lock.
*/

template <typename T, typename SUMTYPE = T>
class RollingAverage
{
	T* samples = NULL;
	int bufferLength = 0;
	int index = 0;
	bool bufferFull = false;

	// SUMTYPE must be able to hold the sum of all values (max/min) without overflow
	SUMTYPE sum;

public:
	RollingAverage() { }
	RollingAverage(int numberOfSamples) { begin(numberOfSamples); }
	~RollingAverage();

	bool begin(int numberOfSamples);
	void clear();
	void addSample(T sample);
	T getAverage();
	int getSampleCount() { return bufferFull ? bufferLength : index; }
	int getBufferLength() { return bufferLength; }
};

// A destructor is not usually needed for embedded code - use static objects
template <typename T, typename SUMTYPE>
RollingAverage<T, SUMTYPE>::~RollingAverage()
{
	if (samples) {
		free(samples);
		samples = NULL;
	}
}

template <typename T, typename SUMTYPE>
bool RollingAverage<T, SUMTYPE>::begin(int numberOfSamples)
{
	if (numberOfSamples <= 0)
		numberOfSamples = 1;
	bufferLength = numberOfSamples;
	if (samples)
		free(samples);
	samples = (T*)malloc(bufferLength * sizeof(T));
	if (samples == NULL) {
		// fatal error, out of memory
		return false;
	}
	bufferLength = bufferLength;
	clear();
	return true;
}

template <typename T, typename SUMTYPE>
void RollingAverage<T, SUMTYPE>::clear()
{
	index = 0;
	sum = 0;
	bufferFull = false;
}

template <typename T, typename SUMTYPE>
void RollingAverage<T, SUMTYPE>::addSample(T sample)
{
	if (bufferFull)
		sum -= samples[index];
	sum += sample;
	samples[index] = sample;
	if (++index == bufferLength) {
		index = 0;
		bufferFull = true;
	}
}

template <typename T, typename SUMTYPE>
T RollingAverage<T, SUMTYPE>::getAverage()
{
	if (bufferFull)
		return sum / bufferLength;
	if (index == 0)
		return 0;
	return sum / index;
}
