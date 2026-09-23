/**
 * Pitch — browser demo.
 *
 * An LED wall seen through a camera. The clip is resampled onto a wall of LEDs
 * with black between them, lit one scan group at a time by PWM, and a camera
 * with a rolling shutter and a Bayer sensor photographs it. Moiré, scan bands,
 * low-grey breakup, the black between the pixels and cabinet faults all fall
 * out of the two samplings — space and time — rather than being drawn.
 *
 * All five shader constants below — `kVertexShader`, `kCopyShader`,
 * `kWallShader`, `kSensorShader` and `kDemosaicShader` — are
 * `source/Shaders.cpp`, copied across unedited. `demo/tools/check_shaders.py`
 * compares them character for character against the C++ and is called from
 * `tools/verify.sh`, because two copies of a shader is exactly the arrangement
 * that drifts.
 *
 * The CPU half is small and it is a **port**: `source/Controls.cpp` (every
 * slider's conversion), the wall's cabinet count and origin, the per-row
 * exposure windows in PWM sub-periods and their split into a whole and a
 * fraction (`split()`), `drive::greyLevels`, and the frame-period smoothing,
 * all out of `Pitch::ProcessOpenGL`. **Nothing checks that port but a reader**
 * — `check_shaders.py` only sees the GLSL.
 *
 * ------------------------------------------------------- what is missing
 *
 * **The host clock's unit.** The plugin votes on whether the host counts in
 * seconds or milliseconds (readout's unit voting). A browser's clock is
 * unambiguous, so that half is absent; the frame-period estimate that turns
 * host frames into camera frames is ported as it stands.
 *
 * **The About block** — four buttons that open a browser.
 *
 * **The integer controls are sliders.** Cabinet W, Cabinet H, Module Rows,
 * Grey Bits and Fault Seed are FF_TYPE_INTEGER in the plugin, typed into as
 * numbers with real ranges. The kit has no integer control, so here they are
 * 0..1 sliders that land on every integer in the plugin's range and show it.
 *
 * **The wall's float format on some browsers.** The plugin keeps the wall at
 * RGBA32F with a mip chain. Sampling a 32-bit float texture with a filter needs
 * OES_texture_float_linear in WebGL2, which most desktop browsers have and many
 * phones do not; without it this page keeps the wall at RGBA16F and says so
 * under the picture.
 *
 * No audio caveat: Pitch has no audio path.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// Pass the RAW `#version 410 core` text to Program: its constructor calls the
// kit's port() on both sources itself.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

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
`;

const COPY = `#version 410 core

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
`;

const WALL = `#version 410 core

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
`;

const SENSOR = `#version 410 core

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
`;

const DEMOSAIC = `#version 410 core

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

	//The 3x3 neighbourhood of the MOSAIC: each site fetched once, carrying
	//only the channel its filter passes. Nine fetches, not one per channel
	//per neighbour.
	int own = channelOf( p );
	vec3 sum   = vec3( 0.0 );
	vec3 count = vec3( 0.0 );
	float centre = 0.0;
	for( int dy = -1; dy <= 1; ++dy )
		for( int dx = -1; dx <= 1; ++dx )
		{
			ivec2 n     = p + ivec2( dx, dy );
			int channel = channelOf( n );
			float v     = mosaic( n, channel );
			if( dx == 0 && dy == 0 )
			{
				centre = v;
				continue;
			}
			//The neighbours that carry a channel: two across or two down for
			//a colour at a green site, four diagonals for the opposite colour,
			//four orthogonals for green.
			sum[ channel ] += v;
			count[ channel ] += 1.0;
		}
	vec3 colour = vec3( 0.0 );
	for( int channel = 0; channel < 3; ++channel )
		colour[ channel ] = channel == own ? centre : ( count[ channel ] > 0.0 ? sum[ channel ] / count[ channel ] : 0.0 );

	vec4 source = texture( Source, uv * MaxUV );
	fragColor   = mix( source, vec4( colour, 1.0 ), MixAmount );
}
`;

//===========================================================================
// Controls.cpp — ported. Host parameters are 0..1; these are what they mean.
//===========================================================================

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const lerp = (from, to, t) => from + (to - from) * clamp01(t);
const unlerp = (from, to, value) => clamp01((value - from) / (to - from));
const geometric = (from, to, t) => from * Math.pow(to / from, clamp01(t));
const ungeometric = (from, to, value) => clamp01(Math.log(value / from) / Math.log(to / from));
// std::lround: halves away from zero. Every value here is non-negative.
const lround = (v) => Math.floor(v + 0.5);

const SCAN_NAMES = ['1/1', '1/2', '1/4', '1/8', '1/16', '1/32'];
const FRAME_RATES = [23.976, 24.0, 25.0, 29.97, 30.0, 50.0, 59.94, 60.0];
const FRAME_RATE_NAMES = ['23.98', '24', '25', '29.97', '30', '50', '59.94', '60'];
const SCRAMBLED_SUB_PERIODS = 16;

const optionIndex = (value, count) => clamp(lround(value), 0, count - 1);

const controls = {
  pitchPixels: (v) => geometric(1.0, 16.0, v),
  pitchParam: (px) => ungeometric(1.0, 16.0, px),
  fillFactor: (v) => lerp(0.05, 1.0, v),
  fillParam: (f) => unlerp(0.05, 1.0, f),
  refreshHz: (v) => geometric(240.0, 7680.0, v),
  refreshParam: (hz) => ungeometric(240.0, 7680.0, hz),
  scanGroups: (option) => 1 << optionIndex(option, SCAN_NAMES.length),
  subPeriods: (option) => (optionIndex(option, 2) === 1 ? SCRAMBLED_SUB_PERIODS : 1),
  cameraScale: (v) => geometric(0.125, 8.0, v),
  cameraScaleParam: (s) => ungeometric(0.125, 8.0, s),
  // ( value - 0.5 ) * 20 is exactly zero at 0.5, which keeps the sensor pass on
  // its exact axis-aligned coverage path.
  rotationRadians: (v) => (clamp01(v) - 0.5) * 20.0 * Math.PI / 180.0,
  focusRadius: (v) => (v <= 0 ? 0 : geometric(0.05, 4.0, v)),
  focusParam: (px) => (px <= 0 ? 0 : ungeometric(0.05, 4.0, px)),
  shutterSeconds: (v) => 1.0 / geometric(24.0, 8000.0, v),
  shutterParam: (s) => ungeometric(24.0, 8000.0, 1.0 / s),
  readoutSeconds: (v) => geometric(0.004, 0.040, v),
  readoutParam: (s) => ungeometric(0.004, 0.040, s),
  frameRateHz: (option) => FRAME_RATES[optionIndex(option, FRAME_RATES.length)],
  faultRate: (v) => clamp01(v),
};

// drive::greyLevels
const greyLevels = (bits) => (1 << clamp(bits, 1, 16)) - 1;

//===========================================================================
// The integer parameters.
//
// FF_TYPE_INTEGER is exempt from the SDK's 0..1 clamp, so the plugin holds
// these as the integer itself, with a real range. The kit has only 0..1
// sliders, so here each is a slider over exactly that range that lands on an
// integer and shows it. The shareable URL carries the slider position.
//===========================================================================

const INTEGERS = {
  cabinetW: [8, 256],
  cabinetH: [8, 256],
  moduleRows: [2, 64],
  greyBits: [4, 16],
  faultSeed: [0, 999],
};
const integerValue = (id, v) => {
  const [lo, hi] = INTEGERS[id];
  return lo + Math.round(clamp01(v) * (hi - lo));
};
const integerParam = (id, value) => {
  const [lo, hi] = INTEGERS[id];
  return (value - lo) / (hi - lo);
};

//===========================================================================
// Pitch::ProcessOpenGL — the CPU half, ported.
//===========================================================================

const MAX_WALL_SIDE = 4096; // Pitch::kMaxWallSide
const FOCUS_TAPS = 32; // Pitch::kFocusTaps
const MAX_FRAME_DELTA = 0.25;

/**
 * split(): a non-negative double into a whole and a fraction, both float. The
 * whole part is exact up to 2^24 and the fraction is small, so the shader never
 * holds anything a float cannot resolve. Math.fround is what `static_cast<
 * float >` does.
 */
