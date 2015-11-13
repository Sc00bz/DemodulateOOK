# DemodulateOOK
Demodulate on/off keying.

## Usage
```
./demodulate-ook file-name
```
If the file isn't detected as a wav file it will assume it is 16 bits/sample, 1 channel, signed integers, and little endian.

## "Issues"
* When using 32 bit samples it needs to allocate 16 GiB (4*2^32 bytes) of RAM.
* Messes up if there are >256 bits set to on or off. Ignoring the beginning and the end of the data and anything longer than 96000 samples that don't switch state.
* 10 samples/bit is the minimum.
* You can't select which channel to use. It just uses the first channel.
* There are issues with clock skew and amplitude skew. For short messages this shouldn't be a problem.
