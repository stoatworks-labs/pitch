#include "Pitch.h"

#include "Controls.h"
#include "Diag.h"
#include "Drive.h"
#include "Shaders.h"

#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace pitch;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Pitch >,                                      // Create method
	"PI01",                                                      // Plugin unique ID of maximum length 4.
	"SW Pitch",                                                  // Plugin name
	2,                                                           // API major version number
	1,                                                           // API minor version number
	0,                                                           // Plugin major version number
	1,                                                           // Plugin minor version number
	FF_EFFECT,                                                   // Plugin type
	"An LED wall seen through a camera.\n\nThe clip is resampled onto a wall of LEDs with black between them, lit one scan group at a time by PWM. A camera with a rolling shutter and a Bayer sensor then photographs it.\n\nWhat falls out: coloured moire that swims with the zoom, scan bands that roll, low greys that break up, and cabinets that are dead, dim or a shade off.",// Plugin description
	"Pitch FFGL effect"                                          // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame is allowed to advance the clock by.
constexpr double kMaxFrameDelta = 0.25;

const char* const kLayoutNames[] = { "3-in-1 SMD", "Discrete RGB" };
const char* const kPwmNames[]    = { "Conventional", "Scrambled" };

/// Wall clock, for hosts that never call SetTime. Steady rather than system,
/// so nothing here moves when the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// Split a non-negative double into ( whole, fraction ) floats. The whole
/// part is exact up to 2^24 and the fraction is a small number, so the
/// shader never holds anything a float cannot resolve.
void split( double u, float& whole, float& fraction )
{
	const double w = std::floor( u );
	whole          = static_cast< float >( w );
	fraction       = static_cast< float >( u - w );
	//A fraction that rounds up to exactly 1.0 in float is a carry.
	if( fraction >= 1.0f )
	{
		fraction = 0.0f;
		whole += 1.0f;
	}
}
} // namespace

