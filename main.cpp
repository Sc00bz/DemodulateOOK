/*
	Copyright (c) 2015 Steve "Sc00bz" Thomas (steve at tobtu dot com)

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all
	copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	SOFTWARE.
*/

#include <stdio.h>
#include <stdint.h>

// Max span of on or off is 2 seconds at 48 kHz
#define MAX_SPAN       (2*48000)
// Samples needed to change on/off
#define RADIO_FLICKER  5

struct wavHeader
{
	uint32_t tag;            // "RIFF"
	uint32_t fileSize;       // realFileSize-8
	uint32_t type;           // "WAVE"
	uint32_t chunkMarker;    // "fmt "
	uint32_t fileSizeSoFar;  // 16
	uint16_t format;         // 1 = PCM
	uint16_t channels;
	uint32_t sampleRate;
	uint32_t byteRate;
	uint16_t bytesPerSample;
	uint16_t bitsPerSample;
	uint32_t dataTag;        // "data"
	uint32_t dataSize;
};

/**
 * Make a file format value give the file's format.
 *
 * @param sampleByteSize - The sample size in bytes (1 to 4)
 * @param channels       - Number of channels (1 to 256)
 * @param channel        - The channel number (0 to 255)
 * @param isSigned       - If samples are signed integer (0 or 1)
 * @param isLittleEndian - If samples are in little endian (0 or 1)
 * @return The file format value
 */
uint32_t makeFileFormat(uint32_t sampleByteSize, uint32_t channels, uint32_t channel, uint32_t isSigned, uint32_t isLittleEndian)
{
	return
		 ((sampleByteSize - 1) &    3)        |
		(((channels       - 1) & 0xff) <<  2) |
		(( channel             & 0xff) << 10) |
		(( isSigned            &    1) << 18) |
		(( isLittleEndian      &    1) << 19);
}

/**
 * Make a file format value give the file's format.
 *
 * @param fileFormat - The file format
 * @return The sample size in bytes (1 to 4)
 */
uint32_t getSampleByteSize(uint32_t fileFormat)
{
	return (fileFormat & 3) + 1;
}

/**
 * Reads a sample from the input file.
 *
 * @param fin        - The input file at an offset into the data
 * @param fileFormat - The file format
 * @param error      - Set to non-zero if there's an error
 * @return The value of the sample or UINT32_MAX on error
 */
uint32_t getSample(FILE *fin, uint32_t fileFormat, uint32_t *error = NULL)
{
	uint32_t sampleSize     = ( fileFormat        &    3) + 1;
	uint32_t channels       = ((fileFormat >>  2) & 0xff) + 1;
	uint32_t channel        =  (fileFormat >> 10) & 0xff;
	uint32_t isSigned       =  (fileFormat >> 18) &    1;
	uint32_t isLittleEndian =  (fileFormat >> 19) &    1;
	uint32_t sample = 0;
	uint8_t  sample8[4];

	if (channels > 1)
	{
		if (fseek(fin, sampleSize * channel, SEEK_CUR))
		{
			if (!feof(fin))
			{
				perror("fseek");
			}
			if (error != NULL)
			{
				*error = 1;
			}
			return UINT32_MAX;
		}
	}
	if (fread(sample8, sampleSize, 1, fin) != 1)
	{
		if (!feof(fin))
		{
			perror("fread");
		}
		if (error != NULL)
		{
			*error = 1;
		}
		return UINT32_MAX;
	}
	if (channels > 1)
	{
		if (fseek(fin, sampleSize * (channels - channel - 1), SEEK_CUR))
		{
			if (!feof(fin))
			{
				perror("fseek");
			}
			if (error != NULL)
			{
				*error = 1;
			}
			return UINT32_MAX;
		}
	}
	if (error != NULL)
	{
		*error = 0;
	}

	// Endian
	if (isLittleEndian)
	{
		for (int i = (int) sampleSize - 1; i >= 0; i--)
		{
			sample <<= 8;
			sample  |= sample8[i];
		}
	}
	else
	{
		for (uint32_t i = 0; i < sampleSize; i++)
		{
			sample <<= 8;
			sample  |= sample8[i];
		}
	}

	// Signed to unsigned
	if (isSigned)
	{
		sample = (sample + (1 << (8 * sampleSize - 1))) & ((1 << (8 * sampleSize)) - 1);
	}

	return sample;
}

