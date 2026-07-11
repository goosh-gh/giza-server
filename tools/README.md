# tools/

Standalone development utilities for giza-server. Everything here runs
independently of the giza-server display server itself.

## Files

- **pdl_3d_wx.pl** — Standalone 3D viewer built on PDL + wxWidgets. Runs without
  starting giza-server; intended for quick 3D checks such as electrode layouts.
  Supports point-cloud, wireframe-grid, and electrode (10-20) modes. Mouse drag
  rotates via a quaternion trackball (pole-free — you can rotate continuously
  past straight-up / straight-down); wheel zooms; `P` toggles
  perspective/orthographic; `R` resets to the standard topomap view
  (looking straight down: Fp top, T3 left, T4 right, Cz nearest).
- **verify_wx_projection.pl** — Headless check of pdl_3d_wx.pl's projection
  convention (screen-Y flip) and trackball rotation (identity seed, rigidity,
  pole-free continuity). Depends only on `Test::More` (no PDL/Wx required).
- **standard_1020.elc** — International 10-20 standard electrode coordinates
  (ASA `.elc` format, 97 points, RAS frame). Used by the verifier to check the
  canonical invariants (Fp top / T3 left / T4 right / Cz nearest) against real
  positions.
- **foreground_giza_server.sh** — Helper to launch giza-server in the foreground.

## Building the Wx dependency (important)

pdl_3d_wx.pl loads a locally built Wx (wxPerl) from `tools/wx/`. **`tools/wx/` is
a build artifact and is gitignored, so it is not included in a clone.** On first
use you must build it yourself so that `tools/wx/{lib,arch}` exist.

- Build instructions: see `~/pdl_kb/build/macos_build_notes.md`
  (MacPorts / Apple Silicon, dyld install_name notes, etc.).
- Outputs: `tools/wx/lib` (Wx.pm and friends) and `tools/wx/arch`
  (Wx.bundle and other XS objects).

## Running

pdl_3d_wx.pl adds `tools/wx/{lib,arch}` to `@INC` automatically via `FindBin`,
so it can be launched with no extra include flags:

    perl tools/pdl_3d_wx.pl --mode electrode
    # modes: scatter | grid | electrode

To pass the paths explicitly instead of relying on FindBin:

    perl -I./tools/wx/lib -I./tools/wx/arch tools/pdl_3d_wx.pl --mode electrode

If the XS loads but the wxWidgets dylibs are not found by dyld
(`Library not loaded: libwx_...`), set `DYLD_LIBRARY_PATH` accordingly
(location is noted in macos_build_notes.md).

## Verifying

    perl tools/verify_wx_projection.pl --src tools/pdl_3d_wx.pl --elc tools/standard_1020.elc
    # -> 1..15, all ok

## Note: relationship to GS3D

pdl_3d_wx.pl (standalone) and PDL::Graphics::Cairo::Driver::GS3D (the
giza-server-integrated 3D driver) fold the projection conventions in opposite
ways:

- **wx**  : no screen-x reversal + screen-y negated in projection
  -> trackball seed = **identity quaternion (1,0,0,0)**
- **GS3D**: screen-x reversed + seed **(0,0,0,1) = diag(-1,-1,1)**

Both produce the same on-screen canonical orientation (Fp top / T3 left /
T4 right), but **the seeds are not interchangeable between the two** — do not
copy one seed into the other.
