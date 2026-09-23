#pragma once

#include "PassBuffer.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

/**
	Pitch -- an LED wall seen through a camera, as an FFGL effect.

	**The one idea.** Every IMAG camera pointed at an LED wall samples a grid
	with a grid, twice: once in space (the sensor's pixel lattice over the
	wall's LED lattice) and once in time (each sensor row's exposure window
	over the wall's multiplexed PWM pulse train). Everything an operator
	fights on a show is what those two samplings do:

	  - moire: the LED lattice aliased against the sensor lattice, coloured
	    by the Bayer mosaic, swimming with Camera Scale, gone with Focus;
	  - scan bands: each row integrates a different slice of the pulse train;
	  - low-grey breakup: a dark level is a short pulse, and a short pulse
	    bands worst; Scrambled PWM spreads it out;
	  - black between the pixels: the fill factor;
	  - cabinet seams: a cabinet that is dead, dim or has a dead row.

	**Four passes**, in `Shaders.h`: copy the input, resample it onto the LED
	grid with the faults, integrate the LEDs over each sensor pixel's aperture
	and each row's exposure window, and optionally put a Bayer mosaic and a
	bilinear demosaic in the way. The PWM overlap is closed-form time; the
	per-row windows are computed in double here and handed over as small
	numbers. See `Drive.h` for the arithmetic and AGENTS.md for the traps.
*/
class Pitch : public CFFGLPlugin
{
public:
	Pitch();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one -- an absolute time handed over in
	/// a single frame is genuinely ambiguous.
	void SetClockScaleForTest( double scale );

	/// Negative-control hooks. The first detunes the PWM overlap integral by
	/// a fraction (the shipped shader carries the term at exactly zero); the
	/// second swaps the Bayer mosaic's phase. Both exist so the harness can
	/// prove its checks are able to fail.
	void SetOverlapDetuneForTest( float detune );
	void SetBayerPhaseForTest( int phase );

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Wall
		PT_PITCH,
		PT_FILL,
		PT_LAYOUT,
		PT_CABINET_W,
		PT_CABINET_H,
		PT_MODULE_ROWS,

		//Drive
		PT_REFRESH,
		PT_SCAN,
		PT_GREY_BITS,
		PT_PWM,
		PT_REFRESH_PHASE,

		//Camera
		PT_SCALE,
		PT_ROTATION,
		PT_FOCUS,
		PT_OLPF,
		PT_SHUTTER,
		PT_READOUT,
		PT_FRAME_RATE,
		PT_BAYER,

		//Faults
		PT_FAULT_RATE,
		PT_DEAD,
		PT_DIM,
		PT_DEAD_ROW,
		PT_BIN_SHIFT,
		PT_FAULT_SEED,

		//Output
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// The largest wall, per axis, in LEDs. 4096 x 4096 x 16 bytes is 256 MB
	/// of float texture, which is where a mistake stops being a wall.
	static constexpr int kMaxWallSide = 4096;

	/// Taps on the focus disc when Focus is on.
	static constexpr int kFocusTaps = 32;

private:
	/// The host's clock in seconds, whatever unit it arrived in.
	double nowSeconds();

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader wallShader;
	ffglex::FFGLShader sensorShader;
	ffglex::FFGLShader demosaicShader;
	ffglex::FFGLScreenQuad quad;

	pitch::PassBuffer picture;///< the input, mipmapped
	pitch::PassBuffer wall;   ///< one texel per LED
	pitch::PassBuffer sensor; ///< the sensor image before the mosaic

	/// Per sensor row, the exposure window in sub-periods as ( floor u0,
	/// frac u0, floor u1, frac u1 ), computed in double every frame.
	GLuint rowTable       = 0;
	int rowTableRows      = 0;
	std::vector< float > rowWindows;

	//--- the clock (readout's unit voting) -----------------------------------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastNow      = -1.0;
	int clockFrames     = 0;

	/// The host's frame period, measured from its own deltas. It is what
	/// turns host frames into camera frames when the camera's rate differs.
	double frameSeconds = 1.0 / 60.0;

	//--- test hooks ----------------------------------------------------------
	float overlapDetune = 0.0f;
	int bayerPhase      = 0;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