/**
 * Counts samples of each value.
 *
 * @param counts     - A pointer to integers that receive the number of samples with said value
 * @param fin        - The input file at the offset of where the data starts
 * @param fileFormat - The file format
 * @return The total number of samples or UINT32_MAX on error
 */
uint32_t getCounts(uint32_t *counts, FILE *fin, uint32_t fileFormat)
{
	uint32_t count     = 0;
	uint32_t error     = 0;
	size_t   numCounts = ((size_t) 1) << (8 * getSampleByteSize(fileFormat));

	for (size_t i = 0; i < numCounts; i++)
	{
		counts[i] = 0;
	}
	while (!feof(fin))
	{
		uint32_t sample = getSample(fin, fileFormat, &error);

		if (error)
		{
			if (feof(fin))
			{
				break;
			}
			return UINT32_MAX;
		}

		counts[sample]++;
		count++;
	}
	return count;
}

/**
 * Finds a threshold value that anything above the value is on and anything below is off.
 * This is done by averaging the highest sample and lowest sample, ignoring the highest 2% and lowest 2% of samples.
 *
 * @param counts     - A constant pointer to integers that were generated from calling getCounts()
 * @param count      - The total number of samples
 * @param fileFormat - The file format
 * @return The threshold value between on and off
 */
uint32_t findOnOffThreshold(const uint32_t *counts, uint32_t count, uint32_t fileFormat)
{
	size_t   numCounts = ((size_t) 1) << (8 * getSampleByteSize(fileFormat));
	uint32_t hi = 0;
	uint32_t lo = (uint32_t) (numCounts - 1);
	uint32_t skipCount = count / 50; // 2%
	uint32_t curCount = 0;

	// Get hi and lo
	for (size_t i = 0; i < numCounts; i++)
	{
		if (counts[i] != 0)
		{
			curCount += counts[i];
			if (curCount > skipCount)
			{
				lo = (uint32_t) i;
				break;
			}
		}
	}
	curCount = 0;
	for (uint32_t i = (uint32_t) (numCounts - 1); i > lo; i--)
	{
		if (counts[i] != 0)
		{
			curCount += counts[i];
			if (curCount > skipCount)
			{
				hi = i;
				break;
			}
		}
	}
	if (lo >= hi)
	{
		return 0;
	}
	return (hi + lo) / 2;
}

/**
 * Moves the file offset past the initial on/off state.
 *
 * @param state          - Set to the first state (on/off)
 * @param radioFlicker   - Number samples needed to change the state
 * @param onOffThreshold - The threshold value between on and off
 * @param fin            - The input file at the offset of where the data starts
 * @param fileFormat     - The file format
 * @return leftOver (used by subsequent calls to getNextSpan())
 */
uint32_t ignoreFirstSpan(uint32_t &state, uint32_t radioFlicker, uint32_t onOffThreshold, FILE *fin, uint32_t fileFormat)
{
	uint32_t nextCount = 0;
	uint32_t curState;
	uint32_t newState;
	uint32_t sample;
	uint32_t error = 0;

	// Get state
	sample = getSample(fin, fileFormat, &error);
	if (error)
	{
		if (feof(fin))
		{
			return 0;
		}
		return UINT32_MAX;
	}

	curState = 1; // on
	if (sample < onOffThreshold)
	{
		curState = 0; // off
	}

	// Read samples
	while (!feof(fin))
	{
		sample = getSample(fin, fileFormat, &error);
		if (error)
		{
			if (feof(fin))
			{
				break;
			}
			return UINT32_MAX;
		}

		newState = 1; // on
		if (sample < onOffThreshold)
		{
			newState = 0; // off
		}
		if (newState != curState)
		{
			nextCount++;
			if (nextCount > radioFlicker)
			{
				return nextCount;
			}
		}
		else
		{
			nextCount = 0;
		}
	}
	return 0;
}

