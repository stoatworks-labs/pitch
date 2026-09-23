#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	Every ranged FF_TYPE_STANDARD parameter this plugin declares is a plain
	0..1 float, including the ones that stand for hertz, milliseconds or LEDs
	per pixel. `CFFGLPluginManager::SetParamInfo` clamps a standard default
	into 0..1 *before* returning, and `SetParamRange` can only be called
	afterwards -- so a parameter declared in hertz cannot declare a default in
	hertz. The conversions live here instead, in one file the plugin and the
	harness both use, so there is only ever one answer to what a slider
	position means.

	`FF_TYPE_INTEGER` is exempt from the clamp and holds its real value, which
	is why the cabinet sizes, the module rows, the grey bits and the seed are
	declared as real integers with real ranges.

	Every geometric mapping has an inverse. The harness needs to say "one LED
	per pixel" and "a shutter of exactly two refresh periods" in the slider's
	own units, and computing the position by hand is how a check ends up
	testing a number near the one it claims.

	Options are mapped by INDEX here. An option parameter's range reads back
	0..1 from the SDK whatever its element count, so nothing outside this file
	should reason from the range.
*/
namespace pitch::controls
{

/// Pitch: 1 to 16 source pixels per LED, geometrically. 1 is what the
/// identity check needs; 16 turns a 1080p clip into a 120x68 wall.
float PitchPixels( float value );
float PitchParam( float pixels );

/// Fill factor: 0.05 to 1, linear. The fraction of each LED cell that emits.
/// 1 is a wall with no black between the pixels, which is also the identity.
float FillFactor( float value );
float FillParam( float fill );

/// Refresh: 240 to 7680 Hz, geometrically. The rate the whole PWM pattern
/// repeats at. 3840 is the figure quoted for a broadcast-grade wall.
float RefreshHz( float value );
float RefreshParam( float hz );

/// Scan ratio option index to the number of scan groups S: 1, 2, 4 ... 32.
int ScanGroups( float optionValue );
constexpr int kScanCount = 6;
const char* ScanName( int index );

/// PWM option index to the number of sub-periods the refresh is split into:
/// 1 for Conventional, kScrambledSubPeriods for Scrambled.
constexpr int kScrambledSubPeriods = 16;
int SubPeriods( float optionValue );

/// Camera scale: 0.125 to 8 LEDs per sensor pixel, geometrically. Zoom and
/// distance in one number: 1 is one LED per pixel, where the moire is worst.
float CameraScale( float value );
float CameraScaleParam( float ledsPerPixel );

/// Rotation: -10 to +10 degrees, linear about 0.5, in radians. 0.5 is exactly
/// zero, which is what makes the axis-aligned coverage path exact.
float RotationRadians( float value );
float RotationParam( float degrees );

/// Focus: 0 to 4 sensor pixels of blur-disc radius, geometric from 0.05 and
/// floored to exactly zero at the bottom of the travel.
float FocusRadius( float value );
float FocusParam( float pixels );

/// Shutter: 1/8000 to 1/24 s, geometric in the denominator. Returns seconds.
float ShutterSeconds( float value );
float ShutterParam( float seconds );

/// Readout: 4 to 40 ms, geometrically. How long the sensor takes to read
/// every row. Returns seconds.
float ReadoutSeconds( float value );
float ReadoutParam( float seconds );

/// Frame rate option index to hertz: 23.98, 24, 25, 29.97, 30, 50, 59.94, 60.
float FrameRateHz( float optionValue );
constexpr int kFrameRateCount = 8;
const char* FrameRateName( int index );

/// Fault rate: 0 to 1, linear. The fraction of cabinets that carry a fault of
/// a kind whose amount is 1.
float FaultRate( float value );

} // namespace pitch::controls
