# RE6Tweaks

Graphics and camera tweaks for **Resident Evil 6** (Steam, `BH6.exe`), as a ReShade add-on.

**0.0.1 is a probe.** It changes nothing in the game. It maps the engine's camera and render
objects so options (field of view, camera distance, shadows, render settings) can be built next.

## Requirements

- Resident Evil 6 is a **32-bit D3D9** game. It needs the **32-bit** ReShade 6.8.0 with add-on
  support, installed as `d3d9.dll` next to `BH6.exe`. A 64-bit ReShade (or any `*.addon64`)
  does not load into this game.
- Copy `RE6Tweaks.addon32` next to `BH6.exe`.

## Probe use

- The add-on scans the engine's singletons 20 s after the first frame and writes
  `RE6Tweaks-probe\statics.txt` in the game folder.
- **Ctrl+F9** (or the overlay button) writes `RE6Tweaks-probe\snapshot-NN.txt`: every field of
  the camera and render singletons and the camera objects they point to. Take one while
  walking, one while aiming and one in a cutscene so they can be compared.
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