/**
 * Moves the file offset to the next on/off state and returns number of samples in the current state.
 *
 * @param state          - Set to the first state (on/off). Do not modify this between call of ignoreFirstSpan() or getNextSpan()
 * @param radioFlicker   - Number samples needed to change the state
 * @param onOffThreshold - The threshold value between on and off
 * @param fin            - The input file at the offset of where the data starts
 * @param fileFormat     - The file format
 * @param leftOver       - The left over from the previous call of ignoreFirstSpan() or getNextSpan()
 * @return The number of samples of state
 */
uint32_t getNextSpan(uint32_t &state, uint32_t radioFlicker, uint32_t onOffThreshold, FILE *fin, uint32_t fileFormat, uint32_t &leftOver)
{
	uint32_t count = leftOver;
	uint32_t nextCount = 0;
	uint32_t curState = 0;
	uint32_t newState;
	uint32_t error = 0;

	// Flip state
	if (state == 0)
	{
		curState = 1;
	}
	state = curState;

	// Read samples
	while (!feof(fin))
	{
		uint32_t sample = getSample(fin, fileFormat, &error);

		if (error)
		{
			if (feof(fin))
			{
				break;
			}
			return UINT32_MAX;
		}

		newState = 1; // on
		if (sample < onOffThreshold)
		{
			newState = 0; // off
		}
		if (newState != curState)
		{
			nextCount++;
			if (nextCount > radioFlicker)
			{
				leftOver = nextCount;
				return count;
			}
		}
		else
		{
			count += nextCount + 1;
			nextCount = 0;
		}
	}

	// Ignore last span
	return 0;
}

/**
 * Counts spans of samples in either on or off states.
 *
 * @param spans          - An array of maxSpan+1 integers
 * @param maxSpan        - The max span to record
 * @param onOffThreshold - The threshold value between on and off
 * @param fin            - The input file at the offset of where the data starts
 * @param fileFormat     - The file format
 * @return The max span or UINT32_MAX on error
 */
uint32_t getSpans(uint32_t *spans, uint32_t maxSpan, uint32_t onOffThreshold, FILE *fin, uint32_t fileFormat)
{
	uint32_t leftOver;
	uint32_t realMaxSpan = 0;
	uint32_t count;
	uint32_t state = 0; // 0 = off, 1 = on

	if (maxSpan == UINT32_MAX)
	{
		maxSpan = UINT32_MAX - 1;
	}
	for (uint32_t i = 0; i <= maxSpan; i++)
	{
		spans[i] = 0;
	}
	leftOver = ignoreFirstSpan(state, RADIO_FLICKER, onOffThreshold, fin, fileFormat);
	if (leftOver == UINT32_MAX)
	{
		return UINT32_MAX;
	}

	while (1)
	{
		count = getNextSpan(state, RADIO_FLICKER, onOffThreshold, fin, fileFormat, leftOver);
		if (count == UINT32_MAX)
		{
			return UINT32_MAX;
		}
		if (count == 0)
		{
			break; // EOF
		}

		if (realMaxSpan < count)
		{
			realMaxSpan = count;
		}
		if (count <= maxSpan)
		{
			spans[count]++;
		}
	}
	return realMaxSpan;
}

/**
 * Finds the width of a single bit in number of samples.
 *
 * @param spans   - An array of at least maxSpan+1 integers
 * @param maxSpan - The max span to record
 * @return The width of a single bit in number of samples or 0 on error
 */
