#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace pitch::controls
{
namespace
{
inline float clamp01( float value )
{
	return std::min( std::max( value, 0.0f ), 1.0f );
}

inline float lerp( float from, float to, float t )
{
	return from + ( to - from ) * clamp01( t );
}

inline float unlerp( float from, float to, float value )
{
	return clamp01( ( value - from ) / ( to - from ) );
}

/// Geometric interpolation. Equal slider movements are equal *ratios*.
inline float geometric( float from, float to, float t )
{
	return from * std::pow( to / from, clamp01( t ) );
}

inline float ungeometric( float from, float to, float value )
{
	return clamp01( std::log( value / from ) / std::log( to / from ) );
}

constexpr float kPi = 3.14159265358979323846f;

const char* const kScanNames[ kScanCount ] = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };

const float kFrameRates[ kFrameRateCount ]             = { 23.976f, 24.0f, 25.0f, 29.97f, 30.0f, 50.0f, 59.94f, 60.0f };
const char* const kFrameRateNames[ kFrameRateCount ] = { "23.98", "24", "25", "29.97", "30", "50", "59.94", "60" };

inline int optionIndex( float optionValue, int count )
{
	return std::clamp( static_cast< int >( std::lround( optionValue ) ), 0, count - 1 );
}
} // namespace

float PitchPixels( float value )
{
	return geometric( 1.0f, 16.0f, value );
}

float PitchParam( float pixels )
{
	return ungeometric( 1.0f, 16.0f, pixels );
}

float FillFactor( float value )
{
	return lerp( 0.05f, 1.0f, value );
}

float FillParam( float fill )
{
	return unlerp( 0.05f, 1.0f, fill );
}

float RefreshHz( float value )
{
	return geometric( 240.0f, 7680.0f, value );
}

float RefreshParam( float hz )
{
	return ungeometric( 240.0f, 7680.0f, hz );
}

int ScanGroups( float optionValue )
{
	return 1 << optionIndex( optionValue, kScanCount );
}

const char* ScanName( int index )
{
	return kScanNames[ std::clamp( index, 0, kScanCount - 1 ) ];
}

int SubPeriods( float optionValue )
{
	return optionIndex( optionValue, 2 ) == 1 ? kScrambledSubPeriods : 1;
}

float CameraScale( float value )
{
	return geometric( 0.125f, 8.0f, value );
}

float CameraScaleParam( float ledsPerPixel )
{
	return ungeometric( 0.125f, 8.0f, ledsPerPixel );
}

float RotationRadians( float value )
{
	//( value - 0.5 ) * 20 is exactly zero at 0.5, which the identity and the
	//moire checks rely on: a rotation of 1e-8 rad would switch the sensor
	//pass off its exact axis-aligned coverage path.
	return ( clamp01( value ) - 0.5f ) * 20.0f * kPi / 180.0f;
}

float RotationParam( float degrees )
{
	return clamp01( degrees / 20.0f + 0.5f );
}

float FocusRadius( float value )
{
	if( value <= 0.0f )
		return 0.0f;
	return geometric( 0.05f, 4.0f, value );
}

float FocusParam( float pixels )
{
	if( pixels <= 0.0f )
		return 0.0f;
	return ungeometric( 0.05f, 4.0f, pixels );
}

float ShutterSeconds( float value )
{
	return 1.0f / geometric( 24.0f, 8000.0f, value );
}

float ShutterParam( float seconds )
{
	return ungeometric( 24.0f, 8000.0f, 1.0f / seconds );
}

float ReadoutSeconds( float value )
{
	return geometric( 0.004f, 0.040f, value );
}

float ReadoutParam( float seconds )
{
	return ungeometric( 0.004f, 0.040f, seconds );
}

float FrameRateHz( float optionValue )
{
	return kFrameRates[ optionIndex( optionValue, kFrameRateCount ) ];
}

const char* FrameRateName( int index )
{
	return kFrameRateNames[ std::clamp( index, 0, kFrameRateCount - 1 ) ];
}

float FaultRate( float value )
{
	return clamp01( value );
}

} // namespace pitch::controls
