#pragma once

/**
	The four passes.

	1. **copy** -- the host's input into a texture of ours, with a mip chain.
	   Resolves MaxUV and the half-texel inset once, so the wall pass can box
	   average a pitch's worth of source through the mip chain. At an interior
	   texel centre it is an exact copy, which the identity check relies on.

	2. **wall** -- one texel per LED. Resamples the picture onto the LED grid
	   at `Pitch` source pixels per LED, aligned to whole cabinets, and applies
	   the seeded per-cabinet faults. Its output is the drive level of every
	   LED, in linear code values, with nothing about time in it.

	3. **sensor** -- straight to the host, or into a buffer when the Bayer
	   mosaic is on. Every sensor pixel works out where its aperture lands on
	   the wall, which LEDs it overlaps and by how much (analytic coverage of
	   each emitter rectangle), and what each of those LEDs was emitting over
	   this ROW's exposure window (the closed-form overlap with the PWM pulse
	   train). That is the whole plugin; see the comment at the top of
	   `kSensorShader`.

	4. **demosaic** -- when Bayer is on: the sensor image through an RGGB
	   mosaic and a plain bilinear demosaic, then the mix.

	All are complete shaders -- nothing is assembled at run time -- so the
	text `tools/verify.sh` extracts and compiles is the text the plugin runs.
*/
namespace pitch::shaders
{

extern const char* const kVertexShader;
extern const char* const kCopyShader;
extern const char* const kWallShader;
extern const char* const kSensorShader;
extern const char* const kDemosaicShader;

} // namespace pitch::shaders