uint32_t findSingleBitWidth(const uint32_t *spans, uint32_t maxSpan)
{
	double minErr = 1e99;
	double curErr;
	uint32_t hi = 0;
	uint32_t lo = maxSpan;
	uint32_t bestSingleBitWidth = 0;

	// Get hi and lo
	for (uint32_t i = 0; i < maxSpan; i++)
	{
		if (spans[i] != 0)
		{
			lo = i;
			break;
		}
	}
	for (uint32_t i = maxSpan; i > lo; i--)
	{
		if (spans[i] != 0)
		{
			hi = i;
			break;
		}
	}
	if (lo > hi)
	{
		return 0;
	}

	// min 10 samples/bit
	// max 256 bits without change
	uint32_t endAt = hi / 256;
	if (endAt < 10)
	{
		endAt = 10;
	}

	// Brute force... meh
	for (uint32_t i = hi; i >= endAt; i--)
	{
		curErr = 0;
		for (uint32_t j = lo; j < hi; j++)
		{
			if (spans[j] != 0)
			{
				uint32_t error;
				double scaledError;

				error = j % i;
				if (error > i - error)
				{
					error = i - error;
				}
				scaledError = (double) error / i;
				curErr += scaledError * scaledError * spans[j];
			}
		}
		if (minErr > curErr)
		{
			bestSingleBitWidth = i;
			minErr = curErr;
		}
	}
	return bestSingleBitWidth;
}

/**
 * Outputs the data.
 *
 * @param singleBitWidth - The width of a single bit in number of samples
 * @param onOffThreshold - The threshold value between on and off
 * @param fin            - The input file at the offset of where the data starts
 * @param fileFormat     - The file format
 * @return The bit length of the data or UINT32_MAX on error
 */
uint32_t printMessage(uint32_t singleBitWidth, uint32_t onOffThreshold, FILE *fin, uint32_t fileFormat)
{
	uint32_t bitLength = 0;
	uint32_t leftOver;
	uint32_t samples;
	uint32_t bits;
	uint32_t state = 0; // 0 = off, 1 = on
	uint8_t  currentByte = 0;

	leftOver = ignoreFirstSpan(state, RADIO_FLICKER, onOffThreshold, fin, fileFormat);
	if (leftOver == UINT32_MAX)
	{
		return UINT32_MAX;
	}

	while (1)
	{
		samples = getNextSpan(state, RADIO_FLICKER, onOffThreshold, fin, fileFormat, leftOver);
		if (samples == UINT32_MAX)
		{
			return UINT32_MAX;
		}
		if (samples == 0)
		{
			break; // EOF
		}

		// Round to the nearest number of bits
		bits = (samples + singleBitWidth / 2) / singleBitWidth;

		uint32_t left = 8 - bitLength % 8;
		bitLength += bits;
		if (state == 0) // off
		{
			if (left <= bits)
			{
				printf("%02x", currentByte);
				currentByte = 0;

				// full bytes
				uint32_t fullBytes = (bits - left) / 8;
				for (uint32_t i = 0; i < fullBytes; i++)
				{
					printf("00");
				}
			}
		}
		else // on
		{
			if (left <= bits)
			{
				currentByte |= (1 << left) - 1;
				printf("%02x", currentByte);
				currentByte = 0;

				// full bytes
				uint32_t fullBytes = (bits - left) / 8;
				for (uint32_t i = 0; i < fullBytes; i++)
				{
					printf("ff");
				}
				bits -= 8 * fullBytes + left;
				left = 8;
			}
			// extra
			currentByte |= (1 << left) - (1 << (left - bits));
		}
	}
	if (bitLength % 8 != 0)
	{
		printf("%02x", currentByte);
	}
	printf("\n");
	return bitLength;
}