function split(u, out, at) {
  const w = Math.floor(u);
  let whole = Math.fround(w);
  let fraction = Math.fround(u - w);
  // A fraction that rounds up to exactly 1.0 in float is a carry.
  if (fraction >= 1.0) {
    fraction = 0.0;
    whole += 1.0;
  }
  out[at] = whole;
  out[at + 1] = fraction;
}

function createRenderer(gl, quad) {
  const copyShader = new Program(gl, VERTEX, COPY, 'copy');
  const wallShader = new Program(gl, VERTEX, WALL, 'wall');
  const sensorShader = new Program(gl, VERTEX, SENSOR, 'sensor');
  const demosaicShader = new Program(gl, VERTEX, DEMOSAIC, 'demosaic');

  // The plugin's wall is RGBA32F, mipmapped and sampled LINEAR_MIPMAP_LINEAR.
  // In WebGL2 a filtered 32-bit float texture needs OES_texture_float_linear;
  // without it the texture is incomplete and samples as black, which would be a
  // plausible-looking dead wall. So: RGBA32F where the browser can filter it,
  // RGBA16F where it cannot, and the page says which.
  const floatLinear = gl.getExtension('OES_texture_float_linear') !== null;
  const wallFormat = floatLinear ? gl.RGBA32F : gl.RGBA16F;

  const picture = new PassBuffer(gl, { mip: true });
  const wall = new PassBuffer(gl, { mip: true });
  const sensor = new PassBuffer(gl, { filter: 'nearest' });

  // The row table: per sensor row ( floor u0, frac u0, floor u1, frac u1 ).
  const rowTable = gl.createTexture();
  let rowTableRows = 0;
  let rowWindows = new Float32Array(0);

  // The frame period, measured from the clock's own deltas.
  let lastNow = -1;
  let frameSeconds = 1.0 / 60.0;

  const note = document.createElement('p');
  note.className = 'stage__status';
  note.textContent = floatLinear
    ? ''
    : 'This browser cannot filter a 32-bit float texture (no OES_texture_float_linear), so the wall is kept at RGBA16F here rather than the plugin\'s RGBA32F. Levels are rounded to about three decimal places before the PWM sees them.';
  if (!floatLinear) {
    document.querySelector(".stage")?.append(note);
  }

  return {
    render({ input, params, width, height, time }) {
      const p = (id) => params.get(id);

      //---------------------------------------------------------------
      // The clock, and the frame period measured from it.
      //---------------------------------------------------------------
      const now = time;
      if (lastNow >= 0 && now > lastNow) {
        const dt = Math.min(now - lastNow, MAX_FRAME_DELTA);
        const delta = clamp(dt, 1.0 / 240.0, 1.0 / 10.0);
        frameSeconds += (delta - frameSeconds) * 0.15;
      }
      lastNow = now;

      //---------------------------------------------------------------
      // What the controls say.
      //---------------------------------------------------------------
      const pitch = controls.pitchPixels(p('pitch'));
      const fill = controls.fillFactor(p('fill'));
      const layout = clamp(lround(p('layout')), 0, 1);
      const cabinetW = integerValue('cabinetW', p('cabinetW'));
      const cabinetH = integerValue('cabinetH', p('cabinetH'));
      const moduleRows = integerValue('moduleRows', p('moduleRows'));

      const refreshHz = controls.refreshHz(p('refresh'));
      const scanGroups = controls.scanGroups(p('scan'));
      const greyBits = integerValue('greyBits', p('greyBits'));
      const subPeriods = controls.subPeriods(p('pwm'));
      const refreshPhase = clamp(p('refreshPhase'), 0, 1);

      const scale = controls.cameraScale(p('scale'));
      const rotation = controls.rotationRadians(p('rotation'));
      const focus = controls.focusRadius(p('focus'));
      const olpf = p('olpf') > 0.5;
      const shutter = controls.shutterSeconds(p('shutter'));
      const readout = controls.readoutSeconds(p('readout'));
      const frameRate = controls.frameRateHz(p('frameRate'));
      const bayer = p('bayer') > 0.5;

      const faultRate = controls.faultRate(p('faultRate'));
      const seed = integerValue('faultSeed', p('faultSeed'));

      //---------------------------------------------------------------
      // The wall: a whole number of cabinets, the nearest to what the
      // picture holds, at least one, centred on the picture.
      //---------------------------------------------------------------
      const ledsAcross = width / pitch;
      const ledsDown = height / pitch;
      const cabinetsX = Math.max(1, lround(ledsAcross / cabinetW));
      const cabinetsY = Math.max(1, lround(ledsDown / cabinetH));
      const wallW = Math.min(cabinetsX * cabinetW, MAX_WALL_SIDE);
      const wallH = Math.min(cabinetsY * cabinetH, MAX_WALL_SIDE);
      const originX = (width - wallW * pitch) * 0.5;
      const originY = (height - wallH * pitch) * 0.5;

      //---------------------------------------------------------------
      // Buffers.
      //---------------------------------------------------------------
      picture.ensure(width, height, gl.RGBA8);
      wall.ensure(wallW, wallH, wallFormat);
      if (bayer) sensor.ensure(width, height, gl.RGBA16F);

      if (rowTableRows !== height) {
        gl.bindTexture(gl.TEXTURE_2D, rowTable);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, height, 1, 0, gl.RGBA, gl.FLOAT, null);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.bindTexture(gl.TEXTURE_2D, null);
        rowTableRows = height;
        rowWindows = new Float32Array(height * 4);
      }

      //---------------------------------------------------------------
      // Time, in sub-periods, per row, in double. The whole number of
      // sub-periods elapsed is dropped here, so every row's window is a
      // small number by the time it becomes a float.
      //---------------------------------------------------------------
      const subPeriod = 1.0 / (refreshHz * subPeriods);
      const cameraTime = now / (frameSeconds * frameRate);
      const frameStart = cameraTime / subPeriod + refreshPhase * subPeriods;
      const phase0 = frameStart - Math.floor(frameStart);
      const rowStep = (readout / height) / subPeriod;
      const windowLen = shutter / subPeriod;

      for (let r = 0; r < height; r += 1) {
        const u0 = phase0 + r * rowStep;
        const u1 = u0 + windowLen;
        split(u0, rowWindows, r * 4);
        split(u1, rowWindows, r * 4 + 2);
      }
      gl.bindTexture(gl.TEXTURE_2D, rowTable);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, height, 1, gl.RGBA, gl.FLOAT, rowWindows);
      gl.bindTexture(gl.TEXTURE_2D, null);

      gl.disable(gl.BLEND);

      //---------------------------------------------------------------
      // 1. Copy, with a mip chain.
      //---------------------------------------------------------------
      picture.bind();
      copyShader.use();
      bindTexture(gl, 0, input.texture);
      copyShader.setSampler('InputTexture', 0);
      copyShader.set('MaxUV', 1, 1);
      copyShader.set('HalfTexel', 0.5 / width, 0.5 / height);
      quad.draw();
      picture.generateMipmap();

      //---------------------------------------------------------------
      // 2. The wall.
      //---------------------------------------------------------------
      wall.bind();
      wallShader.use();
      bindTexture(gl, 0, picture.texture);
      wallShader.setSampler('Picture', 0);
      wallShader.set('PictureSize', width, height);
      gl.uniform2i(wallShader.location('WallSize'), wallW, wallH);
      wallShader.set('Pitch', pitch);
      wallShader.set('WallOrigin', originX, originY);
      gl.uniform2i(wallShader.location('Cabinet'), cabinetW, cabinetH);
      wallShader.setInt('ModuleRows', moduleRows);
      wallShader.setInt('Seed', seed);
      wallShader.set('FaultRate', faultRate);
      wallShader.set('DeadAmount', p('dead'));
      wallShader.set('DimAmount', p('dim'));
      wallShader.set('DeadRowAmount', p('deadRow'));
      wallShader.set('BinShiftAmount', p('binShift'));
      quad.draw();
      wall.generateMipmap();

      //---------------------------------------------------------------
      // 3. The sensor. To the canvas when there is no mosaic, else to a
      // buffer. One exact box where the aperture is axis-aligned, nine
      // under rotation, one again under Focus.
      //---------------------------------------------------------------
      const rotated = rotation !== 0;
      const subDiv = rotated && focus <= 0 ? 3 : 1;
      const taps = focus > 0 ? FOCUS_TAPS : 1;

      const drawSensor = (mixAmount) => {
        sensorShader.use();
        bindTexture(gl, 0, wall.texture);
        bindTexture(gl, 1, rowTable);
        bindTexture(gl, 2, input.texture);
        sensorShader.setSampler('Wall', 0);
        sensorShader.setSampler('RowTable', 1);
        sensorShader.setSampler('Source', 2);
        sensorShader.set('MaxUV', 1, 1);
        gl.uniform2i(sensorShader.location('Size'), width, height);
        gl.uniform2i(sensorShader.location('WallSize'), wallW, wallH);
        sensorShader.set('Scale', scale);
        sensorShader.set('CosR', Math.cos(rotation));
        sensorShader.set('SinR', Math.sin(rotation));
        sensorShader.setInt('SubDiv', subDiv);
        sensorShader.setInt('Olpf', olpf ? 1 : 0);
        sensorShader.set('FocusRadius', focus);
        sensorShader.setInt('FocusTaps', taps);
        sensorShader.set('EmitHalf', 0.5 * Math.sqrt(fill));
        sensorShader.setInt('Layout', layout);
        sensorShader.setInt('Scan', scanGroups);
        sensorShader.set('GreyLevels', greyLevels(greyBits));
        sensorShader.set('WindowLen', windowLen);
        // Always 0 in the plugin: the harness's negative control.
        sensorShader.set('OverlapDetune', 0);
        sensorShader.set('MixAmount', mixAmount);
        quad.draw();
      };

      if (!bayer) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, width, height);
        drawSensor(p('mix'));
        return;
      }

      sensor.bind();
      drawSensor(1.0);

      //---------------------------------------------------------------
      // 4. The mosaic and the demosaic, straight to the canvas.
      //---------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      demosaicShader.use();
      bindTexture(gl, 0, sensor.texture);
      bindTexture(gl, 1, input.texture);
      demosaicShader.setSampler('Sensor', 0);
      demosaicShader.setSampler('Source', 1);
      demosaicShader.set('MaxUV', 1, 1);
      gl.uniform2i(demosaicShader.location('Size'), width, height);
      // RGGB. The swapped phase is the harness's negative control.
      demosaicShader.setInt('BayerPhase', 0);
      demosaicShader.set('MixAmount', p('mix'));
      quad.draw();
    },
  };
}

