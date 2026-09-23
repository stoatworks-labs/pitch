#include "Drive.h"

#include <algorithm>
#include <cmath>

namespace pitch::drive
{

double cumulative( double u, double w )
{
	const double whole = std::floor( u );
	return whole * w + std::min( u - whole, w );
}

double overlap( double u0, double u1, double slot, double w )
{
	return cumulative( u1 - slot, w ) - cumulative( u0 - slot, w );
}

double emission( double u0, double u1, double slot, double q, int scanGroups )
{
	const double e = u1 - u0;
	if( e <= 0.0 )
		return 0.0;
	const double w = q / scanGroups;
	return overlap( u0, u1, slot, w ) * scanGroups / e;
}

void overlapRange( double e, double w, double& lo, double& hi )
{
	const double n = std::floor( e );
	const double r = e - n;
	lo             = n * w + std::max( 0.0, w + r - 1.0 );
	hi             = n * w + std::min( w, r );
}

double greyLevels( int bits )
{
	return static_cast< double >( ( 1u << std::clamp( bits, 1, 16 ) ) - 1u );
}

double quantise( double level, int bits )
{
	const double levels = greyLevels( bits );
	return std::floor( std::clamp( level, 0.0, 1.0 ) * levels + 0.5 ) / levels;
}

} // namespace pitch::drive
