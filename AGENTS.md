# AGENTS.md — Pitch

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

An LED wall seen through a camera, as an FFGL 2.1 effect (`PI01`, shown as
`SW Pitch`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal macOS
`.bundle` and a Windows `.dll`. MIT, intended home `github.com/stoatworks-labs/pitch`.

Built 2026-09-23 in one session from the fleet's templates: readout for the row-time
model, the clock-unit voting and the harness shape; graticule for the cabinet
arithmetic; rosette for the idea of a lattice seen through a sampling grid; tinsel
for `PassBuffer`, the sweep, `verify.sh` and CI.

---

## The one idea

**Two samplings, one in space and one in time.**

The wall is a lattice of small emitters with black between them, lit one scan group
at a time by PWM. The camera is a second lattice (the sensor, behind a Bayer mosaic)
that integrates over an exposure window sliding down the frame. Every sensor pixel
is a box laid over the LED grid; every sensor row is a window laid over the pulse
train. The sensor pass integrates both, and everything an operator fights on a show
is what those two integrals *do*:

| the sampling, applied to | what comes out |
| --- | --- |
| the LED lattice, by the pixel lattice | **moiré**, period `1 / |s − round(s)|` px at *s* LEDs per pixel |
| the same, through the mosaic | **coloured** moiré: R and B sample the fringe a pixel apart |
| the pulse train, by each row's window | **scan bands**, period `P / (Tr / H)` rows |
| a short pulse, by the same window | **low-grey breakup**: relative depth `P / E` until the pulse is long enough to be caught either way |
| the emitter rectangle, by the box | **black between the pixels**, the fill factor |
| a cabinet whose level is zero | **a dead cabinet**, exactly black and exactly aligned |

Both integrals are **closed form**, not sampled. The spatial one is the product of
two one-dimensional overlaps of an axis-aligned box against an axis-aligned emitter,
summed over the LEDs the box touches. The temporal one is
`F( u1 ) − F( u0 )` with `F( u ) = floor( u ) w + min( frac( u ), w )`, the
cumulative on-time of a pulse train of width *w*. A sampled comparator errs in one
direction; the operator's rule that a shutter of whole refresh periods shows no
band has to be a theorem, and here it is one to 1.2e-7.

### What falls out, and what does not

- **The identity is real.** Pitch 1, fill 1, one LED per pixel, a whole-period
  shutter, no Bayer: every stage is an identity and the input comes back byte for
  byte. That is a property of the arithmetic, not a special case in the code.
- **The optics conserve light.** Every pixel box, and every focus-tap-shifted copy
  of every pixel box, tiles the plane, so the frame mean is fill × level × area to
  float precision. This holds *because* the boxes are built from mapped pixel edges
  (see the first trap).
- **Not modelled:** the wall's gamma (the PWM is driven by code values, so real dark
  pulses are shorter still), the emitter's radiation pattern, the lens MTF beyond a
  disc, and the rotated pixel footprint (nine axis-aligned boxes stand in for one
  rotated square). A pixel wider than three LEDs a side reads the wall's mip chain
  times the fill factor instead of walking every emitter; a real lens would have
  blurred that lattice away long before.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Controls.{h,cpp}` | What a 0..1 slider means, in hertz, seconds, LEDs per pixel. Every geometric mapping has an inverse, so a check can ask for exactly one LED per pixel. Options are mapped by index. |
| `source/Drive.{h,cpp}` | The PWM overlap in double: `cumulative`, `overlap`, `emission`, `overlapRange`, `quantise`. The plugin fills the row table with it; the harness states expectations with it. |
| `source/Shaders.{h,cpp}` | Five complete shaders. Nothing is assembled at run time. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed. |
| `source/Pitch.{h,cpp}` | The plugin: parameters, the clock, the wall geometry, the row table, the four passes. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/pitest/` | The offline harness: renders, measures, benchmarks, pipes. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters, plus the release-time checks done locally. |

Four passes:

1. **copy** — the input into a mipmapped texture of ours. Exact at texel centres.
2. **wall** — one texel per LED, RGBA32F. Resamples the picture at `Pitch` source
   pixels per LED through the mip chain (level `log2(Pitch)`; exact at Pitch 1),
   aligned to whole cabinets, with the seeded faults applied as gains.
3. **sensor** — the plugin. Straight to the host when Bayer is off, else to an
   RGBA16F buffer. Per pixel: the row's window from the table, then for every focus
   tap × OLPF copy × sub-box, the analytic light of that box over the LEDs it
   touches, each LED's emission from the closed-form overlap.
