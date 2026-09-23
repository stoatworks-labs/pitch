#include "Shaders.h"

namespace pitch::shaders
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 space. MaxUV is folded in exactly once, in the
	//copy pass; every later pass works on a texture we allocated, where the
	//picture really does fill the texture.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: copy. The host's input into a texture of ours.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;     //the part of the input texture that is really picture
uniform vec2 HalfTexel; //half an input texel, in picture space

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Half a texel in from the edge, so GL_LINEAR at the boundary does not take
	//half its weight from the texture's undrawn padding. At every interior
	//texel centre this is an exact copy: the clamp does not move the point
	//and bilinear at a texel centre returns the texel. --identity relies on
	//that.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );
	fragColor = texture( InputTexture, picture * MaxUV );
}
)";

//---------------------------------------------------------------------------
// Pass 2: the wall. One texel per LED.
//
// LED ( i, j ) -- j counted from the TOP of the wall -- has its centre at
// source pixel WallOrigin + ( i + 0.5, j + 0.5 ) * Pitch, and takes the mean
// of a Pitch-sized box of source there through the mip chain. Its drive level
// is that colour, times whatever the cabinet's faults do to it. The output
// is the level and nothing else: time happens in the sensor pass.
//
// The randomness is an integer hash (a PCG output mix), never fract( sin ).
// A cabinet's faults are a pure function of ( Seed, cabinet x, cabinet y ),
// so a seeded wall reproduces bit-exactly and a resize does not reshuffle it.
//---------------------------------------------------------------------------
const char* const kWallShader = R"(#version 410 core

uniform sampler2D Picture;   //the copy, mipmapped; the picture fills 0..1
uniform vec2 PictureSize;    //source pixels
uniform ivec2 WallSize;      //LEDs
uniform float Pitch;         //source pixels per LED
uniform vec2 WallOrigin;     //source pixel (top-down) of LED (0,0)'s corner

uniform ivec2 Cabinet;       //LEDs per cabinet
uniform int ModuleRows;      //LED rows per module; a module is cabinet-wide

uniform int Seed;
uniform float FaultRate;     //fraction of cabinets carrying a fault of amount 1
uniform float DeadAmount;
uniform float DimAmount;
uniform float DeadRowAmount;
uniform float BinShiftAmount;

in vec2 uv;
out vec4 fragColor;

//PCG output mix. Exact in 32 bits, identical on every GPU and on the CPU.
uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

//A unit float from four integers, decorrelated by mixing each in turn.
float draw( int seed, int cx, int cy, int kind )
{
	uint h = pcg( uint( seed ) * 0x9E3779B9u + 0x2545F491u );
	h      = pcg( h ^ uint( cx ) );
	h      = pcg( h ^ ( uint( cy ) * 0x85EBCA6Bu ) );
	h      = pcg( h ^ ( uint( kind ) * 0xC2B2AE35u ) );
	return float( h >> 8u ) / 16777216.0;
}