int main(int argc, char *argv[])
{
	uint32_t  *counts;
	uint32_t  *spans  = new uint32_t[MAX_SPAN + 1];
	FILE      *fin;
	wavHeader  header;
	uint32_t   startOffset = 0;
	uint32_t   fileSize;
	uint32_t   count;
	uint32_t   bitLength;
	uint32_t   singleBitWidth;
	// 16 bits/sample, 1 channel, signed integers, little endian
	uint32_t   fileFormat = makeFileFormat(2, 1, 0, 1, 1);
	uint32_t   onOffThreshold;

	if (argc != 2)
	{
		fprintf(stderr, "usage:\n\"%s\" file-name\n", argv[0]);
		return 1;
	}

	// Open wav
	fin = fopen(argv[1], "rb");
	if (fin == NULL)
	{
		perror("fopen");
		return 1;
	}

	// File size
    fseek(fin, 0, SEEK_END);
    fileSize = ftell(fin);
    fseek(fin, 0, SEEK_SET);

	// Read wav header
	if (fileSize >= 44)
	{
		if (fread(&header, sizeof(wavHeader), 1, fin) != 1)
		{
			perror("fread");
			return 1;
		}

		if (header.tag           == 0x46464952   && // "RIFF"
		    header.fileSize      == fileSize - 8 &&
		    header.type          == 0x45564157   && // "WAVE"
		    header.chunkMarker   == 0x20746d66   && // "fmt "
		    header.fileSizeSoFar ==         16   &&
		    header.format        ==          1   && // PCM
		    header.dataTag       == 0x61746164   && // "data"
		    header.dataSize      == fileSize - 44)
		{
			if (header.channels          ==   0 ||
			    header.channels          >  256 ||
			    header.bitsPerSample % 8 !=   0 ||
				header.bitsPerSample     ==   0 ||
				header.bitsPerSample     >   32)
			{
				fprintf(stderr, "Error: Only supports raw 16 bit signed data and 8, 16, 24, 32 bit .wav with <257 channels\n");
				return 1;
			}
			else
			{
				printf("File is a .wav\n");
				startOffset = 44;
				fileFormat = makeFileFormat(header.bitsPerSample / 8, header.channels, 0, 1, 1);
			}
		}
		else
		{
			printf("Assuming file is raw 16 bit signed data\n");
			fseek(fin, 0, SEEK_SET);
		}
	}
	if (startOffset == 0)
	{
		if (fileSize % 2 == 1)
		{
			fprintf(stderr, "Error: Only supports raw 16 bit signed data and 8, 16, 24, 32 bit .wav with <257 channels\n");
			return 1;
		}
	}

	size_t numCounts = ((size_t) 1) << (8 * getSampleByteSize(fileFormat));
	// Check for size overflow
	if (numCounts == 0)
	{
		fprintf(stderr, "Error: 32 bit samples requires a 64 bit binary and 16 GiB of RAM.\n");
		return 1;
	}
	counts = new uint32_t[numCounts];

	// Count samples
	printf("Counting...\n");
	count = getCounts(counts, fin, fileFormat);
	if (count == UINT32_MAX)
	{
		return 1;
	}
	fseek(fin, startOffset, SEEK_SET);

	// Finding on off ranges
	printf("Finding on off ranges...\n");
	onOffThreshold = findOnOffThreshold(counts, count, fileFormat);
	if (onOffThreshold == 0)
	{
		fprintf(stderr, "Error: Can't find on off ranges\n");
		return 1;
	}

	// Getting spans
	printf("Getting spans...\n");
	uint32_t realMaxSpan = getSpans(spans, MAX_SPAN, onOffThreshold, fin, fileFormat);
	if (realMaxSpan == UINT32_MAX)
	{
		fprintf(stderr, "Error: 1\n");
		return 1;
	}
	fseek(fin, startOffset, SEEK_SET);

	// Finding single bit width
	printf("Finding single bit width...\n");
	singleBitWidth = findSingleBitWidth(spans, realMaxSpan);
	if (singleBitWidth == 0)
	{
		fprintf(stderr, "Error: 2\n");
		return 1;
	}

	// Print bit width
	printf("samples/bit: %u\n", singleBitWidth);
	if (startOffset != 0) // .wav
	{
		printf("seconds/bit: %0.9f\n", (double) singleBitWidth / header.sampleRate);
		printf("bits/second: %0.3f\n", (double) header.sampleRate / singleBitWidth);
	}

	// Print message
	bitLength = printMessage(singleBitWidth, onOffThreshold, fin, fileFormat);
	if (bitLength == UINT32_MAX)
	{
		fprintf(stderr, "Error: Durp?\n");
		return 1;
	}
	return 0;
}
