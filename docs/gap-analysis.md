# Coverage of this game's console surface

What the SDK runtime already answers for *The Urbz* specifically, and what it does not. Ordinal and
symbol numbers come from `tools/kernel_coverage.py`, `config/game.toml` and `sdk/xdk/5849/`.

## Kernel (xboxkrnl)

Imports are bound by ordinal: the runtime registers exports into a 400-slot table
(`KERNEL_EXPORT` / `KERNEL_FASTCALL` / `KERNEL_DATA`, `sdk/runtime/kernel/exports.h:5-27`,
`sdk/runtime/cpu/dispatch.cpp:25-30`), `kernel::Boot` rewrites each XBE thunk slot
(`sdk/runtime/kernel/boot.cpp:117-132`), and an unbound ordinal logs
`kernel export %u (%s) is not implemented` and traps (`sdk/runtime/cpu/dispatch.cpp:113-115`).
Nothing is silently stubbed.

```
python tools/kernel_coverage.py assets/default.xbe --markdown docs/kernel-coverage.md
default.xbe: 124 kernel imports
  implemented by the SDK runtime: 124
  not implemented (traps if called): 0
  SDK registers 269 exports in total
```

**Every kernel import is now covered: all 124 are implemented by the SDK runtime, with no kernel gap.** Per-ordinal detail is in `docs/kernel-coverage.md`.

## XDK libraries

The XBE links XAPILIB, XBOXKRNL, LIBCMT, D3D8, XGRAPHC, DSOUND and XMV, all 1.0.5849. `xbe_symbols`
located 130 of the 238 signed functions and 11 of 12 data symbols. Section-restricted matching
(`sdk/tools/symbols/main.cpp:480`) means signatures for sections this XBE does not have - BINK, XNET,
XONLINE - can never match and are expected zeros.

| Section | Signatures shipped for 5849 | State for this game |
| --- | --- | --- |
| `D3D` | 100 functions + 8 data | mostly matched; see the misses below |
| `DSOUND` | 63 + 12 xtlid-only + 2 anchors + 2 data | 31 of 75 names matched |
| `XPP` (XAPI input) | 5 + 1 anchor + 2 data | fully matched and replaced (`sdk/runtime/xdk/xapi.cpp:46,69,78,85,110`) |
| `.text` (CRT math, launch helpers) | 15 | `crt_sin/cos/tan/atan/sqrt/log/log10` unmatched |
| `XGRPH`, `XMV`, `DOLBY` | 0 | no host replacement exists; the game's own code is recompiled instead |
| `BINK`, `XNET`, `XONLINE` | 4 / 34 / 2 | not linked in this XBE, zeros are correct |

A name left at `0` in `[symbols]` gets a placeholder whose body traps, so a host override still links
(`sdk/tools/recomp/codegen_emit.cpp:205-214`). It only fires if that function is actually called.

### Misses worth resolving by hand

Find them in Ghidra (`python sdk/tools/symbols/xbe2pe.py config/game.toml game.exe`) and fill the
addresses into `[symbols]`:

- D3D8: `D3DDevice_SetIndices`, `SetLight`, `LightEnable`, `SetMaterial`, `SetPalette`, `SetTile`,
  `SwitchTexture`, `BeginPush`, `EndPush`, `FlushVertexCache`, `GetVertexShaderFunction`,
  `SetVertexShaderConstant1Fast`, `GetRenderTarget2`, `PersistDisplay`, `D3DResource_BlockUntilNotBusy`.
- DirectSound: `DirectSoundCreateBuffer`, `IDirectSoundBuffer_Lock` / `_Unlock` /
  `_GetCurrentPosition` / `_SetCurrentPosition` / `_StopEx`, the `IDirectSound_Set*` 3D setters, the
  `CDirectSoundStream_Set*` setters, `XAudioDownloadEffectsImage`.
- CRT math: `crt_sin`, `crt_cos`, `crt_tan`, `crt_atan`, `crt_sqrt`, `crt_log`, `crt_log10`. The SDK
  computes these on the host precisely because the console's x87 results differ; leaving them
  unresolved means the game's own x87 code runs instead.

## Audio

`sdk/runtime/xdk/dsound.cpp` mixes PCM and Xbox ADPCM buffers and streams in software through XAudio2,
with mix bins and synchronized playback. Not reproduced: DSP effects (`dsound.cpp:529`), filters,
release envelopes (`:636`, `:866`), HRTF, I3DL2 reverb, and anything beyond stereo output
(`:486` reports `DSSPEAKER_STEREO`). `AuditExports` logs
`dsound: exports running their original code:` for DSOUND exports that are linked but unmatched
(`:423-469`) - watch for that line at boot.

The disc ships XACT-style banks (`DATA/audiostr.xsb`, `DATA/audiostr.xwb`) but the XBE links no XACT
engine, and the SDK has no XACT code at all. The game's own bank parser is recompiled and will feed
ordinary DirectSound buffers/streams, which are covered - provided the bank data is PCM or Xbox ADPCM
(`dsound.cpp:364` logs `dsound: format tag %X not supported` otherwise).

`sdk/runtime/kernel/xboxkrnl.cpp:1234` returns `0x00010001` for `XC_AUDIO_FLAGS`, commented "stereo";
on real hardware that bit pattern reads as mono plus AC-3 enabled. If the game branches on the system
audio flags, that is a lead worth checking before blaming the mixer.

## Video (XMV)

This is the largest gap. The disc ships 19 `movies/*.xmv` files and the XBE has an `XMV` section:

- The SDK has no XMV player, decoder, signatures or layout keys; its only movie support is three Bink
  wrappers (`sdk/runtime/xdk/bink.cpp:1-55`), and all four Bink names are `0` here.
- The XMV section is recompiled as guest code (`code_sections` in `config/game.toml:10`), so the
  decoder itself runs - but presentation has nowhere to land:
  `D3DDevice_EnableOverlay` is a `return 0` stub (`sdk/runtime/xdk/d3d8.cpp:422`) and is not even bound
  for this game; there is no `UpdateOverlay`; YUY2/UYVY are in the texture format table but have no
  decoder case (`sdk/runtime/gpu/texture.cpp:51-52,126-235`), so they fall through to a grey
  luminance path; and the `YuvEnable` render state is stored but never read (`d3d8.cpp:612`,
  `sdk/runtime/gpu/state.h:64`).
- Expect the intro, attract and ending movies to be the first thing that needs real work: either a
  host XMV decode path plus a YUV blit, or hooks that skip the videos while the rest is brought up.

## Priority order for bring-up

1. Prove the first presented frame. A black window is not evidence: every present clears the back
   buffer to black (`sdk/runtime/gpu/d3d12.cpp:2754-2755`) and the window class brush is black
   (`sdk/runtime/app/main.cpp:243`), and nothing is logged at present time at any level. Use a build
   without `NDEBUG` (`sdk/runtime/core/log.h:18-22` gates every LOGI/LOGW on it) plus `XBE_PERF`,
   whose `perf:` lines only start once presents happen.
2. Resolve the D3D8 misses above - draw-state functions are on every frame's path.
3. Resolve the `crt_*` math symbols, which otherwise silently change float results.
4. Audio: watch for `dsound: exports running their original code:` and
   `dsound: format tag %X not supported`, then fill the DirectSound misses.
5. XMV video last, as a dedicated piece of work.