4. **demosaic** — RGGB mosaic and a plain bilinear demosaic, nine fetches per
   pixel, then the mix.

---

## Traps

Roughly in the order they will bite.

### ☠️ Pixel boxes built as centre ± half do not tile, and the gaps lose light

The first energy check came out 1.75e-5 low, systematically, at 320×180, and
exactly right at fill 1. Two rounded edges, `c − h` and `c + h`, computed at a
magnitude of ~240 LEDs, each round to a float ULP of 1.5e-5 — and the next pixel's
`c' − h` is a *different* expression, so the shared edge is two different floats and
a gap or overlap of an ULP sits at every pixel. At 4K the ULP is 2.4e-4.

Two fixes, both load-bearing. The box's edges are the **mapped pixel edges**, so two
neighbours compute their shared edge from one expression and the boxes tile
bitwise. And every overlap is taken in the **LED's own coordinates**: `lo − float(i)`
is an exact subtraction (Sterbenz), and `0.5 − EmitHalf` is a small number. After
that the frame mean is off by 2.6e-8. Do not "simplify" either back.

### ☠️ Nothing absolute crosses into GLSL — the row table

Resolume's clock was measured at 499,217 s by ferric. At 7680 Hz and sixteen
sub-periods that is 6e10 sub-periods, which a float resolves to 4096. So the CPU
reduces the frame's start phase in double, drops the whole sub-periods, and hands
the shader **per row** `( floor u0, frac u0, floor u1, frac u1 )` through a 1D
texture. The shader subtracts the whole parts as small integers and keeps the
fractions in [0,1). That is what lets a two-period shutter band to 1.2e-7 rather
than to "about zero".

A first version handed over `Phase0 + row * RowStep` in float; at 40 ms readout and
7680 × 16 sub-periods the ULP was 5e-4 of a sub-period, sixteen per cent of a
10% grey pulse at 1/32 scan. The table costs `H × 16` bytes a frame.

### ☠️ The quarter-pixel OLPF was exactly invisible — and correctly so

The sweep reported OLPF dead at the defaults. It was: the four copies were ±0.25 px
apart, and at four pixels per LED every emitter edge sits further than 0.25 px from
the nearest pixel boundary, so each shifted box still contains the same edge, the
box integral is *linear* in the shift, and the symmetric average is the unshifted
value. Bit for bit. That is not a bug; it is a theorem about box filters, and a
quarter-pixel split is also the wrong filter — the textbook four-spot OLPF
separates the copies by one pixel pitch so its first null lands on Nyquist. The
copies are now ±0.5 px. The sweep is the only reason this was found.

### ☠️ A lattice of phases has a different mean from the continuum

`--pwm` first asserted the column's mean equals the grey level to 1e-4 and failed
by 1.5e-3 at 180 rows. The rows visit phases `frac( r × step )`, a lattice of
spacing 1/9 at that raster, and the lattice mean of a piecewise-linear periodic
function is not its integral. The extremes ARE exact — a plateau wider than the
widest hole in the visited phases holds a sample — so the depth is asserted to
float precision, and the mean to `depth × widest gap`, which is the Riemann-sum
bound. The harness computes the widest gap from the phases actually visited rather
than from `frac( step )`, because a step of 16/9 sub-periods per row visits nine
phases with spacing 1/9, not 7/9.

### ☠️ Refresh Phase 0 and Refresh Phase 1 are the same phase

It runs 0..1 of a refresh period, so the sweep's two ends render identically. Swept
against 0.5. Obvious afterwards; reported as a dead control first.

### ☠️ Cabinet W does nothing visible until something breaks

The wall is centred on the clip, so a different cabinet count moves where the wall
*ends* and nothing else — and the end is outside the frame at the defaults. The
sweep gives it a faults context. Cabinet H happened to sweep live on its own,
because shifting every LED row's index changes which scan group it is in and so
the band phase; that is luck, not coverage, and it has the same context now.

### ☠️ The spec's fringe formula names the wrong quantity

`SPEC-pitch.md` says the fringe period is `1 / |f − round(f)|` with `f = 1 / Camera
Scale`, pixels per LED. The beat is between two *frequencies*: the LED lattice at
*s* = Camera Scale cycles per pixel against the pixel lattice at 1, so the fringe is
`1 / |s − round(s)|`. At *s* = 0.8 that is 5 px; the spec's form would say
`1 / |1.25 − 1|` = 4. The harness measured 5.00 by DFT. The check is written in *s*
and the README says so.

