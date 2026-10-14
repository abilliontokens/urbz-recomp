# The Urbz: Sims in the City - static recompilation

An unofficial native PC port of the original Xbox release of *The Urbz: Sims in the City* (USA),
produced by statically recompiling the game's x86 XBE with the
[XboxXBERecomp SDK](https://github.com/TGP482/XboxXBERecomp) (vendored in `sdk/`).

This repository contains **no game code and no game assets**. It holds tooling, configuration and
host-side glue only. You must supply your own legally acquired copy of the disc.

## Status

| Stage | State |
| --- | --- |
| Disc image read and extracted (`tools/xiso_extract.py`) | done - 44 files, see `docs/iso-extract.md` |
| XBE identified and analysed (`tools/xbe_info.py`) | done - see `docs/xbe-analysis.md` |
| XDK build matched against the SDK's signature set | done - XDK 5849, supported by `sdk/xdk/5849` |
| Toolchain (clang 23.1.3 + ninja + cmake) | done - see `docs/toolchain.md` |
| Repo scaffolded from the SDK template | done |
| `config/game.toml` written by `xbe_symbols` | see `docs/status.md` |
| Recompilation and native build | see `docs/status.md` |

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
src/common/    host-side code: game_info.cpp defines app::kGame
src/game/      hooks and overrides for recompiled functions (src/game is excluded from the host glob)
tools/         xiso_extract.py (XDVDFS reader), xbe_info.py (XBE reader), env.bat (toolchain PATH)
generated/     recompiler output (never committed)
```

## Setup

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