//===========================================================================
// The page.
//===========================================================================

const fmt = (v, digits = 2) => v.toFixed(digits);

const std = (id, name, def, group, display, hint) => ({ id, name, type: 'standard', default: def, group, display, hint });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });
const integer = (id, name, value, group, unit, hint) => ({
  id,
  name,
  type: 'standard',
  default: integerParam(id, value),
  group,
  display: (v) => `${integerValue(id, v)}${unit}`,
  hint,
});

function shutterLabel(v) {
  const s = controls.shutterSeconds(v);
  return `1/${Math.round(1 / s)} s`;
}

mountDemo({
  name: 'Pitch',
  pluginId: 'PI01',
  tagline:
    'An LED wall seen through a camera. The clip is resampled onto a wall of LEDs with black between them, lit one scan group at a time by PWM, and a camera with a rolling shutter and a Bayer sensor photographs it. Coloured moiré that swims with the zoom, scan bands that roll, low greys that break up, and cabinets that are dead, dim or a shade off — none of it drawn, all of it the two samplings.',
  repo: 'https://github.com/stoatworks-labs/pitch',
  page: 'https://stoatworks-labs.com/software/pitch/',
  video: 'https://www.youtube.com/watch?v=FOQa3280HJ8',

  // The wall is RGBA32F (or RGBA16F, see above), the sensor RGBA16F, and the
  // row table RGBA32F: a float render target is an opt-in in WebGL2.
  needFloat: true,

  // Every pass writes alpha 1: the wall is opaque light, with black between.
  showBackdrop: false,

  params: [
    std('pitch', 'Pitch', controls.pitchParam(4.0), 'Wall',
      (v) => `${fmt(controls.pitchPixels(v))} px/LED`,
      'Source pixels per LED, 1 to 16, geometric. 1 is one LED per clip pixel; 16 turns a 1080p clip into a 120×68 wall.'),
    std('fill', 'Fill Factor', controls.fillParam(0.5), 'Wall',
      (v) => `${Math.round(controls.fillFactor(v) * 100)}%`,
      'The fraction of each LED cell that emits, 0.05 to 1. Zoom in and the pixels separate; zoom out and the light is the same. 1 is a wall with no black between the pixels.'),
    opt('layout', 'LED Layout', ['3-in-1 SMD', 'Discrete RGB'], 0, 'Wall',
      'One square emitter per LED, or three strips side by side, each a third of the width and three times as bright — a camera that resolves the chips sees them saturate.'),
    integer('cabinetW', 'Cabinet W', 32, 'Wall', ' LEDs',
      'LEDs across a cabinet, 8 to 256. An integer the plugin takes typed; a slider here.'),
    integer('cabinetH', 'Cabinet H', 32, 'Wall', ' LEDs',
      'LEDs down a cabinet, 8 to 256. The wall is a whole number of cabinets, the nearest to what the clip holds, centred on it.'),
    integer('moduleRows', 'Module Rows', 16, 'Wall', ' rows',
      'LED rows per module, 2 to 64. A module is cabinet-wide; Dead Row takes out one row of one module.'),

    std('refresh', 'Refresh', controls.refreshParam(3840.0), 'Drive',
      (v) => `${Math.round(controls.refreshHz(v))} Hz`,
      'The rate the whole PWM pattern repeats at, 240 to 7680 Hz, geometric. 3840 is the figure quoted for a broadcast-grade wall.'),
    opt('scan', 'Scan Ratio', SCAN_NAMES, 4, 'Drive',
      'How many scan groups the driver multiplexes: one group of rows is lit at a time.'),
    integer('greyBits', 'Grey Bits', 12, 'Drive', ' bits',
      'The PWM\'s grey depth, 4 to 16. Levels are quantised to it before the pulse is built.'),
    opt('pwm', 'PWM', ['Conventional', 'Scrambled'], 0, 'Drive',
      'Conventional puts one pulse per refresh. Scrambled splits the on-time into 16 equal pulses spread through the refresh, and the band depth falls by about that factor.'),
    std('refreshPhase', 'Refresh Phase', 0, 'Drive',
      (v) => `${Math.round(clamp01(v) * 360)}°`,
      'Where in its cycle the wall\'s pulse train sits against the camera\'s frame start.'),

    std('scale', 'Camera Scale', controls.cameraScaleParam(0.25), 'Camera',
      (v) => `${fmt(controls.cameraScale(v), 3)} LEDs/px`,
      'Zoom and distance in one number: 0.125 to 8 LEDs per sensor pixel, geometric. Near 1 LED per pixel the moiré is worst; the fringe is 1 / |s − round(s)| pixels long.'),
    std('rotation', 'Rotation', 0.5, 'Camera',
      (v) => `${fmt((clamp01(v) - 0.5) * 20, 1)}°`,
      'The sensor grid over the wall, ±10°. 0.5 is exactly square, which keeps each pixel one exact box; off square it is nine smaller boxes.'),
    std('focus', 'Focus', 0, 'Camera',
      (v) => (controls.focusRadius(v) === 0 ? 'sharp' : `${fmt(controls.focusRadius(v))} px`),
      'A blur disc of 0 to 4 sensor pixels radius, 32 taps. A lens blurs before the sensor samples, which is the only place a blur can remove aliasing — so this is what kills the moiré.'),
    bool('olpf', 'OLPF', 0, 'Camera',
      'An optical low-pass filter: four copies one pixel pitch apart, which puts its first null at the Nyquist frequency.'),
    std('shutter', 'Shutter', controls.shutterParam(1 / 1000), 'Camera', shutterLabel,
      '1/8000 to 1/24 s. Set it to a whole number of refresh periods and the band depth is zero — the operator\'s rule, and here a theorem.'),
    std('readout', 'Readout', controls.readoutParam(0.016), 'Camera',
      (v) => `${fmt(controls.readoutSeconds(v) * 1000, 1)} ms`,
      'How long the rolling shutter takes to read every row, 4 to 40 ms. Row r starts r × Readout / H after the first.'),
    opt('frameRate', 'Frame Rate', FRAME_RATE_NAMES, 7, 'Camera',
      'The camera\'s rate against the wall\'s refresh. The bands stand still or crawl depending on how the two divide.'),
    bool('bayer', 'Bayer On', 1, 'Camera',
      'An RGGB mosaic and a plain bilinear demosaic. It is what turns a luminance beat into a coloured one.'),

    std('faultRate', 'Fault Rate', 0, 'Faults',
      (v) => `${Math.round(controls.faultRate(v) * 100)}%`,
      'The fraction of cabinets that carry a fault of a kind whose amount is 1. Each kind below is both how often and how badly.'),
    std('dead', 'Dead', 0.3, 'Faults', null, 'Cabinets that are simply off.'),
    std('dim', 'Dim', 0.5, 'Faults', null, 'Cabinets at a lower gain, between a quarter and three quarters of the amount down: a visible seam.'),
    std('deadRow', 'Dead Row', 0.3, 'Faults', null, 'One row of one module out, cabinet-wide.'),
    std('binShift', 'Bin Shift', 0.5, 'Faults', null, 'A different colour bin: each channel a shade off, up to a fifth of the amount either way.'),
    integer('faultSeed', 'Fault Seed', 1, 'Faults', '',
      'Which bad day, 0 to 999. A cabinet\'s faults are a pure function of the seed and its position, so a seed reproduces exactly.'),

    std('mix', 'Mix', 1, 'Output', (v) => `${Math.round(v * 100)}%`,
      'Against the untouched input, so it is a direct A/B with the source.'),
  ],

  sources: ['scene', 'grid', 'ramp', 'bars', 'detail', 'spot'],

  // Combinations chosen for this page. The plugin ships no factory presets, so
  // every value is a real control at a real position, but the choice is ours.
  presets: {
    'Moiré at one LED per pixel': { scale: controls.cameraScaleParam(1.0) },
    'Moiré, killed by Focus': { scale: controls.cameraScaleParam(1.0), focus: controls.focusParam(1.5) },
    'Moiré, five-pixel fringe': { scale: controls.cameraScaleParam(0.8) },
    'Whole-period shutter (no bands)': { shutter: controls.shutterParam(1 / 960) },
    'Crawling bands at 59.94': { frameRate: 6 },
    'Scrambled PWM': { pwm: 1 },
    'Slow refresh, deep bands': { refresh: controls.refreshParam(960), scan: 3 },
    'Pixels opening up': { fill: controls.fillParam(0.15), scale: controls.cameraScaleParam(0.125) },
    'Discrete RGB chips, close up': { layout: 1, scale: controls.cameraScaleParam(0.125) },
    'A bad day': { faultRate: 0.35, dead: 0.4, dim: 0.7, deadRow: 0.6, binShift: 0.8 },
    'Fine pitch, one degree off': { pitch: controls.pitchParam(1.25), scale: controls.cameraScaleParam(0.8), rotation: 0.55, scan: 2, shutter: controls.shutterParam(1 / 1600), faultRate: 0.2 },
    'Identity (the neutral settings)': {
      pitch: controls.pitchParam(1.0), fill: 1, scale: controls.cameraScaleParam(1.0), bayer: 0,
      shutter: controls.shutterParam(1 / 960),
    },
  },

  differences: [
    'The CPU half is a hand port, and nothing checks it but a reader: every slider conversion from Controls.cpp, the wall\'s cabinet count and origin, the per-row exposure windows in PWM sub-periods and their split into a whole and a fraction, and the frame-period estimate, all out of Pitch::ProcessOpenGL. demo/tools/check_shaders.py only compares the five shaders, and it fails the repository\'s verify script if a character of them drifts from source/Shaders.cpp.',
    'The integer controls are sliders. Cabinet W, Cabinet H, Module Rows, Grey Bits and Fault Seed are FF_TYPE_INTEGER in the plugin, which a host shows as a number to type; the kit here has no integer control, so each is a slider that lands on every integer in the plugin\'s own range and shows it. A shared link carries the slider position rather than the integer.',
    'The clock is the browser\'s. The plugin votes on whether the host counts in seconds or milliseconds before it trusts the host\'s clock; a browser\'s is unambiguous, so that half is absent. The frame-period estimate that turns host frames into camera frames — and so decides whether the bands stand still or crawl — is ported as it stands, and runs off this page\'s frame rate, which is your display\'s and not necessarily 60. Pause freezes the bands; Step moves them one sixtieth of a second.',
    'JavaScript has no float. The plugin\'s control conversions are 32-bit in C++ and 64-bit here, so a Camera Scale or a Shutter agrees with the plugin to about seven significant figures rather than exactly. The row windows are computed in double on both sides and rounded to float the same way before the shader sees them.',
    'On a browser that cannot filter a 32-bit float texture (no OES_texture_float_linear — many phones), the wall is kept at RGBA16F instead of the plugin\'s RGBA32F, and a line under the picture says so. The sensor pass and the row table are as the plugin has them.',
    'The presets are this page\'s own combinations. The plugin ships no factory presets; every value is a real control at a real position, but the choice is ours. The "Identity" one is the plugin\'s neutral settings, which return the input byte for byte in the plugin\'s harness — here the browser\'s rounding is not the plugin\'s, so treat it as a picture, not a proof.',
    'The About block is absent: it is four buttons that open a browser.',
    'The plugin\'s numerical proof — the moiré fringe period by DFT, the scan-band period and a whole-period shutter leaving no band, the band depth against the closed-form overlap, the optics conserving light, dead cabinets exactly black and aligned, and the neutral settings returning the input byte for byte — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});
