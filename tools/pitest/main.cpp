/**
	pitest -- render Pitch offline, and measure what its two samplings do.

	Where a moire fringe lands, how deep a scan band is, and what a shutter
	that is a whole number of refresh periods does to it are facts with one
	right answer each. Every check here renders a synthetic scene through the
	REAL plugin class in a headless GL context, reads the pixels back, and
	measures -- by DFT, by frame mean, by exact comparison -- rather than
	eyeballing.

		pitest --out /tmp/frame.png     a picture, on the test card
		pitest --list                   every parameter, its kind and default
		pitest --moire                  the fringe period is 1 / | s - round( s ) | pixels
		pitest --bands                  the band period is the pulse period over Tr / H;
		                                a shutter of whole periods has no bands
		pitest --pwm                    band depth against the closed-form overlap,
		                                rising as grey falls, falling under Scrambled
		pitest --energy                 the optics conserve light: mean = fill x level
		pitest --faults                 a dead cabinet is exactly black, exactly aligned,
		                                and reproduces bit-exactly from its seed
		pitest --identity               Pitch 1, fill 1, whole-period shutter, no Bayer
		                                returns the input byte for byte
		pitest --bayer                  the mosaic samples each colour where it says
		pitest --negative               the checks above can FAIL: a detuned overlap
		                                integral and a swapped Bayer phase are caught
		pitest --bench                  the render cost
		pitest --pipe                   raw frames in, raw frames out

	Every check drives a synthetic 60 fps clock through `SetTime`, so one host
	frame is exactly 16.667 ms. Run each at two rasters at least -- the one you
	develop at and 320x180, which is what CI uses; a check that holds at one
	raster only is a check that was fitted to it.

	`--script` is a plain text file of `frame  Parameter Name  value` lines,
	the same format as the fleet's other harnesses. `--pipe` takes the fleet's
	frame format:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | pitest --pipe --size 1920x1080 [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Controls.h"
#include "Drive.h"
#include "Pitch.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace pitch;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Sources. Rows are top-first in every source, the way a picture is, and
// flipped on the way into GL. The checks read back through the same flip,
// so "row 0" is the top of the picture everywhere in this file.
//---------------------------------------------------------------------------
enum class Source
{
	Card,
	Flat,
	White
};

Source sourceFromName( const std::string& name )
{
	if( name == "flat" )
		return Source::Flat;
	if( name == "white" )
		return Source::White;
	return Source::Card;
}

/// The test card, adapted from readout's: a graded surround, six saturated
/// bars, a smooth ramp, a fine checker, a slow bar and a disc on a Lissajous
/// path so consecutive frames differ, plus a row of dark grey patches across
/// the top where the low-grey breakup has something to show on.
std::vector< unsigned char > buildCard( int width, int height, int frame )
{
	std::vector< unsigned char > card( static_cast< size_t >( width ) * height * 4 );

	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );
	const float t = static_cast< float >( frame );

	const float slowX = 0.85f * w - std::fmod( t * 6.0f * ( w / 1280.0f ), 0.70f * w );
	const float slowW = 0.03f * w;

	const float discX = 0.5f * w + 0.30f * w * std::sin( t * 0.11f );
	const float discY = 0.50f * h + 0.20f * h * std::sin( t * 0.077f + 1.1f );
	const float discR = 0.06f * h;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			float r = 0.30f + 0.10f * v;
			float g = 0.30f + 0.10f * v;
			float b = 0.34f + 0.10f * v;

			if( v < 0.125f )
			{
				//Eight dark patches, 2% to 16% grey.
				const int patch = std::min( 7, static_cast< int >( u * 8.0f ) );
				r = g = b = 0.02f * static_cast< float >( patch + 1 );
			}
			else if( v > 0.875f )
			{
				static const float bars[ 6 ][ 3 ] = {
					{ 1.0f, 0.1f, 0.1f }, { 0.1f, 1.0f, 0.1f }, { 0.1f, 0.1f, 1.0f },
					{ 0.1f, 1.0f, 1.0f }, { 1.0f, 0.1f, 1.0f }, { 1.0f, 1.0f, 0.1f }
				};
				const int bar = std::min( 5, static_cast< int >( u * 6.0f ) );
				r             = bars[ bar ][ 0 ];
				g             = bars[ bar ][ 1 ];
				b             = bars[ bar ][ 2 ];
			}
			else if( v > 0.75f )
			{
				r = g = b = 0.1f + 0.8f * u;
			}
			else if( u < 0.25f && v < 0.35f )
			{
				const bool on = ( ( x / 4 ) + ( y / 4 ) ) % 2 == 0;
				r = g = b = on ? 0.85f : 0.10f;
			}

			const float px = static_cast< float >( x ) + 0.5f;
			if( std::fabs( px - slowX ) < slowW * 0.5f && v > 0.15f && v < 0.75f )
			{
				r = 0.95f;
				g = 0.65f;
				b = 0.20f;
			}

			const float dx   = px - discX;
			const float dy   = ( static_cast< float >( y ) + 0.5f ) - discY;
			const float dist = std::sqrt( dx * dx + dy * dy );
			if( dist < discR )
			{
				const float edge = std::min( 1.0f, ( discR - dist ) / ( discR * 0.2f ) );
				r                = r + ( 0.2f - r ) * edge;
				g                = g + ( 0.7f - g ) * edge;
				b                = b + ( 0.9f - b ) * edge;
			}

			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ i + 0 ]  = static_cast< unsigned char >( std::clamp( r, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 1 ]  = static_cast< unsigned char >( std::clamp( g, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 2 ]  = static_cast< unsigned char >( std::clamp( b, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 3 ]  = 255;
		}
	}

	return card;
}

std::vector< unsigned char > buildFlat( int width, int height, int code )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4 );
	const unsigned char value = static_cast< unsigned char >( std::clamp( code, 0, 255 ) );
	for( size_t i = 0; i < image.size(); i += 4 )
	{
		image[ i + 0 ] = image[ i + 1 ] = image[ i + 2 ] = value;
		image[ i + 3 ]                                   = 255;
	}
	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Pitch::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Pitch& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Pitch::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range. An integer's real range is real.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Pitch& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Pitch& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	const int index         = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}

	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( value.c_str(), nullptr ) );
	return true;
}

bool set( Pitch& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Pitch plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;
	/// Render into an RGBA32F framebuffer rather than RGBA8. The physics
	/// checks read float so their tolerances can be float-derived; the
	/// identity check reads 8-bit because 8-bit is the claim.
	bool floatOutput = false;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct   = {};
	FFGLTextureStruct* inputs[ 1 ]  = { nullptr };
	ProcessOpenGLStruct process     = {};

	bool begin( int w, int h )
	{
		width  = w;
		height = h;

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}

		sourceTexture = makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
		return true;
	}

	/// Render one frame of `pixels` (top row first) at frame number `frame`.
	bool render( int frame, const std::vector< unsigned char >& pixels )
	{
		const double seconds = static_cast< double >( frame ) / fps;

		//A synthetic clock, and it has to be synthetic: left to the wall
		//clock the harness renders a hundred frames in a few milliseconds,
		//so no time passes and the bands never roll. The unit is declared
		//rather than inferred.
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( seconds );

		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
		return ok;
	}

	/// The output, top row first, 8-bit.
	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	/// The output, top row first, as floats.
	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}
};

//---------------------------------------------------------------------------
// The baseline every check starts from: a wall of one LED per source pixel
// that tiles the picture in whole cabinets, a single scan group, sixteen grey
// bits, a 1000 Hz refresh with a shutter of exactly one period, a 20 ms
// readout, one LED per sensor pixel, no rotation, no blur, no mosaic, no
// faults. Each check then moves the one or two things it is about.
//---------------------------------------------------------------------------

/// A divisor of n nearest to `target` within the cabinet range, so the wall
/// is exactly the picture. Falls back to `target` for a raster with no such
/// divisor, and the check that asked will say so.
int cabinetSide( int n, int target )
{
	int best = -1;
	for( int d = 8; d <= 256; ++d )
		if( n % d == 0 && ( best < 0 || std::abs( d - target ) < std::abs( best - target ) ) )
			best = d;
	return best < 0 ? target : best;
}

constexpr double kRefreshHz  = 1000.0;
constexpr double kReadoutSec = 0.020;

void baseline( Pitch& p, int width, int height )
{
	set( p, "Pitch", controls::PitchParam( 1.0f ) );
	set( p, "Fill Factor", controls::FillParam( 1.0f ) );
	set( p, "LED Layout", 0.0f );
	set( p, "Cabinet W", static_cast< float >( cabinetSide( width, 32 ) ) );
	set( p, "Cabinet H", static_cast< float >( cabinetSide( height, 36 ) ) );
	set( p, "Module Rows", 6.0f );
	set( p, "Refresh", controls::RefreshParam( static_cast< float >( kRefreshHz ) ) );
	set( p, "Scan Ratio", 0.0f );
	set( p, "Grey Bits", 16.0f );
	set( p, "PWM", 0.0f );
	set( p, "Refresh Phase", 0.0f );
	set( p, "Camera Scale", controls::CameraScaleParam( 1.0f ) );
	set( p, "Rotation", 0.5f );
	set( p, "Focus", 0.0f );
	set( p, "OLPF", 0.0f );
	set( p, "Shutter", controls::ShutterParam( static_cast< float >( 1.0 / kRefreshHz ) ) );
	set( p, "Readout", controls::ReadoutParam( static_cast< float >( kReadoutSec ) ) );
	set( p, "Frame Rate", 7.0f );
	set( p, "Bayer On", 0.0f );
	set( p, "Fault Rate", 0.0f );
	set( p, "Mix", 1.0f );
}

/// The frames rendered before a check reads back. Several rather than one so
/// the clock-unit machinery and the frame-period smoothing have settled, and
/// so the phase at the frame read is not the phase at load.
constexpr int kSettleFrames = 6;

//---------------------------------------------------------------------------
// Measurements.
//---------------------------------------------------------------------------

/// A row of one channel across [x0, x1).
std::vector< double > rowProfile( const std::vector< float >& image, int width, int row, int x0, int x1, int channel )
{
	std::vector< double > out;
	for( int x = x0; x < x1; ++x )
		out.push_back( image[ ( static_cast< size_t >( row ) * width + x ) * 4 + channel ] );
	return out;
}

/// A column of one channel down every row.
std::vector< double > columnProfile( const std::vector< float >& image, int width, int height, int column, int channel )
{
	std::vector< double > out;
	for( int y = 0; y < height; ++y )
		out.push_back( image[ ( static_cast< size_t >( y ) * width + column ) * 4 + channel ] );
	return out;
}

double minimum( const std::vector< double >& v )
{
	return *std::min_element( v.begin(), v.end() );
}

double maximum( const std::vector< double >& v )
{
	return *std::max_element( v.begin(), v.end() );
}

double mean( const std::vector< double >& v )
{
	double sum = 0.0;
	for( double x : v )
		sum += x;
	return sum / static_cast< double >( v.size() );
}

/// Michelson contrast, ( max - min ) / ( max + min ).
double contrast( const std::vector< double >& v )
{
	const double lo = minimum( v ), hi = maximum( v );
	return hi + lo > 0.0 ? ( hi - lo ) / ( hi + lo ) : 0.0;
}

/// The DFT bin with the most energy, ignoring DC. Period = length / bin.
int dftPeak( const std::vector< double >& signal )
{
	const size_t n = signal.size();
	const double m = mean( signal );
	int best       = 0;
	double bestMag = -1.0;
	for( size_t k = 1; k <= n / 2; ++k )
	{
		double re = 0.0, im = 0.0;
		for( size_t i = 0; i < n; ++i )
		{
			const double a = -2.0 * M_PI * static_cast< double >( k * i ) / static_cast< double >( n );
			re += ( signal[ i ] - m ) * std::cos( a );
			im += ( signal[ i ] - m ) * std::sin( a );
		}
		const double mag = re * re + im * im;
		if( mag > bestMag )
		{
			bestMag = mag;
			best    = static_cast< int >( k );
		}
	}
	return best;
}

/// The widest hole in the set of phases the rows actually visit, when row r
/// sits at phase frac( r * step ). The phases of a rational step form a
/// lattice; an irrational one fills in; either way this is the gap that has
/// to be narrower than a plateau for the plateau to be sampled.
double widestPhaseGap( double step, int rows )
{
	std::vector< double > phases;
	for( int r = 0; r < rows; ++r )
	{
		const double u = r * step;
		phases.push_back( u - std::floor( u ) );
	}
	std::sort( phases.begin(), phases.end() );
	double widest = phases.front() + 1.0 - phases.back();
	for( size_t i = 1; i < phases.size(); ++i )
		widest = std::max( widest, phases[ i ] - phases[ i - 1 ] );
	return widest;
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAILED";
}

//---------------------------------------------------------------------------
// --identity
//
// Pitch 1, fill 1, a shutter of exactly one refresh period, one LED per
// pixel, no rotation, no blur, no mosaic: every stage is an identity and the
// input must come back BYTE FOR BYTE. The negative control drops the fill to
// a half, which must not.
//
// Why bitwise and not "within one ULP": the arithmetic is within a few ULPs
// -- the copy is exact at texel centres, the pixel box is exactly the LED
// cell, the coverage is 1.0 exactly, the PWM factor is a whole window over a
// whole window and lands within an ULP of 1 -- and the output is 8-bit. A
// value k/255 * ( 1 +- 1e-6 ) rounds back to k unless it sat within 1e-6 of
// a half code, and no k/255 does. So the 8-bit result is exact without
// relying on any float being exactly what it should.
//---------------------------------------------------------------------------
int runIdentity( int width, int height, bool quiet = false )
{
	struct Case
	{
		const char* name;
		float fill;
		bool expectExact;
	};
	const Case cases[] = {
		{ "fill 1.0 (must be exact)      ", 1.0f, true },
		{ "fill 0.5 (the negative control)", 0.5f, false },
	};

	int failures = 0;
	for( const Case& c : cases )
	{
		Session session;
		baseline( session.plugin, width, height );
		set( session.plugin, "Fill Factor", controls::FillParam( c.fill ) );
		if( !session.begin( width, height ) )
			return 1;

		std::vector< unsigned char > card;
		for( int frame = 0; frame < kSettleFrames; ++frame )
		{
			card = buildCard( width, height, frame );
			if( !session.render( frame, card ) )
				return 1;
		}
		const std::vector< unsigned char > out = session.readBack();
		session.end();

		int worst = 0;
		for( size_t i = 0; i < out.size(); ++i )
			worst = std::max( worst, std::abs( static_cast< int >( out[ i ] ) - static_cast< int >( card[ i ] ) ) );

		const bool ok = c.expectExact ? worst == 0 : worst > 0;
		if( !quiet )
			std::printf( "identity %s: worst %3d/255  %s\n", c.name, worst, verdict( ok ) );
		if( !ok )
			++failures;
	}

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "identity: Pitch 1, fill 1, a whole-period shutter and no Bayer return the input byte for byte"
		                                    : "identity: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --energy
//
// The optics conserve light. With the wall well inside the frame, a shutter
// of one whole period (so every LED reports its own level), and Focus on,
// the mean over the frame equals fill x level x ( wall area / frame area ).
//
// Why that is exact and not approximate: every focus tap shifts the whole
// lattice of pixel boxes by the same offset, and a shifted lattice of boxes
// still tiles the plane, so each tap integrates every emitter rectangle
// exactly once. The tolerance is float: a few ULPs per pixel on values near
// 0.25, accumulated in double. 2e-5 relative is a hundred times that.
//---------------------------------------------------------------------------
int runEnergy( int width, int height, float detune = 0.0f, bool quiet = false )
{
	const float fill     = 0.5f;
	const int code       = 128;
	const double level   = drive::quantise( code / 255.0, 16 );
	const float scale    = 1.5f;
	const double wallW   = width;//cabinets tile the picture exactly
	const double wallH   = height;
	const double expected = fill * level * ( wallW / scale ) * ( wallH / scale ) / ( static_cast< double >( width ) * height );
	const double tolerance = 2e-5 * expected;

	struct Case
	{
		const char* name;
		float focus;
	};
	const Case cases[] = {
		{ "focus 0   ", 0.0f },
		{ "focus 1.5 ", 1.5f },
	};

	int failures = 0;
	for( const Case& c : cases )
	{
		Session session;
		session.floatOutput = true;
		baseline( session.plugin, width, height );
		set( session.plugin, "Fill Factor", controls::FillParam( fill ) );
		set( session.plugin, "Camera Scale", controls::CameraScaleParam( scale ) );
		set( session.plugin, "Focus", controls::FocusParam( c.focus ) );
		session.plugin.SetOverlapDetuneForTest( detune );
		if( !session.begin( width, height ) )
			return 1;

		const std::vector< unsigned char > flat = buildFlat( width, height, code );
		for( int frame = 0; frame < kSettleFrames; ++frame )
			if( !session.render( frame, flat ) )
				return 1;
		const std::vector< float > out = session.readBackFloat();
		session.end();

		double sum = 0.0;
		for( size_t i = 0; i < out.size(); i += 4 )
			sum += out[ i ];
		const double got = sum / static_cast< double >( static_cast< size_t >( width ) * height );

		const bool ok = std::fabs( got - expected ) <= tolerance;
		if( !quiet )
			std::printf( "energy %s: frame mean %.7f, expected fill x level x area = %.7f (off by %.2e, tolerance %.2e)  %s\n",
			             c.name, got, expected, got - expected, tolerance, verdict( ok ) );
		if( !ok )
			++failures;
	}

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "energy: the optics conserve light, blurred or not" : "energy: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --moire
//
// A flat white wall with a fill factor of a half, no rotation, no mosaic,
// seen at s LEDs per sensor pixel. The LED lattice has spatial frequency s
// cycles per pixel; sampled once per pixel it aliases to | s - round( s ) |,
// so the fringe period is 1 / | s - round( s ) | pixels. That is the beat
// between two FREQUENCIES, which is why it is written in s and not in the
// LED's period 1/s: at s = 0.8 the fringe is 5 px, and 1 / | 1.25 - 1 |
// would have said 4.
//
// Measured by DFT over the central half of the middle row, whose length is
// chosen so the expected period divides it and the peak lands on a bin.
// An integer s has no fringe: every pixel holds exactly one cell.
//
// Then Focus: a disc of radius rho has the MTF 2 J1( 2 pi rho nu ) /
// ( 2 pi rho nu ), which at nu = 0.8 and rho = 1.5 is 0.037. So a blur of
// 1.5 px leaves under 4% of the fundamental; the floor of 0.05 on the
// contrast is that, with room for the 32-tap disc not being a perfect one.
//---------------------------------------------------------------------------
int runMoire( int width, int height, bool quiet = false )
{
	struct Case
	{
		const char* name;
		float scale;
		float focus;
		bool fringe;
	};
	const Case cases[] = {
		{ "s = 0.80          ", 0.80f, 0.0f, true },
		{ "s = 1.25          ", 1.25f, 0.0f, true },
		{ "s = 1.00          ", 1.00f, 0.0f, false },
		{ "s = 0.80, focus 1.5", 0.80f, 1.5f, false },
	};

	const int x0 = width / 4;
	const int x1 = x0 + width / 2;
	const int length = x1 - x0;

	int failures = 0;
	double sharpContrast = 0.0;
	for( const Case& c : cases )
	{
		Session session;
		session.floatOutput = true;
		baseline( session.plugin, width, height );
		set( session.plugin, "Fill Factor", controls::FillParam( 0.5f ) );
		set( session.plugin, "Camera Scale", controls::CameraScaleParam( c.scale ) );
		set( session.plugin, "Focus", controls::FocusParam( c.focus ) );
		if( !session.begin( width, height ) )
			return 1;

		const std::vector< unsigned char > white = buildFlat( width, height, 255 );
		for( int frame = 0; frame < kSettleFrames; ++frame )
			if( !session.render( frame, white ) )
				return 1;
		const std::vector< float > out = session.readBackFloat();
		session.end();

		const std::vector< double > profile = rowProfile( out, width, height / 2, x0, x1, 0 );
		const double con                    = contrast( profile );
		const double alias                  = std::fabs( c.scale - std::round( c.scale ) );
		const int expectBin                 = static_cast< int >( std::lround( length * alias ) );
		const int bin                       = con > 1e-4 ? dftPeak( profile ) : 0;

		bool ok;
		if( c.fringe )
		{
			ok = bin == expectBin && con > 0.05;
			sharpContrast = std::max( sharpContrast, con );
			if( !quiet )
				std::printf( "moire %s: peak bin %d of %d = period %.2f px, expected bin %d = %.2f px; contrast %.4f  %s\n",
				             c.name, bin, length, bin > 0 ? static_cast< double >( length ) / bin : 0.0, expectBin,
				             alias > 0.0 ? 1.0 / alias : 0.0, con, verdict( ok ) );
		}
		else if( c.focus > 0.0f )
		{
			ok = con < 0.05 && con < 0.3 * sharpContrast;
			if( !quiet )
				std::printf( "moire %s: contrast %.4f against %.4f sharp (floor 0.05 from the disc MTF at the fundamental)  %s\n",
				             c.name, con, sharpContrast, verdict( ok ) );
		}
		else
		{
			ok = con < 1e-4;
			if( !quiet )
				std::printf( "moire %s: contrast %.2e (an integer s has no fringe)  %s\n", c.name, con, verdict( ok ) );
		}
		if( !ok )
			++failures;
	}

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "moire: the fringe period is 1 / | s - round( s ) | pixels, and Focus kills it" : "moire: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --bands
//
// A uniform grey wall, one scan group, read out over Tr with a shutter E.
// Row r's window starts Tr / H later than row r-1's, so the band pattern
// down the frame is the pulse train's overlap function sampled at that step:
// its period in rows is P / ( Tr / H ). With P = 1 ms and Tr = 20 ms the
// period is H / 20 rows at every raster, so the DFT peak is bin 20.
//
// When E is a whole number of periods the overlap is the same for every
// phase and the depth is zero -- to float precision, because the whole
// parts are subtracted as small integers and the fractions are in [0,1).
// The tolerance of 1e-4 on values near 0.5 is a hundred ULPs.
//
// The depth when E is 1.25 periods is checked against the closed form too,
// which --pwm does more thoroughly.
//---------------------------------------------------------------------------
int runBands( int width, int height, float detune = 0.0f, bool quiet = false )
{
	const int code     = 128;
	const double q     = drive::quantise( code / 255.0, 16 );
	const int column   = width / 2;

	struct Case
	{
		const char* name;
		double periods;
		bool banded;
	};
	const Case cases[] = {
		{ "E = 1.25 P", 1.25, true },
		{ "E = 2.00 P", 2.00, false },
	};

	int failures = 0;
	for( const Case& c : cases )
	{
		Session session;
		session.floatOutput = true;
		baseline( session.plugin, width, height );
		set( session.plugin, "Shutter", controls::ShutterParam( static_cast< float >( c.periods / kRefreshHz ) ) );
		session.plugin.SetOverlapDetuneForTest( detune );
		if( !session.begin( width, height ) )
			return 1;

		const std::vector< unsigned char > flat = buildFlat( width, height, code );
		for( int frame = 0; frame < kSettleFrames; ++frame )
			if( !session.render( frame, flat ) )
				return 1;
		const std::vector< float > out = session.readBackFloat();
		session.end();

		const std::vector< double > profile = columnProfile( out, width, height, column, 0 );
		const double depth                  = maximum( profile ) - minimum( profile );
		const double step                   = ( kReadoutSec / height ) * kRefreshHz;//periods per row

		bool ok;
		if( c.banded )
		{
			double lo, hi;
			drive::overlapRange( c.periods, q, lo, hi );
			const double expectDepth = ( hi - lo ) / c.periods;
			const int bin            = dftPeak( profile );
			const int expectBin      = static_cast< int >( std::lround( height * step ) );
			//The extremes are plateaus; a plateau wider than the widest hole in
			//the visited phases is sampled exactly, so the tolerance is float.
			const double plateau = std::min( std::fabs( q - 0.25 ), std::fabs( 1.0 - q - 0.25 ) );
			const double gap     = widestPhaseGap( step, height );
			ok = bin == expectBin && std::fabs( depth - expectDepth ) <= 1e-4 && plateau >= gap;
			if( !quiet )
				std::printf( "bands %s: peak bin %d = period %.2f rows, expected bin %d = %.2f rows; depth %.5f, closed form %.5f; plateau %.3f >= phase gap %.3f  %s\n",
				             c.name, bin, static_cast< double >( height ) / std::max( 1, bin ), expectBin, 1.0 / step, depth,
				             expectDepth, plateau, gap, verdict( ok ) );
		}
		else
		{
			ok = depth <= 1e-4;
			if( !quiet )
				std::printf( "bands %s: depth %.2e (must be 0 to a hundred ULPs: the shutter is a whole number of periods)  %s\n",
				             c.name, depth, verdict( ok ) );
		}
		if( !ok )
			++failures;
	}

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "bands: the band period is P / ( Tr / H ) rows, and a whole-period shutter has none"
		                                    : "bands: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --pwm
//
// Band depth against the closed-form overlap, at E = 1.3 periods, for three
// grey levels under Conventional PWM and one under Scrambled.
//
// The closed form: with E = n + r periods and a pulse of width w = q (one
// group), the window always holds n whole pulses and catches between
// max( 0, w + r - 1 ) and min( w, r ) of one more. The relative depth is
// therefore P / E for any pulse shorter than both r and 1 - r, and falls
// once the pulse is long enough to be caught whichever way the window sits
// -- which is the low-grey breakup: the dark level bands worst. Scrambled
// splits the pulse across 16 sub-periods, so E is 20.8 sub-periods and the
// depth falls by about that factor.
//
// Measured extremes are exact to float when the plateaus of the overlap
// function are wider than the row step, which is asserted.
//---------------------------------------------------------------------------
int runPwm( int width, int height, bool quiet = false )
{
	const double periods = 1.3;
	const int column     = width / 2;

	struct Case
	{
		const char* name;
		int code;
		int pwm;
	};
	const Case cases[] = {
		{ "conventional, 10%", 26, 0 },
		{ "conventional, 50%", 128, 0 },
		{ "conventional, 90%", 230, 0 },
		{ "scrambled,    50%", 128, 1 },
	};

	int failures = 0;
	double relDepth[ 4 ] = {};
	int n = 0;
	for( const Case& c : cases )
	{
		Session session;
		session.floatOutput = true;
		baseline( session.plugin, width, height );
		set( session.plugin, "Shutter", controls::ShutterParam( static_cast< float >( periods / kRefreshHz ) ) );
		set( session.plugin, "PWM", static_cast< float >( c.pwm ) );
		if( !session.begin( width, height ) )
			return 1;

		const std::vector< unsigned char > flat = buildFlat( width, height, c.code );
		for( int frame = 0; frame < kSettleFrames; ++frame )
			if( !session.render( frame, flat ) )
				return 1;
		const std::vector< float > out = session.readBackFloat();
		session.end();

		const std::vector< double > profile = columnProfile( out, width, height, column, 0 );
		const double depth                  = maximum( profile ) - minimum( profile );
		const double got                    = mean( profile );

		const int sub        = c.pwm == 1 ? controls::kScrambledSubPeriods : 1;
		const double e       = periods * sub;
		const double q       = drive::quantise( c.code / 255.0, 16 );
		double lo, hi;
		drive::overlapRange( e, q, lo, hi );
		const double expectDepth = ( hi - lo ) / e;
		//Plateau widths of the overlap function, in sub-periods: the top one
		//is | w - r |, the bottom one is | 1 - w - r |.
		const double r       = e - std::floor( e );
		const double plateau = std::min( std::fabs( q - r ), std::fabs( 1.0 - q - r ) );
		const double step    = ( kReadoutSec / height ) * kRefreshHz * sub;
		//A plateau wider than the widest hole in the visited phases holds a
		//sample, so the measured extreme is the true one.
		const double spacing = widestPhaseGap( step, height );

		//The mean over the rows is the mean of a piecewise-linear periodic
		//function over a LATTICE of phases, not over all of them, and the two
		//differ by up to the total variation over a period (twice the depth)
		//times half the lattice spacing. That bound is the tolerance.
		const double meanBound = expectDepth * spacing;
		const bool ok = std::fabs( depth - expectDepth ) <= 1e-4 && std::fabs( got - q ) <= meanBound && plateau >= spacing;
		relDepth[ n++ ] = depth / q;
		if( !quiet )
			std::printf( "pwm %s: depth %.5f, closed form %.5f; mean %.5f, level %.5f (lattice bound %.5f); relative depth %.3f; plateau %.3f >= phase spacing %.3f  %s\n",
			             c.name, depth, expectDepth, got, q, meanBound, depth / q, plateau, spacing, verdict( ok ) );
		if( !ok )
			++failures;
	}

	const bool monotone = relDepth[ 0 ] > relDepth[ 1 ] && relDepth[ 1 ] > relDepth[ 2 ];
	const bool scrambled = relDepth[ 3 ] < relDepth[ 1 ];
	if( !quiet )
	{
		std::printf( "pwm: relative depth rises as grey falls under Conventional: %.3f > %.3f > %.3f  %s\n", relDepth[ 0 ],
		             relDepth[ 1 ], relDepth[ 2 ], verdict( monotone ) );
		std::printf( "pwm: and falls under Scrambled at the same level: %.4f < %.4f  %s\n", relDepth[ 3 ], relDepth[ 1 ],
		             verdict( scrambled ) );
	}
	if( !monotone )
		++failures;
	if( !scrambled )
		++failures;

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "pwm: band depth follows the closed-form overlap, worst at low grey, spread out by Scrambled"
		                                    : "pwm: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --faults
//
// One LED per pixel, one pixel per LED, a white wall, cabinets that tile the
// frame. Every cabinet's pixels are read, and a cabinet must be uniform:
// dead ones exactly 0, the rest exactly 255. A dead row is exactly one row,
// the whole cabinet wide. The same seed twice is the same bytes; another
// seed is not. None of that compares against a CPU copy of the hash -- it
// reads the picture.
//---------------------------------------------------------------------------
int runFaults( int width, int height, bool quiet = false )
{
	const int cabW = cabinetSide( width, 32 );
	const int cabH = cabinetSide( height, 36 );
	if( width % cabW != 0 || height % cabH != 0 )
	{
		if( !quiet )
			std::printf( "faults: no cabinet size tiles %dx%d, so this raster cannot be checked  FAILED\n", width, height );
		return 1;
	}
	const int cabsX = width / cabW;
	const int cabsY = height / cabH;

	auto render = [ & ]( float rate, float dead, float dim, float deadRow, float seed, std::vector< unsigned char >& out ) {
		Session session;
		baseline( session.plugin, width, height );
		set( session.plugin, "Fault Rate", rate );
		set( session.plugin, "Dead", dead );
		set( session.plugin, "Dim", dim );
		set( session.plugin, "Dead Row", deadRow );
		set( session.plugin, "Bin Shift", 0.0f );
		set( session.plugin, "Fault Seed", seed );
		if( !session.begin( width, height ) )
			return false;
		const std::vector< unsigned char > white = buildFlat( width, height, 255 );
		for( int frame = 0; frame < kSettleFrames; ++frame )
			if( !session.render( frame, white ) )
				return false;
		out = session.readBack();
		session.end();
		return true;
	};

	auto pixel = [ & ]( const std::vector< unsigned char >& img, int x, int y ) {
		return img[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
	};

	int failures = 0;

	//--- dead cabinets -----------------------------------------------------
	{
		std::vector< unsigned char > out;
		if( !render( 0.5f, 1.0f, 0.0f, 0.0f, 7.0f, out ) )
			return 1;

		int deadCount = 0, mixed = 0, odd = 0;
		for( int cy = 0; cy < cabsY; ++cy )
			for( int cx = 0; cx < cabsX; ++cx )
			{
				int zeros = 0, whites = 0;
				for( int y = cy * cabH; y < ( cy + 1 ) * cabH; ++y )
					for( int x = cx * cabW; x < ( cx + 1 ) * cabW; ++x )
					{
						const unsigned char v = pixel( out, x, y );
						if( v == 0 )
							++zeros;
						else if( v == 255 )
							++whites;
						else
							++odd;
					}
				if( zeros == cabW * cabH )
					++deadCount;
				else if( whites != cabW * cabH )
					++mixed;
			}
		const bool ok = mixed == 0 && odd == 0 && deadCount > 0 && deadCount < cabsX * cabsY;
		if( !quiet )
			std::printf( "faults dead: %d of %d cabinets (%dx%d LEDs) exactly black, %d mixed, %d pixels neither 0 nor 255  %s\n",
			             deadCount, cabsX * cabsY, cabW, cabH, mixed, odd, verdict( ok ) );
		if( !ok )
			++failures;
	}

	//--- dead rows ---------------------------------------------------------
	{
		std::vector< unsigned char > out;
		if( !render( 0.5f, 0.0f, 0.0f, 1.0f, 7.0f, out ) )
			return 1;

		int rows = 0, bad = 0;
		for( int cy = 0; cy < cabsY; ++cy )
			for( int cx = 0; cx < cabsX; ++cx )
			{
				int blackRows = 0;
				for( int y = cy * cabH; y < ( cy + 1 ) * cabH; ++y )
				{
					int zeros = 0;
					for( int x = cx * cabW; x < ( cx + 1 ) * cabW; ++x )
						if( pixel( out, x, y ) == 0 )
							++zeros;
						else if( pixel( out, x, y ) != 255 )
							++bad;
					if( zeros == cabW )
						++blackRows;
					else if( zeros != 0 )
						++bad;//a partial row is not a dead row
				}
				if( blackRows > 1 )
					++bad;
				rows += blackRows;
			}
		const bool ok = bad == 0 && rows > 0 && rows < cabsX * cabsY;
		if( !quiet )
			std::printf( "faults dead row: %d cabinets with exactly one cabinet-wide black row, %d irregularities  %s\n", rows, bad,
			             verdict( ok ) );
		if( !ok )
			++failures;
	}

	//--- dim ---------------------------------------------------------------
	{
		std::vector< unsigned char > out;
		if( !render( 1.0f, 0.0f, 1.0f, 0.0f, 7.0f, out ) )
			return 1;

		int nonUniform = 0, bright = 0;
		std::vector< int > shades;
		for( int cy = 0; cy < cabsY; ++cy )
			for( int cx = 0; cx < cabsX; ++cx )
			{
				const unsigned char first = pixel( out, cx * cabW, cy * cabH );
				for( int y = cy * cabH; y < ( cy + 1 ) * cabH; ++y )
					for( int x = cx * cabW; x < ( cx + 1 ) * cabW; ++x )
						if( pixel( out, x, y ) != first )
							++nonUniform;
				if( first == 255 )
					++bright;
				shades.push_back( first );
			}
		std::sort( shades.begin(), shades.end() );
		shades.erase( std::unique( shades.begin(), shades.end() ), shades.end() );
		const bool ok = nonUniform == 0 && bright == 0 && shades.size() >= 2;
		if( !quiet )
			std::printf( "faults dim: every cabinet uniform (%d stray pixels), none at full (%d), %zu distinct shades  %s\n",
			             nonUniform, bright, shades.size(), verdict( ok ) );
		if( !ok )
			++failures;
	}

	//--- the seed ------------------------------------------------------------
	{
		std::vector< unsigned char > a, b, c;
		if( !render( 0.5f, 1.0f, 0.0f, 0.0f, 7.0f, a ) || !render( 0.5f, 1.0f, 0.0f, 0.0f, 7.0f, b )
		    || !render( 0.5f, 1.0f, 0.0f, 0.0f, 8.0f, c ) )
			return 1;
		const bool same    = a == b;
		const bool differs = a != c;
		if( !quiet )
			std::printf( "faults seed: seed 7 twice is byte-identical (%s); seed 8 differs (%s)  %s\n", same ? "yes" : "NO",
			             differs ? "yes" : "NO", verdict( same && differs ) );
		if( !( same && differs ) )
			++failures;
	}

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "faults: dead cabinets are exactly black and exactly aligned, and a seed reproduces bit-exactly"
		                                    : "faults: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --bayer
//
// The mosaic samples each colour on its own sub-lattice, and the demosaic
// fills the gaps. So at a red site the demosaiced red IS the sensor's red
// there (to the half-float the sensor buffer rounds to: 2^-11 relative,
// which can move an 8-bit code by one), and on a white wall with a fringe
// the demosaiced red and blue DIFFER, because their sub-lattices sample the
// fringe a pixel apart. With the mosaic's phase swapped the red at a
// nominal red site is an interpolation instead, and the check fails.
//
// Read against the Bayer-OFF picture, which on a white wall has R = G = B
// exactly: the three channels go through identical arithmetic.
//---------------------------------------------------------------------------
int runBayer( int width, int height, int phase = 0, bool quiet = false )
{
	auto render = [ & ]( bool bayer, std::vector< unsigned char >& out ) {
		Session session;
		baseline( session.plugin, width, height );
		set( session.plugin, "Fill Factor", controls::FillParam( 0.5f ) );
		set( session.plugin, "Camera Scale", controls::CameraScaleParam( 0.8f ) );
		set( session.plugin, "Bayer On", bayer ? 1.0f : 0.0f );
		session.plugin.SetBayerPhaseForTest( phase );
		if( !session.begin( width, height ) )
			return false;
		const std::vector< unsigned char > white = buildFlat( width, height, 255 );
		for( int frame = 0; frame < kSettleFrames; ++frame )
			if( !session.render( frame, white ) )
				return false;
		out = session.readBack();
		session.end();
		return true;
	};

	std::vector< unsigned char > off, on;
	if( !render( false, off ) || !render( true, on ) )
		return 1;

	auto at = [ & ]( const std::vector< unsigned char >& img, int x, int y, int c ) {
		return static_cast< int >( img[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ] );
	};

	int grey = 0;//Bayer off must be neutral everywhere
	int worstSite = 0, worstColour = 0;
	for( int y = 1; y < height - 1; ++y )
		for( int x = 1; x < width - 1; ++x )
		{
			if( at( off, x, y, 0 ) != at( off, x, y, 1 ) || at( off, x, y, 0 ) != at( off, x, y, 2 ) )
				++grey;
			//RGGB: red at even/even, blue at odd/odd, green elsewhere.
			const int channel = ( y & 1 ) == 0 ? ( ( x & 1 ) == 0 ? 0 : 1 ) : ( ( x & 1 ) == 0 ? 1 : 2 );
			worstSite         = std::max( worstSite, std::abs( at( on, x, y, channel ) - at( off, x, y, channel ) ) );
			worstColour       = std::max( worstColour, std::abs( at( on, x, y, 0 ) - at( on, x, y, 2 ) ) );
		}

	const bool ok = grey == 0 && worstSite <= 1 && worstColour >= 8;
	if( !quiet )
	{
		std::printf( "bayer: Bayer off is neutral at every pixel (%d that are not); at each site the demosaic returns the sensor's own channel to within %d/255 (must be <= 1, the half-float buffer); colour moire |R - B| reaches %d/255 (must be >= 8)  %s\n",
		             grey, worstSite, worstColour, verdict( ok ) );
		std::printf( "%s\n", ok ? "bayer: the mosaic samples each colour where it says, and the demosaic makes the moire coloured"
		                        : "bayer: FAILED" );
	}
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --negative
//
// A check that cannot fail is not a check. Each of these perturbs the model
// and asserts that the check catches it.
//---------------------------------------------------------------------------
int runNegative( int width, int height )
{
	struct Control
	{
		const char* name;
		int failuresSeen;
	};
	Control controls[] = {
		{ "bands with the overlap integral detuned by 10% must fail   ", runBands( width, height, 0.1f, true ) },
		{ "energy with the overlap integral detuned by 10% must fail  ", runEnergy( width, height, 0.1f, true ) },
		{ "bayer with the mosaic phase swapped must fail              ", runBayer( width, height, 1, true ) },
	};

	int failures = 0;
	for( const Control& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		std::printf( "negative %s: %s  %s\n", c.name, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
			++failures;
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed model is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( Pitch& plugin, int width, int height, int frames, double fps )
{
	Session session;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.index < Pitch::PT_ABOUT_FIRST && p.type != FF_TYPE_BUFFER && p.type != FF_TYPE_EVENT )
			session.plugin.SetFloatParameter( p.index, p.value );
	session.fps = fps;
	if( !session.begin( width, height ) )
		return -1.0;

	const std::vector< unsigned char > card = buildCard( width, height, 0 );

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, card );
	glFinish();

	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
		session.render( warmup + frame, card );
	glFinish();
	const auto end = std::chrono::steady_clock::now();

	session.end();

	const double seconds = std::chrono::duration< double >( end - start ).count();
	return seconds * 1000.0 / static_cast< double >( frames );
}

int runBench( Pitch& plugin, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};

	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame\n" );

	for( const Size& size : sizes )
	{
		const double ms = benchAt( plugin, size.width, size.height, frames, fps );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0 );
	}

	std::printf( "\nCost is the sensor pass: one analytic box per pixel at the defaults, nine\n"
	             "under rotation, and 32 disc taps per box with Focus on. Whatever the\n"
	             "settings above were, they are what was measured; run with --set to\n"
	             "measure something else.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line. Same format as the rest
// of the fleet, so one filming script drives any of them.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"pitest -- render and measure the Pitch LED-wall-through-a-camera effect\n"
		"\n"
		"  --out PATH        render the test card through the plugin (default /tmp/pitch.png)\n"
		"  --size WxH        raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N        frames to render before reading back (default 40)\n"
		"  --fps N           synthetic frame rate driving the clock (default 60)\n"
		"  --source S        card (default), flat, or white\n"
		"  --level N         the flat source's code value, 0..255 (default 128)\n"
		"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
		"  --list            print every parameter, its kind, default and range, then exit\n"
		"  --moire           the fringe period is 1 / | s - round( s ) | pixels; Focus kills it\n"
		"  --bands           the band period is P / ( Tr / H ) rows; a whole-period shutter has none\n"
		"  --pwm             band depth against the closed-form overlap; low grey worst; Scrambled less\n"
		"  --energy          the optics conserve light\n"
		"  --faults          a dead cabinet is exactly black and exactly aligned; a seed reproduces\n"
		"  --identity        the neutral settings return the input byte for byte\n"
		"  --bayer           the mosaic samples each colour where it says\n"
		"  --negative        the checks can fail: a detuned overlap and a swapped phase are caught\n"
		"  --bench           time ProcessOpenGL at 720p through 4K\n"
		"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH     parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/pitch.png";
	std::string scriptPath;
	std::string sourceName = "card";
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	int level      = 128;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			sourceName = argv[ ++i ];
		else if( argument == "--level" && hasNext )
			level = std::atoi( argv[ ++i ] );
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--moire" || argument == "--bands" || argument == "--pwm" || argument == "--energy"
		         || argument == "--faults" || argument == "--identity" || argument == "--bayer" || argument == "--negative" )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( wantList )
	{
		//No GL needed, so it is answered before a context is made -- which also
		//means it works on a machine where creating one fails, and in CI.
		Pitch plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low,
			             p.high );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( !checks.empty() )
	{
		int failures = 0;
		for( const std::string& check : checks )
		{
			int result = 0;
			if( check == "--moire" )
				result = runMoire( width, height );
			else if( check == "--bands" )
				result = runBands( width, height );
			else if( check == "--pwm" )
				result = runPwm( width, height );
			else if( check == "--energy" )
				result = runEnergy( width, height );
			else if( check == "--faults" )
				result = runFaults( width, height );
			else if( check == "--identity" )
				result = runIdentity( width, height );
			else if( check == "--bayer" )
				result = runBayer( width, height );
			else if( check == "--negative" )
				result = runNegative( width, height );
			failures += result;
			std::printf( "\n" );
		}
		return finish( failures == 0 ? 0 : 1 );
	}

	Session session;
	session.fps = fps;

	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}

	if( wantBench )
		return finish( runBench( session.plugin, frames, fps ) );

	if( !session.begin( width, height ) )
		return finish( 1 );

	if( wantPipe )
	{
		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = entry.second;
			}
		}

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			if( got < frame.size() )
				break;

			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

			if( !session.render( index, frame ) )
				break;

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
		}

		session.end();
		return finish( 0 );
	}

	const Source source = sourceFromName( sourceName );
	for( int frame = 0; frame < frames; ++frame )
	{
		std::vector< unsigned char > pixels;
		switch( source )
		{
		case Source::Flat: pixels = buildFlat( width, height, level ); break;
		case Source::White: pixels = buildFlat( width, height, 255 ); break;
		case Source::Card:
		default: pixels = buildCard( width, height, frame ); break;
		}
		if( !session.render( frame, pixels ) )
			return finish( 1 );
	}

	const std::vector< unsigned char > image = session.readBack();
	session.end();

	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}

	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
