# RE6Tweaks

Graphics and camera tweaks for **Resident Evil 6** (Steam, `BH6.exe`), as a ReShade add-on.

**0.6.0**: startup crash fixes, field of view and camera distance for the gameplay camera,
shadow resolution, distance and filtering, draw distance for effects and grass, ambient
occlusion and god rays quality, film grain / depth of field / radial blur / bloom switches,
and texture filtering and sharpness. It also carries the probe tools used to map the engine.

## Options (ReShade overlay, RE6Tweaks tab; saved in ReShade.ini [RE6Tweaks])

- **Field of view**: Game, +10% to +50%. Widens the gameplay camera, walking and aiming (aiming
  keeps its relative zoom). Cutscenes are unchanged. Applies at once.
- **Camera distance**: Game, 1.15x to 1.75x. Moves the gameplay camera back along its view line,
  so the crosshair stays on target. The game's wall collision uses the normal distance, so far
  settings can clip into walls in tight spaces. Cutscenes are unchanged.

- **Shadow resolution**: Game, 2x, 4x (up to 8192). Multiplies every shadow map the game uses
  (sun 512/1024, lamps and flashlight 512/1024). Applies at once, in gameplay.
- **Shadow distance**: Game, 1.5x, 2x. Shadows drawn further out; with sun shadows it spreads
  the map over a larger area, so pair it with a higher resolution.
- **Shadow filtering**: Game, Filter 2, Filter 3. The shadow shaders support four edge filters
  (mPCFType 0-3); the game uses 1.
- **Draw distance**: Game, 1.5x, 2x, 3x. RE6 has no global model LOD distance (each model class
  decides its own); its distance limits are the effects cull distances (sEffect, 8 tiers from
  10 m to 200 m, stored with their squares) and grass fade/LOD distances (sGrass, fade 10-300 m,
  LOD 10-80 m). Both are scaled from the game's values. Applies at once, in gameplay.
- **Ambient occlusion**: Game, Full resolution, Full resolution + smoother (2x blur taps). The
  game's uAmbientOcclusionFilter (resolution enum 0 = screen size .. 3 = quarter). Only where a
  level creates that filter; none existed in Leon chapter 1, so it is untested in game. The
  overlay shows how many AO and god rays filters are active.
- **God rays**: Game, Smoother (2x iterations). uGodRaysFilter / uMultiGodRaysFilter; same
  caveat, untested in game.
- **Film grain, Depth of field, Radial blur, Bloom**: Game or Off. Screen effects the game's
  menu doesn't control (an effect census in Leon's chapter found TV noise, bokeh DOF, bloom and
  radial blur in gameplay; chromatic aberration only on the title/pause menus). Off clears the
  unit Draw flag (cUnit +0x10 bit 0x800) atomically; Game turns back on only what was turned off.
- **Texture filtering**: Game, 16x anisotropic. Surface textures (wrapping samplers with linear
  filtering and mips) only; full-screen passes and shadow maps keep the game's filtering.
- **Texture sharpness**: Game, -0.5, -1 mip bias on the same samplers. -1 can shimmer.

Textures: RenoDX builds its D3D11 samplers inside its D3D9 proxy, out of reach of ReShade's
create_sampler event, so RE6Tweaks hooks IDirect3DDevice9::SetSamplerState (vtable slot 69) on
the device the game gets and changes the filter where the game sets it. All options apply at once.

Shadows: the game's shadow units (uStableShadow, uSpotShadow, uLightSpaceShadow,
uUniformShadow) are tracked through their own vtables (each entry records the unit, the
destructor forgets it), so only live units are touched; the game thread applies the map size
through the game's own setter (0x1039680, which rebuilds the maps) and the view distance at
+0x58. Units whose map size the game keeps resetting are left alone.

How: each frame the gameplay camera (uCameraCtrl) writes its field of view, position and
look-at point (code at 0x60DF18). A jump at 0x60DF70 adjusts them right after; they are
recomputed every frame, so nothing builds up. Checked against the exe timestamp 5a543b8a and the
original bytes.

## Requirements

- Resident Evil 6 is a **32-bit D3D9** game. It needs the **32-bit** ReShade 6.8.0 with add-on
  support, installed as `d3d9.dll` next to `BH6.exe`. A 64-bit ReShade (or any `*.addon64`)
  does not load into this game.
- Copy `RE6Tweaks.addon32` next to `BH6.exe`.

## Startup crash fixes

- **Too many display modes.** RE6 copies every D3D9 display mode into a 256-entry table without
  checking the size. Displays with DSR/DLDSR and high refresh rates report more (377 here), and
  the game crashes at startup, with or without mods (BH6.exe+0xb429f6). RE6Tweaks hands the game
  a shorter list (every resolution at its top refresh rate, plus 60/120/144/165 Hz, at most 128
  modes) and puts d3d9 back as it was once the game creates its device.
- **RenoDX alignment crash.** RenoDX for RE6 (build 2026-09-21) clears a heap object with
  16-byte-aligned stores, but 32-bit `HeapAlloc` only guarantees 8 bytes, so it crashes at
  random (renodx-residentevil6.addon32+0x72a9c). RE6Tweaks makes that add-on's heap blocks
  16-byte aligned. It only touches that exact RenoDX build.

Both need the add-on to load at process start. Add this to `ReShade.ini`:

```
[ADDON]
LoadFromDllMain=RE6Tweaks.addon32
```

(`LoadFromDllMain` takes add-on file names; `1` is read as a file called "1".)

## Probe use

- The add-on scans the engine's singletons 20 s after the first frame and writes
  `RE6Tweaks-probe\statics.txt` in the game folder.
- **Ctrl+F9** (or the overlay button) writes `RE6Tweaks-probe\snapshot-NN.txt`: every field of
  the camera and render singletons and the camera objects they point to. Take one while
  walking, one while aiming and one in a cutscene so they can be compared.
- **Ctrl+F8** traces which code writes the camera fields for 2 s (hardware data breakpoints).
- The overlay's effect census lists the screen-effect filters alive right now (logged too).
- **Ctrl+F10** measures the frame rate for 10 s.
- Log: `RE6Tweaks.log` in the game folder.

## How it finds things

The game runs on Capcom's MT Framework. Every engine object has a virtual `getDTI()` whose code
is `mov eax, <type record>; ret`, and the type record holds the class name at +4. The probe
reads (never calls) that code to name any object, so it finds `sCamera`, `sRender`, `sShadow`
and others without hard-coded addresses. Offline helpers: `tools\dti.py` (type records and
`getDTI` functions in the exe), `tools\vt.py` (vtables and constructors).

Exe facts (Steam build, 2026-11-23): image base 0x400000, no DRM wrapper. Singleton instance
pointers sit 4 bytes before their type record for `sMain` (0x186e1b4), `sScene` (0x186e280) and
`sShadow` (0x1870774); `sCamera` and `sRender` do not follow that pattern.

## Build

`.\build.ps1 [-Install]` (Visual Studio 2022+ with C++ x86 tools).
