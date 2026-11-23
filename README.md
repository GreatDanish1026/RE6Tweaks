# RE6Tweaks

Graphics and camera tweaks for **Resident Evil 6** (Steam, `BH6.exe`), as a ReShade add-on.

**0.0.1 is a probe.** It changes nothing in the game. It maps the engine's camera and render
objects so options (field of view, camera distance, shadows, render settings) can be built next.

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
