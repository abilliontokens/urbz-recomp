# Bring-up status

Recorded 2026-10-14. Every number below was observed on this machine; commands are reproducible from the
repository root.

## Where the port stands

| Stage | Result |
| --- | --- |
| Disc image read | 44 files, partition base 0x00000000, `docs/iso-extract.md` |
| XBE identified | title `0x45410060`, retail, XDK 5849, SHA-1 `5712b6a3e2eaaacfcae228cf6154ade499a0033c` |
| XDK signature set | `sdk/xdk/5849` present: `layout.toml` + `signatures.toml` - no new signature set needed |
| `config/game.toml` | written by `xbe_symbols`: 130 of 238 XDK functions and 11 of 12 data symbols located |
| Recompilation | 15,942 functions, 910,100 instructions, 64 partitions |
| Native build | `out/build/release/Urbz.exe`, 16,132,096 bytes, clang 23.1.3 + lld + ninja |
| Launch | boots, finds the game, opens its window, plays the intro movies and renders past them at 60 fps |

## Getting it to draw

Three defects stood between "links and boots" and "picture on screen". Each was found from observed
runtime state, not guesswork.

1. **Deadlock before the first frame.** The engine thread posted command 5 to the game's Renderer
   worker and waited forever on a semaphore (`Renderer+0x80`) that only a GPU push-buffer callback
   releases. The callback is installed by the XDK's `D3DDevice_InsertCallback` (guest `0x24E320`),
   which the SDK neither replaces nor signatures, so its token went into a scratch buffer and was
   dropped. `src/game/hooks.cpp` now replaces that function and runs the callback immediately: the
   host renderer has no GPU timeline for the guest to be behind.
2. **Access violation in XDK tile code.** `sub_0025295F` faulted reading `*this + 0x3214` with
   `*this` NULL - the NV2A tile/miniport registers, reached from `D3DDevice_SetTile`, which
   `xbe_symbols` had left at `0`. The function is at `0x24BC90`: 0xD1 bytes with 0x1EE and 0xC2 byte
   callees, exactly the call cluster `sdk/xdk/5849/signatures.toml` records for `D3DDevice_SetTile`.
   Naming it in `config/game.toml` hands it to the SDK's no-op replacement.
3. **Black screen with correct frames.** A frame capture showed the guest render target 93% non-black
   while the window measured 0.0% lit, and logging the ramp showed the title setting an all-zero
   gamma ramp when the first movie starts and never restoring it. On the console the video plays
   through the YUV overlay and the framebuffer is deliberately blanked; this renderer has no overlay
   and draws the video into the framebuffer, so the blackout hid it. The SDK fork now treats an
   all-black ramp as identity and applies pending ramps from either device slot.

Evidence after the fixes: 60.0 fps steady, 1 draw per frame plus a ~0.55 MB texture upload while a
movie plays, then 10-15 draws per frame with no video uploads once the movies end, window client area
85-94% lit (`out/shot.ps1` measures the client rect and verifies the window is foreground).

The SDK is a submodule pinned to `abilliontokens/XboxXBERecomp`, branch `urbz-port-fixes`, which
carries fixes 3's two runtime changes. Clone with `--recurse-submodules`.

## Build transcript (abridged)

```
cmake --preset release                         # 68.0 s configure, fetches Zydis, toml++, ImGui, SDL3
cmake --build out\build\release --target xbe_symbols
out\build\release\xbe_symbols.exe assets\default.xbe sdk\xdk --out config\game.toml
  default.xbe: title 45410060
    libraries: XAPILIB 1.0.5849 XBOXKRNL 1.0.5849 LIBCMT 1.0.5849 D3D8 1.0.5849 XGRAPHC 1.0.5849 DSOUND 1.0.5849 XMV 1.0.5849
    using sdk\xdk\5849
    found 130 of 238 functions, 11 of 12 data
cmake --build out\build\release               # 51.5 s, 381 targets, "Linking CXX executable Urbz.exe"
```

`generated/game/x86_recomp_stats.txt`:

```
functions 15942
instructions 910100
```

## Launch evidence

`logs/urbz.log` beside the executable, first run:

```
[    0.026] Game: default.xbe in D:\Urbz\assets, sha1 5712b6a3e2eaaacfcae228cf6154ade499a0033c
[    0.314] D3D12 adapter: NVIDIA GeForce RTX 5060 Ti
```

Process sample 15 s after launch: 44 threads, 15.8 s CPU, 182 MB working set, main window title
`The Urbz: Sims in the City`. At 25 s: 26.0 s CPU, same working set. No `CrashDumps/` directory was
produced, and the runtime logged no trap - the recompiled code is executing and the host survived,
but nothing has been seen on screen yet and no frame counters were logged.

## Known gaps, in the order they will matter

1. **Unmatched XDK functions.** 108 of 238 names are `0` in `config/game.toml` and become trapping
   stubs if called. Many are plainly absent from this game (Bink, XNet/Winsock, XOnline), but these are
   likely real misses that the game does use and that must be found by hand in Ghidra
   (`python sdk/tools/symbols/xbe2pe.py config/game.toml game.exe`):
   `D3DDevice_SetIndices`, `D3DDevice_SetLight`, `D3DDevice_LightEnable`, `D3DDevice_SetMaterial`,
   `D3DDevice_SetPalette`, `D3DDevice_SetTile`, `D3DDevice_SwitchTexture`, `D3DDevice_BeginPush`,
   `D3DDevice_EndPush`, `D3DDevice_FlushVertexCache`, `D3DDevice_GetVertexShaderFunction`,
   `D3DDevice_SetVertexShaderConstant1Fast`, `D3DResource_BlockUntilNotBusy`,
   `IDirectSoundBuffer_Lock` / `_Unlock` / `_GetCurrentPosition` / `_SetCurrentPosition`,
   `DirectSoundCreateBuffer`, the `IDirectSound_Set*` 3D setters, the `CDirectSoundStream_Set*` setters,
   and the CRT math entries `crt_sin`, `crt_cos`, `crt_tan`, `crt_atan`, `crt_sqrt`, `crt_log`,
   `crt_log10` (the SDK computes these on the host to avoid the Xbox x87 result mismatch).
2. **Dolby / XPP / XGRPH sections.** The XBE links `DOLBY`, `XPP` and `XGRPH` library code; `DOLBY` is
   excluded from `code_sections` by the symbol scanner, the other two are recompiled.
3. **XMV video.** 19 `.xmv` files ship on the disc and the XBE has an `XMV` section; the SDK's video
   support is written for Bink.
4. **Instruction decode noise.** The recompiler reported unimplemented opcodes that are typical of data
   decoded as code (`aaa` 7, `aad` 6, `aam` 14, `aas` 6, `arpl` 12, `bound` 17, `daa` 6, `das` 5,
   `enter` 5, `insb` 21, `int1` 7, `into` 1, `lds` 10, `les` 84, `lss` 1, `outsb` 2, `outsd` 1). If any
   of these sit inside a real function they need `[xbe] data_ranges` or `[[function]]` entries; if they
   are only in data they are harmless.

## Next actions

1. Confirm whether the window ever presents a frame: run with `XBE_D3D12_DEBUG` and `XBE_CAPTURE_DIR`
   set, and add `[xbe] trace` entries for the boot path to see where the guest stops.
2. Resolve the D3D8 and DirectSound misses above in Ghidra and fill their addresses into
   `[symbols]` in `config/game.toml`.
3. Fill the `crt_*` math symbols so the game's float results match the Xbox.
4. Re-run `build.bat` after every config change; `config/game.toml` already carries the title/entry
   guard, so a wrong image is rejected rather than silently recompiled.
