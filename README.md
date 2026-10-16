# The Urbz: Sims in the City - static recompilation

An unofficial native PC port of the original Xbox release of *The Urbz: Sims in the City* (USA),
produced by statically recompiling the game's x86 XBE with the
[XboxXBERecomp SDK](https://github.com/TGP482/XboxXBERecomp) (vendored in `sdk/`).

This repository contains **no game code and no game assets**. It holds tooling, configuration and
host-side glue only. You must supply your own legally acquired copy of the disc.

## Run it on Windows

You supply the game: the USA Xbox release, as a disc image or as the files from one.

1. Download `Urbz.zip` from [Releases](https://github.com/abilliontokens/urbz-recomp/releases/latest)
   and unzip it anywhere.
2. Put your disc image in the `assets` folder beside `Urbz.exe`, so you have
   `Urbz.exe` and `assets\<anything>.iso`. A whole-disc dump or a plain xiso both work, and so do
   extracted files (`default.xbe` with its `DATA` and `movies` folders beside it).
3. Run `Urbz.exe`. It mounts the image as the console's `D:` and boots the game.

You need Windows 10/11 x64 and a Direct3D 12 GPU. Play it with a controller (Xbox, PlayStation and
Switch pads all work, through SDL) or with the keyboard and mouse - see **Controls** below. The first
run writes `settings.toml`, `logs\` and `hdd\` (where saves go) beside the exe. In game, **F1** opens
settings (display mode, resolution, aspect ratio, ultrawide, vsync, frame cap, render scale,
anisotropic filtering) and **F2** the frame-time graph.

Any frame rate runs the game at its real speed, including uncapped on a high-refresh display. The
engine quantised its frame step to whole 60ths of a second, which made it run 2.4x fast at 144 fps;
the host replaces that quantiser with the time that really passed (`src/game/hooks.cpp`, measured at
1.000 game seconds per real second at 60, 144 and unlimited).

If it says the game is missing, the `assets` folder has no image or extracted `default.xbe` in it.
The exe only runs the XBE it was recompiled from - SHA-1 `5712b6a3...`, USA retail; any other build
is offered as a warning you have to confirm, and will not run properly if you take it.

## Controls

The game itself only ever reads an Xbox pad, so keyboard and mouse are translated into one on port 0
(`src/common/input_kbm.cpp`). A controller and the keyboard can be used at the same time; neither
takes input away from the other. Up to four controllers are picked up in the order they are plugged
in, and the keyboard is always port 1.

| Keyboard / mouse | Pad | In game |
| --- | --- | --- |
| `W` `A` `S` `D` | left stick | walk; menus that read the stick |
| arrow keys | D-pad | menu and dialog navigation |
| mouse, right button held | right stick | camera, deflected from where you pressed: rotate and zoom |
| `Q` / `E` | right stick left/right | rotate the camera |
| `R` / `F` | right stick up/down | zoom the camera in / out |
| mouse wheel | right stick up/down, in pulses | zoom the camera in / out |
| space | A | accept, interact |
| escape, backspace | B | cancel, back |
| `X` / `Y` | X / Y | |
| `Z` / `C` | left / right trigger | |
| `1` / `2` | black / white | |
| enter | Start | "press Start", pause menu |
| tab | Back | |
| shift / ctrl, middle mouse | left / right stick click | |

The camera is the right stick and nothing else: the game turns on its X axis and zooms on its Y,
which is also what moves its height and field of view - there is no separate tilt. Rotation
direction follows the game's own invert-camera-rotation option.

Keys are read only while the game window has focus, and are dropped when it loses focus, so an
alt-tabbed game does not walk on by itself. While the F1 settings panel is up it owns the keyboard
and the mouse and the game sees neither.

## Status

| Stage | State |
| --- | --- |
| Disc image read and extracted (`tools/xiso_extract.py`) | done - 44 files, see `docs/iso-extract.md` |
| XBE identified and analysed (`tools/xbe_info.py`) | done - see `docs/xbe-analysis.md` |
| XDK build matched against the SDK's signature set | done - XDK 5849, supported by `sdk/xdk/5849` |
| Toolchain (clang 23.1.3 + ninja + cmake) | done - see `docs/toolchain.md` |
| Repo scaffolded from the SDK template | done |
| `config/game.toml` written by `xbe_symbols` | done - 130 of 238 XDK functions located |
| Recompilation and native build | done - 15,942 functions, `Urbz.exe` boots and opens its window, see `docs/status.md` |
| Kernel import coverage (`tools/kernel_coverage.py`) | 123 of 124 covered, see `docs/kernel-coverage.md` |
| Remaining console-surface gaps | see `docs/gap-analysis.md` |

## The game

| Field | Value |
| --- | --- |
| Title ID | `0x45410060` (EA, serial 96) |
| Region | North America |
| XBE | `default.xbe`, 3,035,136 bytes, SHA-1 `5712b6a3e2eaaacfcae228cf6154ade499a0033c` |
| Linked | 2004-10-07 |
| XDK | 1.0.5849 (XAPILIB, XBOXKRNL, LIBCMT, D3D8, XGRAPHC, DSOUND, XMV) |
| Kernel imports | 124 ordinals |

The build is pinned to this exact XBE. A different region or revision needs its own analysis pass.

## Layout

```
assets/        your disc image and/or the files extracted from it (never committed)
config/        game.toml - the recompiler config written by xbe_symbols, then hand-corrected
docs/          analysis and bring-up notes
sdk/           XboxXBERecomp SDK checkout (recompiler, runtime, XDK HLE, D3D12 renderer)
src/common/    host-side code: game_info.cpp defines app::kGame, input_kbm.cpp the keyboard pad
src/game/      hooks and overrides for recompiled functions (src/game is excluded from the host glob)
tools/         xiso_extract.py (XDVDFS reader), xbe_info.py (XBE reader), env.bat (toolchain PATH)
generated/     recompiler output (never committed)
```

## Build it from source

1. Put your disc image in the repository root, or `default.xbe` plus its `DATA` and `movies`
   folders in `assets/`.
2. Extract it:
   ```
   python tools/xiso_extract.py info    "<your>.iso"
   python tools/xiso_extract.py extract "<your>.iso" assets
   ```
3. Check that the XBE is the one this configuration targets:
   ```
   python tools/xbe_info.py assets/default.xbe --json build/xbe_info.json --markdown docs/xbe-analysis.md
   ```
   The SHA-1 must match the table above.
4. Install the toolchain and build:
   ```
   tools\env.bat
   build.bat
   ```
   `build.bat` configures the `release` preset, builds `xbe_symbols`, writes `config/game.toml`
   from the XBE and the SDK's XDK 5849 signatures, then recompiles the XBE and builds `Urbz.exe`
   into `out/build/release`. The first build is long: it compiles one C++ function per guest
   function.

## Working on the port

- Never edit anything in `generated/`; it is rewritten on every recompile.
- Behaviour changes go in `src/game/` as `X86_FUNC(sub_XXXXXXXX)` overrides (call
  `__imp__sub_XXXXXXXX` for the original) or as `[[hook]]` entries in `config/game.toml`.
- Analysis mistakes are corrected in `config/game.toml` with `[[function]]`, `[[switch]]` and
  `[xbe] data_ranges`.
- `python sdk/tools/symbols/xbe2pe.py config/game.toml game.exe` writes a flat PE with the
  recompiled functions as exports, for symbol work in Ghidra.

## Legal

Bring your own disc. Do not commit or distribute the XBE, the disc image, extracted assets, or
any key material. `assets/`, `generated/`, `*.iso` and `out/` are ignored by git for that reason.
The SDK is MIT licensed; see `sdk/LICENSE` and `sdk/THIRD_PARTY.txt`.
