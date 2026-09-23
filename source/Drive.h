#pragma once

/**
	The wall's drive, as closed-form time.

	An LED wall is lit in time as well as space. A multiplexed driver lights
	one scan group at a time, and it makes grey by pulse-width modulation. The
	emission of one LED is therefore a periodic pulse train, and what a camera
	row sees is the OVERLAP of its exposure window with that train. This file
	is the arithmetic of that overlap, in double, on the CPU. The GLSL in
	`Shaders.cpp` evaluates the same closed form per LED; this copy exists so
	the plugin can hand the shader frame-relative row windows that are exact,
	and so the harness can state what a band's depth ought to be.

	Units. Everything here is in SUB-PERIODS. The refresh period is `1 / R`
	seconds; Conventional PWM puts one pulse per refresh, so the sub-period is
	the refresh period; Scrambled splits the on-time into `M` equal pulses
	spread evenly through the refresh, so the sub-period is `1 / ( R * M )`
	and the pulse is `M` times shorter. In either case, within every
	sub-period scan group `g` of `S` owns the slot `[ g / S, ( g + 1 ) / S )`,
	and a grey level `q` (quantised to the grey bits) is a pulse of width
	`q / S` at the start of that slot.

	The cumulative on-time of that train from 0 to `u` is

	    F( u ) = floor( u ) * w + min( frac( u ), w ),   w = q / S

	for the group whose slot starts at 0; for group g, shift u by g / S. The
	overlap of a window `[ u0, u1 ]` is `F( u1 ) - F( u0 )`, and the mean over
	all phases is `( u1 - u0 ) * w`. Dividing the overlap by that mean and
	multiplying by q gives the level the camera reports, normalised so that a
	window an integer number of sub-periods long reports exactly q.

	Closed form, not sampled: a sampled comparator errs in one direction, and
	the operator's rule that a shutter which is a whole number of refresh
	periods shows no banding has to fall out EXACTLY, not approximately.
*/
namespace pitch::drive
{

/// Cumulative on-time, in sub-periods, of a pulse train of width `w` (a
/// fraction of the sub-period) whose pulse begins every sub-period at 0.
double cumulative( double u, double w );

/// On-time caught by the window [u0, u1] by the group whose slot begins at
/// `slot` (a fraction of the sub-period), pulse width `w`.
double overlap( double u0, double u1, double slot, double w );

/// The level the camera reports: overlap normalised so that a window of an
/// integer number of sub-periods reports exactly `q`. `scanGroups` is S.
double emission( double u0, double u1, double slot, double q, int scanGroups );

/// The extremes of the overlap over every possible phase, for a window `e`
/// sub-periods long and a pulse of width `w`. `e = n + r` with n whole:
/// the window always holds n whole pulses, and the remainder r catches
/// between max( 0, w + r - 1 ) and min( w, r ) of one more.
void overlapRange( double e, double w, double& lo, double& hi );

/// Grey level quantised to `bits` bits: round( q * ( 2^bits - 1 ) ) / ( 2^bits - 1 ).
double quantise( double level, int bits );

/// The number of grey levels less one, as the shader wants it.
double greyLevels( int bits );

} // namespace pitch::drive