//---------------------------------------------------------------------------
Pitch::Pitch()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The scan bands are a function of when each row was exposed against the
	//wall's refresh, so the effect needs the host's clock: a re-render of the
	//same composition must band the same frame the same way.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults, in the slider's units, computed from the physical value so
	// that what the README says the default is, is the default.
	//
	// They add up to a 4-pixel-pitch wall of 32x32 cabinets at 3840 Hz and
	// 1/16 scan, seen at one LED per four sensor pixels (the wall fills the
	// frame) through a 1/1000 shutter: the pixel structure shows, the bands
	// show, and the moire arrives the moment Camera Scale moves.
	//---------------------------------------------------------------------
	params[ PT_PITCH ]  = controls::PitchParam( 4.0f );
	params[ PT_FILL ]   = controls::FillParam( 0.5f );
	params[ PT_LAYOUT ] = 0.0f;
	params[ PT_CABINET_W ]   = 32.0f;
	params[ PT_CABINET_H ]   = 32.0f;
	params[ PT_MODULE_ROWS ] = 16.0f;

	params[ PT_REFRESH ]       = controls::RefreshParam( 3840.0f );
	params[ PT_SCAN ]          = 4.0f;//1/16
	params[ PT_GREY_BITS ]     = 12.0f;
	params[ PT_PWM ]           = 0.0f;
	params[ PT_REFRESH_PHASE ] = 0.0f;

	params[ PT_SCALE ]      = controls::CameraScaleParam( 0.25f );
	params[ PT_ROTATION ]   = 0.5f;//zero
	params[ PT_FOCUS ]      = 0.0f;
	params[ PT_OLPF ]       = 0.0f;
	params[ PT_SHUTTER ]    = controls::ShutterParam( 1.0f / 1000.0f );
	params[ PT_READOUT ]    = controls::ReadoutParam( 0.016f );
	params[ PT_FRAME_RATE ] = 7.0f;//60
	params[ PT_BAYER ]      = 1.0f;

	params[ PT_FAULT_RATE ] = 0.0f;
	params[ PT_DEAD ]       = 0.3f;
	params[ PT_DIM ]        = 0.5f;
	params[ PT_DEAD_ROW ]   = 0.3f;
	params[ PT_BIN_SHIFT ]  = 0.5f;
	params[ PT_FAULT_SEED ] = 1.0f;

	params[ PT_MIX ] = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. Every ranged FF_TYPE_STANDARD parameter is a plain 0..1
	// float even where it stands for hertz: SetParamInfo clamps a STANDARD
	// default into 0..1 *before* a range can be attached (SDK b1afaf9). The
	// conversions live in Controls.cpp. FF_TYPE_INTEGER is exempt, so the
	// cabinet sizes, the module rows, the grey bits and the seed are declared
	// with their real ranges.
	//
	// Option lists are declared in their natural order and NOT sorted: every
	// one here is a progression.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int id, const char* name, const char* const* names, int count ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};
	auto declareInteger = [ this ]( unsigned int id, const char* name, float lo, float hi ) {
		SetParamInfo( id, name, FF_TYPE_INTEGER, params[ id ] );
		SetParamRange( id, lo, hi );
	};

	SetParamInfof( PT_PITCH, "Pitch", FF_TYPE_STANDARD );
	SetParamInfof( PT_FILL, "Fill Factor", FF_TYPE_STANDARD );
	declareOptions( PT_LAYOUT, "LED Layout", kLayoutNames, 2 );
	declareInteger( PT_CABINET_W, "Cabinet W", 8.0f, 256.0f );
	declareInteger( PT_CABINET_H, "Cabinet H", 8.0f, 256.0f );
	declareInteger( PT_MODULE_ROWS, "Module Rows", 2.0f, 64.0f );

	SetParamInfof( PT_REFRESH, "Refresh", FF_TYPE_STANDARD );
	{
		const char* scanNames[ controls::kScanCount ];
		for( int i = 0; i < controls::kScanCount; ++i )
			scanNames[ i ] = controls::ScanName( i );
		declareOptions( PT_SCAN, "Scan Ratio", scanNames, controls::kScanCount );
	}
	declareInteger( PT_GREY_BITS, "Grey Bits", 4.0f, 16.0f );
	declareOptions( PT_PWM, "PWM", kPwmNames, 2 );
	SetParamInfof( PT_REFRESH_PHASE, "Refresh Phase", FF_TYPE_STANDARD );

	SetParamInfof( PT_SCALE, "Camera Scale", FF_TYPE_STANDARD );
	SetParamInfof( PT_ROTATION, "Rotation", FF_TYPE_STANDARD );
	SetParamInfof( PT_FOCUS, "Focus", FF_TYPE_STANDARD );
	SetParamInfo( PT_OLPF, "OLPF", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_SHUTTER, "Shutter", FF_TYPE_STANDARD );
	SetParamInfof( PT_READOUT, "Readout", FF_TYPE_STANDARD );
	{
		const char* rateNames[ controls::kFrameRateCount ];
		for( int i = 0; i < controls::kFrameRateCount; ++i )
			rateNames[ i ] = controls::FrameRateName( i );
		declareOptions( PT_FRAME_RATE, "Frame Rate", rateNames, controls::kFrameRateCount );
	}
	SetParamInfo( PT_BAYER, "Bayer On", FF_TYPE_BOOLEAN, true );

	SetParamInfof( PT_FAULT_RATE, "Fault Rate", FF_TYPE_STANDARD );
	SetParamInfof( PT_DEAD, "Dead", FF_TYPE_STANDARD );
	SetParamInfof( PT_DIM, "Dim", FF_TYPE_STANDARD );
	SetParamInfof( PT_DEAD_ROW, "Dead Row", FF_TYPE_STANDARD );
	SetParamInfof( PT_BIN_SHIFT, "Bin Shift", FF_TYPE_STANDARD );
	declareInteger( PT_FAULT_SEED, "Fault Seed", 0.0f, 999.0f );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	for( FFUInt32 i = PT_PITCH; i <= PT_MODULE_ROWS; ++i )
		SetParamGroup( i, "Wall" );
	for( FFUInt32 i = PT_REFRESH; i <= PT_REFRESH_PHASE; ++i )
		SetParamGroup( i, "Drive" );
	for( FFUInt32 i = PT_SCALE; i <= PT_BAYER; ++i )
		SetParamGroup( i, "Camera" );
	for( FFUInt32 i = PT_FAULT_RATE; i <= PT_FAULT_SEED; ++i )
		SetParamGroup( i, "Faults" );
	SetParamGroup( PT_MIX, "Output" );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Pitch effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Pitch::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally: when a shader will not
	//compile it is almost always the driver or the GL version, and knowing
	//which machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &copyShader, shaders::kCopyShader, "copy" },
		{ &wallShader, shaders::kWallShader, "wall" },
		{ &sensorShader, shaders::kSensorShader, "sensor" },
		{ &demosaicShader, shaders::kDemosaicShader, "demosaic" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( shaders::kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Pitch: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Pitch: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	glGenTextures( 1, &rowTable );
	glBindTexture( GL_TEXTURE_2D, rowTable );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	rowTableRows = 0;

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Pitch::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//The unit voting is readout's, unchanged: the ratio of the host's clock
//delta to a steady clock's names the unit outright, and nothing plausible
//sits between 1 and 1000.
double Pitch::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
FFResult Pitch::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//The host's viewport, read before anything of ours changes it. Every
	//pass into a buffer sizes the viewport to that buffer, and nothing in
	//the SDK puts a viewport back -- ScopedFBOBinding restores the
	//framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// The clock, and the frame period measured from it.
	//---------------------------------------------------------------------
	const double now = nowSeconds();
	if( lastNow >= 0.0 && now > lastNow )
	{
		const double dt    = std::min( now - lastNow, kMaxFrameDelta );
		const double delta = std::clamp( dt, 1.0 / 240.0, 1.0 / 10.0 );
		frameSeconds += ( delta - frameSeconds ) * 0.15;
	}
	lastNow = now;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) + " frame=" + std::to_string( frameSeconds ) );

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const float pitch     = controls::PitchPixels( params[ PT_PITCH ] );
	const float fill      = controls::FillFactor( params[ PT_FILL ] );
	const int layout      = std::clamp( static_cast< int >( std::lround( params[ PT_LAYOUT ] ) ), 0, 1 );
	const int cabinetW    = std::clamp( static_cast< int >( std::lround( params[ PT_CABINET_W ] ) ), 8, 256 );
	const int cabinetH    = std::clamp( static_cast< int >( std::lround( params[ PT_CABINET_H ] ) ), 8, 256 );
	const int moduleRows  = std::clamp( static_cast< int >( std::lround( params[ PT_MODULE_ROWS ] ) ), 2, 64 );

	const double refreshHz  = controls::RefreshHz( params[ PT_REFRESH ] );
	const int scanGroups    = controls::ScanGroups( params[ PT_SCAN ] );
	const int greyBits      = std::clamp( static_cast< int >( std::lround( params[ PT_GREY_BITS ] ) ), 4, 16 );
	const int subPeriods    = controls::SubPeriods( params[ PT_PWM ] );
	const double refreshPhase = std::clamp( static_cast< double >( params[ PT_REFRESH_PHASE ] ), 0.0, 1.0 );

	const float scale       = controls::CameraScale( params[ PT_SCALE ] );
	const float rotation    = controls::RotationRadians( params[ PT_ROTATION ] );
	const float focus       = controls::FocusRadius( params[ PT_FOCUS ] );
	const bool olpf         = params[ PT_OLPF ] > 0.5f;
	const double shutter    = controls::ShutterSeconds( params[ PT_SHUTTER ] );
	const double readout    = controls::ReadoutSeconds( params[ PT_READOUT ] );
	const double frameRate  = controls::FrameRateHz( params[ PT_FRAME_RATE ] );
	const bool bayer        = params[ PT_BAYER ] > 0.5f;

	const float faultRate = controls::FaultRate( params[ PT_FAULT_RATE ] );
	const int seed        = std::clamp( static_cast< int >( std::lround( params[ PT_FAULT_SEED ] ) ), 0, 999 );

	//---------------------------------------------------------------------
	// The wall. `Pitch` source pixels per LED, and a whole number of
	// cabinets -- the nearest whole number to what the picture holds, at
	// least one -- centred on the picture. LEDs whose centre falls outside
	// the picture are lit black.
	//---------------------------------------------------------------------
	const double ledsAcross = width / static_cast< double >( pitch );
	const double ledsDown   = height / static_cast< double >( pitch );
	const int cabinetsX     = std::max( 1, static_cast< int >( std::lround( ledsAcross / cabinetW ) ) );
	const int cabinetsY     = std::max( 1, static_cast< int >( std::lround( ledsDown / cabinetH ) ) );
	const int wallW         = std::min( cabinetsX * cabinetW, kMaxWallSide );
	const int wallH         = std::min( cabinetsY * cabinetH, kMaxWallSide );
	const float originX     = static_cast< float >( ( width - wallW * static_cast< double >( pitch ) ) * 0.5 );
	const float originY     = static_cast< float >( ( height - wallH * static_cast< double >( pitch ) ) * 0.5 );

	//---------------------------------------------------------------------
	// Buffers. Every Ensure() happens here, before anything binds a texture:
	// allocating one leaves the active unit bound to nothing, and the symptom
	// of getting the order wrong is correct on every frame except the one
	// that allocates.
	//---------------------------------------------------------------------
	const bool allocated = picture.Ensure( width, height, GL_RGBA8, PassBuffer::Sampling::Mipmapped )
	                       && wall.Ensure( wallW, wallH, GL_RGBA32F, PassBuffer::Sampling::Mipmapped )
	                       && ( !bayer || sensor.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Nearest ) );
	if( !allocated )
	{
		diag::error( "could not allocate the pass buffers: picture " + std::to_string( width ) + "x"
		             + std::to_string( height ) + ", wall " + std::to_string( wallW ) + "x" + std::to_string( wallH )
		             + " - try a larger Pitch" );
		return FF_FAIL;
	}

	if( rowTableRows != height )
	{
		glBindTexture( GL_TEXTURE_2D, rowTable );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, height, 1, 0, GL_RGBA, GL_FLOAT, nullptr );
		glBindTexture( GL_TEXTURE_2D, 0 );
		rowTableRows = height;
		rowWindows.assign( static_cast< size_t >( height ) * 4, 0.0f );
	}

	//---------------------------------------------------------------------
	// Time, in sub-periods, per row, in double.
	//
	// The camera's frame n begins at n / FrameRate of real time, and frame n
	// is the host's frame n, so camera time is host time scaled by the ratio
	// of the two rates. The wall's pulse train has run since time zero at
	// Refresh; only the phase at this frame matters, so the whole number of
	// sub-periods elapsed is dropped here, in double, and every row's window
	// is then a small number. Resolume's clock has been seen at 499,217 s,
	// where a float resolves to 0.03 s; nothing absolute crosses into GLSL.
	//---------------------------------------------------------------------
	const double subPeriod   = 1.0 / ( refreshHz * subPeriods );
	const double cameraTime  = now / ( frameSeconds * frameRate );
	const double frameStart  = cameraTime / subPeriod + refreshPhase * subPeriods;
	const double phase0      = frameStart - std::floor( frameStart );
	const double rowStep     = ( readout / height ) / subPeriod;
	const double windowLen   = shutter / subPeriod;

	for( int r = 0; r < height; ++r )
	{
		const double u0 = phase0 + r * rowStep;
		const double u1 = u0 + windowLen;
		float* entry    = rowWindows.data() + static_cast< size_t >( r ) * 4;
		split( u0, entry[ 0 ], entry[ 1 ] );
		split( u1, entry[ 2 ], entry[ 3 ] );
	}
	glBindTexture( GL_TEXTURE_2D, rowTable );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, height, 1, GL_RGBA, GL_FLOAT, rowWindows.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );

	//---------------------------------------------------------------------
	// 1. Copy, with a mip chain.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( picture.GetGLID(), ScopedFBOBinding::RB_REVERT );
		picture.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		copyShader.Set( "HalfTexel", 0.5f / static_cast< float >( width ), 0.5f / static_cast< float >( height ) );
		quad.Draw();
	}
	picture.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 2. The wall.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( wall.GetGLID(), ScopedFBOBinding::RB_REVERT );
		wall.ResizeViewPort();
		ScopedShaderBinding shader( wallShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.TextureID() );

		wallShader.Set( "Picture", 0 );
		wallShader.Set( "PictureSize", static_cast< float >( width ), static_cast< float >( height ) );
		glUniform2i( wallShader.FindUniform( "WallSize" ), wallW, wallH );
		wallShader.Set( "Pitch", pitch );
		wallShader.Set( "WallOrigin", originX, originY );
		glUniform2i( wallShader.FindUniform( "Cabinet" ), cabinetW, cabinetH );
		wallShader.Set( "ModuleRows", moduleRows );
		wallShader.Set( "Seed", seed );
		wallShader.Set( "FaultRate", faultRate );
		wallShader.Set( "DeadAmount", params[ PT_DEAD ] );
		wallShader.Set( "DimAmount", params[ PT_DIM ] );
		wallShader.Set( "DeadRowAmount", params[ PT_DEAD_ROW ] );
		wallShader.Set( "BinShiftAmount", params[ PT_BIN_SHIFT ] );
		quad.Draw();
	}
	wall.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 3. The sensor. To the host when there is no mosaic, else to a buffer.
	//
	// The sub-box count is 1 wherever the aperture is axis-aligned, because
	// then a single analytic box IS the exact integral; under rotation the
	// rotated square is approximated by nine smaller axis-aligned ones.
	// Focus forces it back to 1: the disc taps cost enough, and the blur
	// hides what the sub-boxes would have added.
	//---------------------------------------------------------------------
	const bool rotated = rotation != 0.0f;
	const int subDiv   = ( rotated && focus <= 0.0f ) ? 3 : 1;
	const int taps     = focus > 0.0f ? kFocusTaps : 1;

	auto drawSensor = [ & ]( float mixAmount ) {
		ScopedShaderBinding shader( sensorShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding wallTexture( wall.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding tableTexture( rowTable );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding sourceTexture( input.Handle );

		sensorShader.Set( "Wall", 0 );
		sensorShader.Set( "RowTable", 1 );
		sensorShader.Set( "Source", 2 );
		sensorShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		glUniform2i( sensorShader.FindUniform( "Size" ), width, height );
		glUniform2i( sensorShader.FindUniform( "WallSize" ), wallW, wallH );
		sensorShader.Set( "Scale", scale );
		sensorShader.Set( "CosR", std::cos( rotation ) );
		sensorShader.Set( "SinR", std::sin( rotation ) );
		sensorShader.Set( "SubDiv", subDiv );
		sensorShader.Set( "Olpf", olpf ? 1 : 0 );
		sensorShader.Set( "FocusRadius", focus );
		sensorShader.Set( "FocusTaps", taps );
		sensorShader.Set( "EmitHalf", 0.5f * std::sqrt( fill ) );
		sensorShader.Set( "Layout", layout );
		sensorShader.Set( "Scan", scanGroups );
		sensorShader.Set( "GreyLevels", static_cast< float >( drive::greyLevels( greyBits ) ) );
		sensorShader.Set( "WindowLen", static_cast< float >( windowLen ) );
		sensorShader.Set( "OverlapDetune", overlapDetune );
		sensorShader.Set( "MixAmount", mixAmount );
		quad.Draw();
	};

	if( !bayer )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
		drawSensor( params[ PT_MIX ] );
		return FF_SUCCESS;
	}

	{
		ScopedFBOBinding fbo( sensor.GetGLID(), ScopedFBOBinding::RB_REVERT );
		sensor.ResizeViewPort();
		drawSensor( 1.0f );
	}

	//---------------------------------------------------------------------
	// 4. The mosaic and the demosaic, straight to the host.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( demosaicShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding sensorTexture( sensor.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding sourceTexture( input.Handle );

		demosaicShader.Set( "Sensor", 0 );
		demosaicShader.Set( "Source", 1 );
		demosaicShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		glUniform2i( demosaicShader.FindUniform( "Size" ), width, height );
		demosaicShader.Set( "BayerPhase", bayerPhase );
		demosaicShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Pitch::DeInitGL()
{
	copyShader.FreeGLResources();
	wallShader.FreeGLResources();
	sensorShader.FreeGLResources();
	demosaicShader.FreeGLResources();
	quad.Release();
	picture.Destroy();
	wall.Destroy();
	sensor.Destroy();
	if( rowTable != 0 )
	{
		glDeleteTextures( 1, &rowTable );
		rowTable = 0;
	}
	rowTableRows = 0;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Pitch::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Pitch::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Pitch::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Pitch::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Pitch::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Pitch::SetOverlapDetuneForTest( float detune )
{
	overlapDetune = detune;
}

void Pitch::SetBayerPhaseForTest( int phase )
{
	bayerPhase = phase == 0 ? 0 : 1;
}
