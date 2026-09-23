# pitch

An LED wall seen through a camera, as an FFGL **effect** for Resolume Arena/Avenue.
C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the sensor pass, the row table or the drive model.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/pitest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Camera Scale=0.4" --set "Bayer On=0" --set "Cabinet W=64"`
  (0..1 for sliders, the real integer for integers, the element index for options)
- List parameters, kinds, defaults and ranges: `./build/pitest --list`
- Other sources: `--source flat --level 128`, `--source white`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH` and an optional `--script` of timed `frame Name Value` cues:
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/pitest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`
  A cue line is `frame  Parameter Name  value` (`#` starts a comment), in the same
  units as `--set`. Values interpolate linearly between a name's cues and hold
  before the first and after the last, so a step needs two cues a frame apart —
  an option index interpolated is a different option on the way. Frame *n* is
  clocked at `n / --fps` (default 60). An unknown name exits 2 before any frame; a
  partial frame at EOF ends the stream with exit 0.

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check at 320x180 AND
  1280x720 + the sweep + the bundle)
- The neutral settings return the input byte for byte: `./build/pitest --identity`
- The optics conserve light: `./build/pitest --energy`
- The fringe period is `1/|s - round(s)|` px and Focus kills it: `./build/pitest --moire`
- The band period is `P/(Tr/H)` rows and a whole-period shutter has none: `./build/pitest --bands`
- Band depth against the closed-form overlap: `./build/pitest --pwm`
- Dead cabinets exactly black and aligned; a seed reproduces: `./build/pitest --faults`
- The mosaic samples each colour where it says: `./build/pitest --bayer`
- The checks can fail: `./build/pitest --negative`
- Every check takes `--size WxH`; CI runs them at 320x180
- `--pipe` keeps the fleet frame format: `tools/verify.sh` feeds it 2.5 frames and wants
  exactly 2 back, and feeds it a cue naming no control and wants exit 2
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/pitest --bench` (best of three; the GPU here is shared)
- What a host sees: `../oxbow/build/oxbow probe build-universal/Pitch.bundle`

## Notes
- **Two samplings.** The sensor pass does both: an analytic box over the LED grid
  per pixel (space) and a closed-form overlap with the PWM pulse train per row
  (time). `Pitch.cpp` only converts controls into the shader's units and fills the
  per-row window table in double. A wrong fringe or a wrong band is a GLSL fix.
- **Nothing absolute crosses into GLSL.** The row table carries each row's window as
  `( floor, frac )` pairs in sub-periods, with the frame's whole sub-periods dropped
  in double on the CPU. Resolume's clock has been seen at 499,217 s.
- **Every overlap is taken in the LED's own coordinates**, and every pixel box is
  built from mapped pixel EDGES. At the wall's magnitude a float resolves 1e-5 of an
  LED, and building boxes as centre ± half leaves a gap of that size at every pixel.
- **`OverlapDetune` and `BayerPhase` are test hooks**, always 0 in the plugin; they
  exist so `--negative` can prove the checks fail.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen
  it, so every slider is 0..1 and `Controls.cpp` holds the units, with inverses.
  `FF_TYPE_INTEGER` is exempt: Cabinet W/H, Module Rows, Grey Bits and Fault Seed
  hold their real values. Options are mapped by index in `Controls.cpp`.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `pitch_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `PI01`, display name `SW Pitch`.

## Not done yet
- **Never loaded into Resolume on macOS.** Everything numeric is measured offline on
  macOS, plus an `oxbow` load. The Windows CI build passed the Arena gate 9/9 on
  win-lab (Arena 7.27.1, llvmpipe) on 2026-09-23.
- No OpenFX port, no factory presets. The browser demo is below. The user guide is
  `docs/USER-GUIDE.md`; every claim in it is read from the code, so change both together.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies; `guide` is set
  to the fleet URL so the About button count is final at v0.1.0.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/pitch/pitch.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\pitch\logs\pitch.YYYY-MM-DD.log   (Windows)

## Browser demo

`demo/` is a static page at **pitch-demo.stoatworks-labs.com**, deployed by
`cf-run npx wrangler deploy` from the repo root (`wrangler.toml`, a
static-assets-only Worker — no build step, no Pages, no `_redirects`).

- `demo/vendor/` is vendored from
  `infrastructure/stoatworks-backend/resolume-demo/kit/` and is **not** a place
  to edit. Re-sync with
  `~/Projects/infrastructure/stoatworks-backend/resolume-demo/sync.sh pitch`
  and confirm it says `synced pitch` rather than skipping.
- The five shader constants in `demo/plugin.js` are `source/Shaders.cpp`
  verbatim, and `demo/tools/check_shaders.py` (run by `verify.sh`) fails on a
  single character of drift.
- **Everything else in `demo/plugin.js` is a hand port** of `Controls.cpp`,
  `drive::greyLevels`, and the wall/row-window/`split()`/frame-period half of
  `Pitch::ProcessOpenGL`, and **nothing checks it but a reader.** Change the
  row-window arithmetic in C++ and it has to be changed there too.
- Absent from the page and said so on it: the host-clock unit voting and the
  About block. The five FF_TYPE_INTEGER controls are 0..1 sliders over the
  plugin's own ranges. No audio caveat — Pitch has no audio path.