void main()
{
	//Which LED. Texel rows are bottom-first in GL; the wall is counted from
	//the top like the picture, so the cabinet arithmetic reads as an
	//operator would draw it.
	ivec2 led = ivec2( floor( uv * vec2( WallSize ) ) );
	led       = clamp( led, ivec2( 0 ), WallSize - 1 );
	int row   = WallSize.y - 1 - led.y;
	int col   = led.x;

	//The LED's centre in source pixels, and the box of source it averages.
	//A Pitch of 1 reads level 0 at a texel centre: an exact copy.
	vec2 centre = WallOrigin + ( vec2( col, row ) + 0.5 ) * Pitch;
	vec3 level  = vec3( 0.0 );
	if( centre.x >= 0.0 && centre.x <= PictureSize.x && centre.y >= 0.0 && centre.y <= PictureSize.y )
	{
		vec2 pictureUV = vec2( centre.x / PictureSize.x, 1.0 - centre.y / PictureSize.y );
		level          = textureLod( Picture, pictureUV, log2( Pitch ) ).rgb;
	}

	//-----------------------------------------------------------------
	// Faults, per cabinet. Each kind is a separate draw against
	// FaultRate * amount, so the kinds are independent and a cabinet can
	// be both dim and shifted, as a real one can.
	//-----------------------------------------------------------------
	int cx = col / Cabinet.x;
	int cy = row / Cabinet.y;
	int inCabinetRow = row - cy * Cabinet.y;

	vec3 gain = vec3( 1.0 );

	if( draw( Seed, cx, cy, 1 ) < FaultRate * DeadAmount )
		gain = vec3( 0.0 );

	if( draw( Seed, cx, cy, 2 ) < FaultRate * DimAmount )
	{
		//Between a quarter and three quarters of the amount, so a run of dim
		//cabinets is not all the same shade.
		float drop = DimAmount * ( 0.25 + 0.5 * draw( Seed, cx, cy, 3 ) );
		gain *= 1.0 - drop;
	}

	if( draw( Seed, cx, cy, 4 ) < FaultRate * DeadRowAmount )
	{
		int modules   = max( 1, Cabinet.y / max( 1, ModuleRows ) );
		int badModule = min( modules - 1, int( draw( Seed, cx, cy, 5 ) * float( modules ) ) );
		int badRow    = min( ModuleRows - 1, int( draw( Seed, cx, cy, 6 ) * float( ModuleRows ) ) );
		if( inCabinetRow == badModule * ModuleRows + badRow )
			gain = vec3( 0.0 );
	}

	if( draw( Seed, cx, cy, 7 ) < FaultRate * BinShiftAmount )
	{
		//A different bin is a different balance: each channel wanders up to
		//a fifth of the amount either way, independently.
		vec3 shift = vec3( draw( Seed, cx, cy, 8 ), draw( Seed, cx, cy, 9 ), draw( Seed, cx, cy, 10 ) ) * 2.0 - 1.0;
		gain *= 1.0 + 0.2 * BinShiftAmount * shift;
	}

	fragColor = vec4( max( level * gain, vec3( 0.0 ) ), 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 3: the sensor. The plugin.
//
// Two samplings, one in space and one in time.
//
// SPACE. The camera's pixel grid is laid over the wall's LED grid at Scale
// LEDs per pixel and a rotation. Every LED is a small emitting rectangle in a
// cell of black, so a pixel's value is the integral of emitter coverage over
// its aperture. That integral is done analytically: the aperture is an
// axis-aligned box in LED space (or, under rotation, a few smaller boxes
// approximating the rotated square), and the coverage of a box against an
// axis-aligned rectangle is a product of two one-dimensional overlaps. No
// point sampling anywhere, so the fill factor comes out exact and the moire
// is the true beat between the two lattices, not the beat with a sample
// pattern. Focus is a disc of taps on the sensor, each tap the same analytic
// box: blur before sampling, which is the only place a lens can blur.
//
// TIME. Sensor row r of H integrates over [ r * Tr / H, r * Tr / H + E ]. In
// sub-period units that window is handed over per row as ( whole, fraction )
// pairs, computed in double on the CPU, so nothing here ever holds a large
// number: the overlap of the window with an LED's pulse train is
//
//     F( u ) = floor( u ) * w + min( frac( u ), w )
//     on     = F( u1 - slot ) - F( u0 - slot )
//
// in closed form, with the whole parts subtracted as small integers and the
// fractions kept in [0,1). What comes out is normalised so that a window an
// integer number of sub-periods long reports exactly the LED's level -- the
// operator's rule that the shutter should be a multiple of the refresh.
//
// Everything an operator fights is what those two samplings DO: moire is the
// spatial one, scan bands are the temporal one, low-grey breakup is the
// temporal one at a short pulse, black between the pixels is the fill factor,
// and a dead cabinet is a rectangle of LEDs whose level is zero.
//---------------------------------------------------------------------------
const char* const kSensorShader = R"(#version 410 core

uniform sampler2D Wall;       //one texel per LED, mipmapped, levels 0..1
uniform sampler2D RowTable;   //per sensor row: ( floor u0, frac u0, floor u1, frac u1 )
uniform sampler2D Source;     //the host's input, for Mix
uniform vec2 MaxUV;

uniform ivec2 Size;           //sensor pixels
uniform ivec2 WallSize;       //LEDs
uniform float Scale;          //LEDs per sensor pixel
uniform float CosR;           //rotation of the sensor grid over the wall
uniform float SinR;
uniform int SubDiv;           //boxes per pixel per axis: 1 exact, 3 under rotation
uniform int Olpf;             //1: four copies one pixel pitch apart
uniform float FocusRadius;    //blur-disc radius, sensor pixels; 0 for none
uniform int FocusTaps;        //taps on the disc; 1 when FocusRadius is 0

uniform float EmitHalf;       //half the emitter's side, in LED units: sqrt( fill ) / 2
uniform int Layout;           //0: 3-in-1 (one square), 1: discrete (three strips)

uniform int Scan;             //scan groups S
uniform float GreyLevels;     //2^bits - 1
uniform float WindowLen;      //exposure, in sub-periods
uniform float OverlapDetune;  //ALWAYS 0 in the plugin; the harness's negative control

uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

const int kMaxSpan     = 6;   //LEDs per axis a box may touch on the exact path
const float kExactSpan = 3.0; //boxes wider than this take the prefiltered path

float overlap1( float a0, float a1, float b0, float b1 )
{
	return max( 0.0, min( a1, b1 ) - max( a0, b0 ) );
}

//What one LED emits over this row's window, per channel, as a level: the
//closed-form overlap of the window with the pulse train, normalised.
vec3 emission( vec3 level, int ledRow, vec4 win )
{
	vec3 q     = floor( level * GreyLevels + 0.5 ) / GreyLevels;
	float slot = float( ledRow - ( ledRow / Scan ) * Scan ) / float( Scan );
	vec3 w     = q / float( Scan );

	float i0 = win.x, f0 = win.y - slot;
	if( f0 < 0.0 ) { f0 += 1.0; i0 -= 1.0; }
	float i1 = win.z, f1 = win.w - slot;
	if( f1 < 0.0 ) { f1 += 1.0; i1 -= 1.0; }

	vec3 on = ( i1 - i0 ) * w + min( vec3( f1 ), w * ( 1.0 + OverlapDetune ) ) - min( vec3( f0 ), w );
	return on * float( Scan ) / WindowLen;
}

//Light integrated over an axis-aligned box [lo, hi] in LED coordinates
//( x right, y down ), in units of LED-area times level.
vec3 boxLight( vec2 lo, vec2 hi, vec4 win )
{
	vec2 span = hi - lo;

	if( max( span.x, span.y ) > kExactSpan )
	{
		//Zoomed well out: a pixel holds more LEDs than are worth walking, and
		//a real lens has long since blurred the lattice away. The mip chain
		//gives the mean level, the fill factor gives the emitting fraction,
		//and only a single-group wall still bands at this scale -- with
		//several groups a pixel this wide averages every phase.
		vec2 inside = vec2( overlap1( lo.x, hi.x, 0.0, float( WallSize.x ) ),
		                    overlap1( lo.y, hi.y, 0.0, float( WallSize.y ) ) );
		if( inside.x <= 0.0 || inside.y <= 0.0 )
			return vec3( 0.0 );
		vec2 centre = ( lo + hi ) * 0.5;
		vec2 wallUV = vec2( centre.x / float( WallSize.x ), 1.0 - centre.y / float( WallSize.y ) );
		vec3 mean   = textureLod( Wall, wallUV, log2( max( span.x, span.y ) ) ).rgb;
		vec3 lit    = Scan == 1 ? emission( mean, 0, win ) : floor( mean * GreyLevels + 0.5 ) / GreyLevels;
		float fill  = EmitHalf * EmitHalf * 4.0;
		return lit * fill * inside.x * inside.y;
	}

	ivec2 a = ivec2( floor( lo ) );
	ivec2 b = ivec2( floor( hi ) );
	a       = max( a, ivec2( 0 ) );
	b       = min( b, WallSize - 1 );

	//Every overlap below is taken in the LED's OWN coordinates: the box edges
	//less the LED's index, which is an exact subtraction, against an emitter
	//edge that is a small number. At the wall's magnitude a float resolves
	//1e-5 of an LED, and an emitter edge rounded there is a systematic error
	//of that size in every coverage -- measured as 1.7e-5 of the light at
	//320 px, and it would be 3e-4 at 4K.
	vec3 sum = vec3( 0.0 );
	for( int j = a.y; j <= b.y && j - a.y < kMaxSpan; ++j )
	{
		float ly0 = lo.y - float( j );
		float ly1 = hi.y - float( j );
		float ay  = overlap1( ly0, ly1, 0.5 - EmitHalf, 0.5 + EmitHalf );
		if( ay <= 0.0 )
			continue;

		for( int i = a.x; i <= b.x && i - a.x < kMaxSpan; ++i )
		{
			vec3 level = texelFetch( Wall, ivec2( i, WallSize.y - 1 - j ), 0 ).rgb;
			if( level.r <= 0.0 && level.g <= 0.0 && level.b <= 0.0 )
				continue;

			float lx0 = lo.x - float( i );
			float lx1 = hi.x - float( i );
			vec3 ax;
			if( Layout == 0 )
			{
				ax = vec3( overlap1( lx0, lx1, 0.5 - EmitHalf, 0.5 + EmitHalf ) );
			}
			else
			{
				//Three strips side by side, each a third of the width, each
				//three times as bright: the same light from the LED as a whole,
				//and a camera that resolves the chips sees them saturate.
				float third = EmitHalf * 2.0 / 3.0;
				float x0    = 0.5 - EmitHalf;
				ax = 3.0 * vec3( overlap1( lx0, lx1, x0, x0 + third ),
				                 overlap1( lx0, lx1, x0 + third, x0 + 2.0 * third ),
				                 overlap1( lx0, lx1, x0 + 2.0 * third, 0.5 + EmitHalf ) );
			}
			if( ax.r <= 0.0 && ax.g <= 0.0 && ax.b <= 0.0 )
				continue;

			sum += ax * ay * emission( level, j, win );
		}
	}
	return sum;
}

//The golden-angle spiral: taps spread evenly over the unit disc with no
//lattice of their own to beat against the wall's.
vec2 discTap( int t, int taps )
{
	float r     = sqrt( ( float( t ) + 0.5 ) / float( taps ) );
	float theta = float( t ) * 2.39996323;
	return r * vec2( cos( theta ), sin( theta ) );
}

void main()
{
	//Which pixel, counted from the top like a sensor row. Reconstructed as an
	//integer so the pixel's centre is exact rather than an interpolated uv.
	ivec2 p = ivec2( floor( uv * vec2( Size ) ) );
	p       = clamp( p, ivec2( 0 ), Size - 1 );
	int row = Size.y - 1 - p.y;

	vec4 win = texelFetch( RowTable, ivec2( row, 0 ), 0 );

	vec2 pixelCorner  = vec2( float( p.x ), float( row ) );
	vec2 sensorCentre = vec2( Size ) * 0.5;
	vec2 wallCentre   = vec2( WallSize ) * 0.5;

	float subSide = 1.0 / float( SubDiv );
	float boxHalf = Scale * subSide * 0.5;
	int copies    = Olpf == 1 ? 4 : 1;

	vec3 sum    = vec3( 0.0 );
	float count = 0.0;
	for( int t = 0; t < FocusTaps; ++t )
	{
		vec2 focusOffset = FocusRadius > 0.0 ? discTap( t, FocusTaps ) * FocusRadius : vec2( 0.0 );
		for( int c = 0; c < copies; ++c )
		{
			//A four-spot birefringent filter: four copies one pixel pitch
			//apart, which puts the filter's first null at the Nyquist
			//frequency. A quarter-pixel version was tried first and was
			//EXACTLY invisible at four pixels per LED: averaging a box
			//integral over shifts smaller than the distance from an emitter
			//edge to the box's own edge is linear, and linear averages to the
			//unshifted value.
			vec2 olpfOffset = Olpf == 1 ? vec2( ( c & 1 ) == 0 ? -0.5 : 0.5, ( c & 2 ) == 0 ? -0.5 : 0.5 ) : vec2( 0.0 );
			for( int sy = 0; sy < SubDiv; ++sy )
			{
				for( int sx = 0; sx < SubDiv; ++sx )
				{
					//The box's EDGES are the mapped edges of the sub-pixel, so
					//two neighbouring pixels compute their shared edge from one
					//expression and the boxes tile the wall bitwise. A box
					//built as centre +- half does not: the two rounded edges
					//leave a gap of a float ULP at every pixel, and the light
					//in the gaps is lost.
					vec2 e0 = pixelCorner + vec2( sx, sy ) * subSide + focusOffset + olpfOffset - sensorCentre;
					vec2 e1 = e0 + subSide;
					vec2 lo, hi;
					if( SinR == 0.0 )
					{
						lo = e0 * Scale + wallCentre;
						hi = e1 * Scale + wallCentre;
					}
					else
					{
						vec2 q = e0 + 0.5 * subSide;
						vec2 r = vec2( q.x * CosR - q.y * SinR, q.x * SinR + q.y * CosR );
						vec2 ledCentre = r * Scale + wallCentre;
						lo = ledCentre - boxHalf;
						hi = ledCentre + boxHalf;
					}
					sum += boxLight( lo, hi, win );
					count += 1.0;
				}
			}
		}
	}

	//Mean radiance over the aperture: each box integrated level over its own
	//area, so divide by the area of one box and the number of boxes.
	vec3 light  = sum / ( count * 4.0 * boxHalf * boxHalf );
	vec4 result = vec4( light, 1.0 );

	vec4 source = texture( Source, uv * MaxUV );
	fragColor   = mix( source, result, MixAmount );
}
)";

//---------------------------------------------------------------------------
// Pass 4: the Bayer mosaic and a plain bilinear demosaic.
//
// The mosaic samples each colour on its own sub-lattice of the sensor, and
// the demosaic fills the gaps from neighbours. On a wall that is white to the
// sensor, that is what turns a luminance beat into a coloured one: the R and
// B sub-lattices sample the same fringe a pixel apart, and near the Nyquist
// frequency a pixel apart is half a cycle.
//---------------------------------------------------------------------------
const char* const kDemosaicShader = R"(#version 410 core

uniform sampler2D Sensor;   //the sensor pass, full colour, before the mosaic
uniform sampler2D Source;   //the host's input, for Mix
uniform vec2 MaxUV;
uniform ivec2 Size;
uniform int BayerPhase;     //0: R at even/even (RGGB); 1: R and B swapped. Test hook.
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

//0 red, 1 green, 2 blue, for a pixel counted from the top-left.
int channelOf( ivec2 p )
{
	int k = ( p.y & 1 ) * 2 + ( p.x & 1 );
	if( BayerPhase == 1 )
		k = 3 - k;
	return k == 0 ? 0 : ( k == 3 ? 2 : 1 );
}

float mosaic( ivec2 p, int channel )
{
	ivec2 c = clamp( p, ivec2( 0 ), Size - 1 );
	return texelFetch( Sensor, ivec2( c.x, Size.y - 1 - c.y ), 0 )[ channel ];
}

void main()
{
	ivec2 p = ivec2( floor( uv * vec2( Size ) ) );
	p       = clamp( p, ivec2( 0 ), Size - 1 );
	p.y     = Size.y - 1 - p.y;

	int own = channelOf( p );
	vec3 colour = vec3( 0.0 );
	for( int channel = 0; channel < 3; ++channel )
	{
		if( channel == own )
		{
			colour[ channel ] = mosaic( p, own );
			continue;
		}
		//The neighbours in the 3x3 that carry this channel: two across or two
		//down for a colour at a green site, four diagonals for the opposite
		//colour, four orthogonals for green.
		float sum   = 0.0;
		float count = 0.0;
		for( int dy = -1; dy <= 1; ++dy )
			for( int dx = -1; dx <= 1; ++dx )
			{
				if( dx == 0 && dy == 0 )
					continue;
				ivec2 n = p + ivec2( dx, dy );
				if( channelOf( n ) != channel )
					continue;
				sum += mosaic( n, channel );
				count += 1.0;
			}
		colour[ channel ] = count > 0.0 ? sum / count : 0.0;
	}

	vec4 source = texture( Source, uv * MaxUV );
	fragColor   = mix( source, vec4( colour, 1.0 ), MixAmount );
}
)";

} // namespace pitch::shaders