### ☠️ Mutation-test only a committed tree

The mutation was applied with `sed`, checked, and reverted with `git checkout
source/Shaders.cpp` — which also reverted the uncommitted OLPF fix sitting in the
same file. Caught because the sweep was re-run before committing. Commit first,
mutate second.

### ☠️ The bench measures the GPU you have, and this one is shared

Three sibling harnesses were running their own verify at the time; one bench run
reported 30 ms at 4K and the next 11. `--bench` now reports the best of three runs,
which is the one nothing else interrupted, and the README says the GPU was shared.

### `FFGLScopedFBOBinding.h` is not in the umbrella header, and its path has a prefix

`#include <ffglex/FFGLScopedFBOBinding.h>`. The bare name fails to resolve.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and put
back before the final pass); every `ffglex::Scoped*` clears to 0 on exit, so every
`Ensure()` happens before anything binds a texture; `FFGLFBO::Release()` leaks the
colour texture, which is why `PassBuffer::Destroy()` deletes it first; `SetParamInfo`
clamps a STANDARD default into 0..1 before `SetParamRange` can widen it; the core is
an **OBJECT** library; `SetTextParameter` must return `FF_SUCCESS` for the About
block; the harness drives a synthetic clock; `nm | grep -q` fails under pipefail
when grep succeeds; an option's range reads back 0..1 whatever its element count.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at
320×180 and 1280×720 in `verify.sh` and at 1920×1080 by hand.

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--identity` | output bytes == input bytes | **0**. The arithmetic is within a few float ULPs of an identity; a value `k/255 × (1 ± 1e-6)` rounds to `k` unless it sat within 1e-6 of a half code, and no `k/255` does. Not exact cancellation: an 8-bit margin of half a code. | none; cabinets are chosen to tile any raster |
| `--energy` | frame mean vs fill × level × area | **2e-5 relative**, a hundred float ULPs on values near 0.25 accumulated in double. Exact tiling of the boxes (and of every tap-shifted lattice of boxes) makes the expectation exact. | none; the wall sits W/4 px inside the frame at every W |
| `--moire` fringe | DFT peak bin of the central half-row | **exact bin**. The profile length W/2 is a multiple of every expected period (4, 5) at every raster the fleet uses. Contrast floor 0.05 is well under the 0.08–0.13 measured; a whole-pixel geometry, so the fringe is raster-independent. | none |
| `--moire` s = 1 | contrast | **1e-4**: every pixel holds exactly one cell, so the value is one float everywhere | none |
| `--moire` focus | contrast after a 1.5 px disc | **0.05**, from the disc MTF `2J1(2πρν)/(2πρν)` = 0.037 at ν = 0.8, ρ = 1.5, with room for a 32-tap disc; also < 0.3 × sharp | none |
| `--bands` period | DFT peak bin down a column | **exact bin 20** at every H: the period is H/20 rows by construction (P = 1 ms, Tr = 20 ms) | bin is raster-free by design |
| `--bands` depth | max − min vs closed form | **1e-4**, a hundred ULPs; valid because the plateau (0.248) is wider than the widest visited phase gap (0.111 at 180 rows, 0.028 at 720), which the check asserts | the gap shrinks with H; asserted, not assumed |
| `--bands` whole periods | depth | **1e-4**; measured 1.2e-7 | none |
| `--pwm` depth | max − min vs closed form, four cases | **1e-4** on the depth (plateau ≥ gap asserted); mean to **depth × gap**, the Riemann-sum bound for a lattice of phases | the mean bound loosens at coarse rasters, which is right |
| `--faults` | every pixel of every cabinet 0 or 255; rows | **exact**; dead-cabinet count strictly between 0 and all at rate 0.5 | needs a cabinet size that divides the raster; `cabinetSide` finds one for 320/180/1280/720/1920/1080, and the check says so when it cannot |
| `--bayer` | own channel at its site vs Bayer off | **1/255**, from the RGBA16F sensor buffer (2^-11 relative can move an 8-bit code by one); colour moiré ≥ 8/255 against a measured 36 | none; the 1-px border is excluded |
| `--negative` | the three perturbations fail | n/a | none |

Two things are deliberately NOT relied on: exact cancellation (`e` is 1 ± 1e-7 after
the slider round trip, and the identity holds anyway because of the half-code
margin), and `pow` returning exact values (the pitch of 1 is `1 × 16^0`, which is
exact, but nothing else is assumed exact).

What might still differ on another rasteriser: the DFT peak with a software
rasteriser that filters `texelFetch` differently (it should not; it is nearest),
and the disc MTF floor if a driver's `sin`/`cos` in the golden-angle spiral is
poor (the spiral only needs the taps spread, not placed exactly).

---

## Decisions taken without asking

- **The camera's frame rate scales host time.** Camera frame *n* begins at
  `n / FrameRate` of real time, and frame *n* is the host's frame *n*, so camera time
  is host time × (host fps / Frame Rate), with the host fps measured from its own
  deltas as readout does. At 60 on a 60 host the bands are locked; at 59.94 they
  crawl at ~4 periods a second on a 3840 Hz wall, which is what a real 59.94 camera
  does on a 60-locked wall. Frame Rate is an option list so the values are exact.
- **Scrambled PWM is sixteen sub-periods.** Each pulse split into sixteen evenly
  spaced sixteenth-pulses, so the visible refresh is 16 × Refresh. Real drivers
  differ; sixteen is enough to show the effect and the number is one constant.
- **Refresh Phase is a fraction of the refresh period**, added in sub-periods.
- **Each fault control is both rate and severity.** A kind is present in a cabinet
  when its draw is under `Fault Rate × amount`, and for Dim and Bin Shift the amount
  also sets how bad. One control per kind that means "how much of this" read as an
  operator would.
- **Discrete RGB is three strips at three times the radiance.** So a discrete wall
  is as bright as a 3-in-1 wall of the same fill, and a camera that resolves the
  chips sees them saturate, as it does.
- **The wall is a whole number of cabinets, the nearest to what the clip holds, at
  least one, centred.** LEDs whose centre falls outside the clip are lit black.
- **Output alpha is 1.** An LED wall is an opaque object in a camera's picture; the
  black surround is part of the picture, not transparency.
- **No gamma on the wall.** Code values drive the PWM directly. See "what does not
  fall out"; the alternative breaks the byte-exact identity and adds a control.
- **Focus forces one box per pixel.** Rotation uses nine; with a blur on, the taps
  already cost 32× and the blur hides what the sub-boxes would add.
- **Two test hooks live in the shipped shader.** `OverlapDetune` (always 0) and
  `BayerPhase` (always 0). The negative controls need to perturb the *plugin's*
  model, not the harness's expectation, and a uniform that is zero costs nothing.
- **No factory presets**, like readout: the mechanism is well understood elsewhere
  and can be lifted later.
- **Physics checks read a float framebuffer; identity reads 8-bit.** The claim
  decides the format.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4.1 (2026-09-23)

Every number below is `tools/verify.sh` on this machine against a fresh universal
Release build, at 320×180 and 1280×720, with the same checks passing at 1920×1080
by hand.

- **Identity.** 0/255 at every raster; the negative control differs by 128/255.
- **Energy.** Frame mean off by −2.6e-8 (focus 0) and −1.9e-8 (focus 1.5 px)
  against a tolerance of 2.2e-6; before the tiling fix it was −1.95e-6.
- **Moiré.** DFT bin 32 of 160 = 5.00 px at s = 0.8 (contrast 0.126); bin 40 = 4.00
  px at s = 1.25 (0.083); 1.1e-5 contrast at s = 1; 0.0099 after a 1.5 px disc.
- **Bands.** Bin 20 at every raster (9, 36, 54 rows); depth 0.20000 against
  0.20000; 1.2e-7 at two periods.
- **PWM.** 0.07843 / 0.23077 / 0.07541 depth at 10/50/90% grey, each equal to the
  closed form to five places; relative 0.769 > 0.460 > 0.084; Scrambled 0.019.
- **Faults.** 26 of 50 cabinets exactly black at 180 rows, 372 of 800 at 720; 0
  stray pixels; dead rows exactly one row wide; 42 / 127 distinct dim shades;
  seeds reproduce and differ.
- **Bayer.** Own-channel sites 0/255 from the Bayer-off image; |R − B| up to 36/255.
- **Negative controls.** A 10% detune of the overlap fails `--bands` and
  `--energy`; a swapped mosaic phase fails `--bayer`.
- **Mutation.** `( i1 - i0 ) * w` → `( i1 + i0 ) * w` in the shipped overlap:
  caught by `--identity`, `--bands`, `--pwm`, `--energy`; `--moire` passed, which is
  right — it runs at a whole-period shutter and does not depend on the drive.
- **No dead controls**, all 26, with the About block's text and four buttons skipped.
- **Every shader compiles** through `glslc`.
- **The bundle** is universal, exports `_plugMain`, carries `com.stoatworks.ffgl.pitch`,
  ad-hoc signs, and `oxbow` reports `SW Pitch` / `PI01` / `effect` and renders 120
  frames through `plugMain`.
- **Render cost** at the defaults (Bayer on), best of three runs of 60 frames after
  a warm-up, `glFinish` both sides, on a shared GPU:

  | | ms/frame | % of a 60fps frame |
  | --- | --- | --- |
  | 1280×720 | 0.74 | 4.4% |
  | 1920×1080 | 1.29 | 7.7% |
  | 2560×1440 | 2.11 | 12.7% |
  | 3840×2160 | 4.91 | 29.5% |

  Heavier than readout (2.3 ms at 4K): the sensor pass walks up to four LEDs per
  pixel with a closed-form overlap per channel each, then the demosaic reads nine
  texels. Three tenths of a 4K frame is real; an operator stacking effects will
  feel it. Runs minutes apart on this shared GPU moved the 4K figure between 4.9
  and 5.4 ms; the table is the `verify.sh` run that the rest of this section
  reports.

### Assumed, or not done

- ☠️ **Never loaded into Resolume on macOS.** On Windows, v0.1.0's CI build passed the
  fleet Arena gate 9/9 in Arena 7.27.1 on llvmpipe (2026-09-23; 7 time-varying controls
  inconclusive on the gate's still picture). Otherwise everything was compiled,
  rendered and measured offline against the real plugin class in a headless CGL
  context, plus an `oxbow` load. The inspector presentation of 26 controls in five
  groups, the integer fields, and the host's real clock are all untested.
- **The clock-unit voting has only seen the harness's seconds.** It is readout's
  code, which has met Arena; this plugin has met it only on Windows, for one gate run.
- **The Windows build is CI-built** and has run only in the Arena gate on win-lab.
- **Not verified at 4K**, only benchmarked there.
- **No resize-mid-run check.** The plugin holds no state across frames beyond the
  scalar clock, so there is nothing a resize could carry over wrongly; the buffers
  reallocate on size change and were exercised by the bench's four rasters.
- **Rotation is approximate** (nine boxes) and unmeasured beyond the sweep.
- **The coarse path** (a pixel over three LEDs a side) is unmeasured beyond the sweep
  and is an approximation by design.
- **The 32-tap disc** is a fixed golden-angle spiral; its MTF is not the ideal jinc.
  The moiré check measures what it does (0.0099 residual against an ideal 0.037 ×
  0.126 = 0.0047), not what a jinc would.
- **No OpenFX port and no browser demo.** Not required for 0.1.0.
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies**, in
  the shape the fleet's syncs generate; register the project and re-run the syncs
  before the first release. `guide` was set to
  `stoatworks-labs.com/software/pitch/guide/` when `docs/USER-GUIDE.md` was written,
  so the About block has its four buttons now and the parameter count does not
  change when the header is regenerated. That URL, like `page`, does not resolve
  until the site registers the project.
- **Nothing has been through a show.**

---

## Open questions

- **Should the wall apply a gamma before the PWM?** It would make low-grey breakup
  worse and more real, and it would break the byte-exact identity unless the camera
  applied the inverse — which is also what a real camera does. Probably a v0.2
  control, "Wall Gamma", defaulting to 1.
- **Is one pixel pitch the right OLPF split for this camera?** It is the textbook
  four-spot filter; some cameras split by less and rely on the lens. Exposing the
  separation as a control would be honest but is a control nobody would touch.
- **Should Camera Scale be relative to the wall rather than absolute?** At the
  defaults it happens to fill the frame; change Pitch and it no longer does. A
  "fit" that keyed Scale to Pitch would be friendlier and less physical.
- **Sixteen sub-periods for Scrambled**: a real S-PWM driver's number depends on the
  grey depth and the refresh. A control would be more honest than a constant.
- **The spec's `1/|f − round(f)|`** — see the trap. If the spec's author meant *f*
  to be LEDs per pixel the formula is right and only the prose was crossed.

---

## Siblings

- **readout** — the row-time model, the clock-unit voting, the harness and verify
  shape, the CI and release workflows.
- **graticule** — the LED Tiles cabinet arithmetic in integers, and the integer
  parameter declaration.
- **rosette** — a lattice seen through a sampling grid, and the moiré it makes.
- **tinsel** — `PassBuffer`, `sweep.py`, and the fleet's trap list.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
