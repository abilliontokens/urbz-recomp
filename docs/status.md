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
| Gameplay | reaches free-roam city gameplay: Create-an-Urb, name entry, city load, rooftop district, dialogue, Power Social (see "Into the city") |

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
carries the runtime fixes described here. Clone with `--recurse-submodules`.


## Input, sound and the menu screens

Getting from the title to a playable menu took four more fixes, each found the same way.

1. **No controller input.** The game reads the pad through `XInputGetState`; a scripted pad hook in
   `src/common/game_info.cpp` (`URBZ_AUTOSTART`, later `URBZ_STEP`) injects presses on port 0. It only
   ever ORs bits in. The `URBZ_STEP` form reads a token from a file (`START`, `A`, `UP`, `RIGHTx9`,
   `LSDOWN`, ...) and emits an exact burst of timed pulses on the game's own poll clock, so a driver
   can do press -> screenshot -> decide -> press deterministically.
2. **`IDirectSoundBuffer_PlayEx` was a trapping stub.** It is wired in `config/game.toml`
   (`0x27FC29`, xtlid `0xB005A`) to a host implementation in `sdk/runtime/xdk/dsound.cpp` that shares
   `Play`'s flag handling; the scheduled-start time is not reproduced.
3. **Black scene after the title, then sliced logo and missing menu text.** The engine writes its own
   push-buffer packets for vertex-shader constants (`0x41EA4`); the SDK's push walker skipped them, so
   the projection constants stayed zero, and the walker only drained the ring at a few points, so
   draws snapshotted the previous drain's matrices. The SDK now parses those packets into its
   constant store and drains the ring at the top of every draw entry point, and
   `D3DDevice_SetVertexShaderConstant1Fast` (`0x24CF30`) is wired with a count-aware host loop.
4. **Monochrome textures.** P8 textures decoded without their palettes. `0x24BB90` is wired as
   `D3DDevice_SetPalette` (evidence and the naming caveat are recorded at the entry in
   `config/game.toml`): the guest function is stdcall `(stage, header)`, stores the header at
   `CDevice+0xF98+stage*4` and refcounts it - exactly the SDK host `SetPalette`'s shape - while the
   SDK's `SwitchTexture` host is fastcall `(method, data, format)` and rewrites the bound texture
   header. The `0x1B20 + 64*stage` push inside the guest function, which suggested `SwitchTexture`,
   is how the console uploads a palette through the texture unit's method space. Callers invoke it
   only for format-`0x0B` (P8) textures, passing `texture+0x58`, the embedded palette header; with it
   wired, the Maxis logo and menus render in full colour.

Evidence: `out/menu_fix.png` shows the load/new-game dialog in colour with readable text;
`out/step3_after_title_start.png` shows the main menu (1 Player / 2 Player / Credits).

## Into the city

Driving the menus deterministically with the step hook reached actual gameplay. Advancing out of
Create-an-Urb crashed once, and the crash named the next missing symbol: guest return addresses
`0x27FC7E` and `0x27F302` map to xtlid `0xB0057` at `0x27FC65`, a two-argument buffer call whose
original code forwards a mode to the buffer's inner object (`[this+0x20]`). Host buffer objects
have no such inner object, so the guest code faulted reading guest address `0x12`. The mode values
(0 resume, 1 pause, 2 pause-until-`SynchPlayback`) match `CDirectSoundStream_Pause`'s semantics
exactly, so `0x27FC65` is wired as `IDirectSoundBuffer_Pause` in `config/game.toml` (signature stub
`{xtlid = 0xB0057}` in `sdk/xdk/5849/signatures.toml`) with a host body next to `StopEx` in
`dsound.cpp`. After the fix the same press reaches the Hair screen, both name entries accept the
default names, and "Are you ready to enter the City with this Urb?" loads the city.

Evidence, run of 2026-10-14 with `XBE_PERF=1` (`out/run_perf.log`): `out/gameplay_city2.png`
(`isGame=True`) shows the created Urb standing in the city during the intro camera, and
`out/gameplay_rooftop.png` shows free-roam on the starting rooftop - player Urb, NPCs (Will, Darius),
district geometry, the Power Social prompt - after which the social played out and Darius's
reputation dialogue ran. Perf lines around those frames: 576-984 draws per frame (33k-57k verts,
3.9-6.7 MB upload) at ~61 fps, no exception lines in either run's log, and the boot-time dsound
audit no longer lists `0xB0057`. What is proven: movies, title, menus, save/new-game dialog,
Create-an-Urb (gender/body/skin, face/hair pages, both name keyboards), city load, free-roam
rendering, walking, dialogue and one Power Social. Not yet exercised: saving to a device and
reloading, districts beyond the starting rooftop, jobs/minigames beyond the intro social.

Still running their original guest code (boot-time audit, not yet hit in play): xtlid `0xB00A3`,
`0xB00A5`, `0xB00A4` (small stubs beside the `IDirectSound_Release`/`IDirectSoundBuffer_Release`
entries at `0x27ED56`/`0x27EDA0`/`0x27EDA5`), `0xB0080` at `0x27FCCC` and `0xB00A9` at `0x27FD0D`.
Like `Pause`, these operate on host sound objects if called, so they are the most likely next
crash; `IDirectSoundBuffer_Lock` also remains `0` in `config/game.toml`.

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
`The Urbz: Sims in the City`. (This section records the first boot; the sections above record the
runs that followed, which present frames and reach gameplay.)

## Known gaps, in the order they will matter

1. **Unmatched XDK functions.** 108 of 238 names are `0` in `config/game.toml` and become trapping
   stubs if called. Many are plainly absent from this game (Bink, XNet/Winsock, XOnline), but these are
   likely real misses that the game does use and that must be found by hand in Ghidra
   (`python sdk/tools/symbols/xbe2pe.py config/game.toml game.exe`):
   `D3DDevice_SetIndices`, `D3DDevice_SetLight`, `D3DDevice_LightEnable`, `D3DDevice_SetMaterial`,
   `D3DDevice_BeginPush`, `D3DDevice_EndPush`, `D3DDevice_FlushVertexCache`,
   `D3DDevice_GetVertexShaderFunction`, `D3DResource_BlockUntilNotBusy`,
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

1. Name the remaining original-code dsound exports (`0xB00A3`, `0xB00A5`, `0xB00A4`, `0xB0080`,
   `0xB00A9`) and `IDirectSoundBuffer_Lock`, and wire host bodies for the ones play reaches, in the
   manner of `IDirectSoundBuffer_Pause`.
2. Play further: save to a device and reload, leave the starting rooftop for another district, and
   run a job/minigame, fixing what surfaces.
3. Resolve the remaining D3D8 and DirectSound misses above in Ghidra and fill their addresses into
   `[symbols]` in `config/game.toml`.
4. Fill the `crt_*` math symbols so the game's float results match the Xbox.
5. Re-run `build.bat` after every config change; `config/game.toml` already carries the title/entry
   guard, so a wrong image is rejected rather than silently recompiled.

## Settings overlay, ultrawide and the first release

Recorded 2026-10-15.

- `src/common/overlay.cpp` is wired into `app::GameInfo::ui`: **F1** a settings panel (display mode,
  resolution, aspect ratio, ultrawide, vsync, frame cap, render scale, anisotropic filtering), applied
  live and written to `settings.toml`, and **F2** a frame-time graph fed by the SDK's new timers
  (`g_perf_sendstate_ns`, `g_perf_hle_ns`, plus the per-stage draw breakdown). Both start closed, and
  the overlay only owns the mouse while the settings panel is up.
- The SDK fork gained `aspect_ratio` (`auto`, `4:3`, `16:9`, `16:10`, `21:9`, `32:9`) and `ultrawide`.
  The widening moves the viewport offset, never its scale, so a display wider than 16:9 shows more
  world at the sides at an unchanged vertical field of view (Hor+, no stretch); only the main frame
  target grows, so off-screen passes keep their own size and centre.
- `tools/upscale_movies.py` upscales the disc's `.xmv` movies 6x into `assets/movies_4k` with
  realesrgan-ncnn-vulkan. It is offline tooling only: no runtime path reads those files yet.

Release check, the shape a player gets: a folder holding nothing but `Urbz.exe` and
`assets\Urbz, The - Sims in the City (USA).iso` (no extracted files). The log records
`Game: default.xbe in ...(USA).iso, sha1 5712b6a3...`, `files: D: is ...(USA).iso` and
`D3D12 adapter: NVIDIA GeForce RTX 5060 Ti`; 45 s in, the borderless client area measured
3840x2160 with 92.0% of sampled pixels lit, showing the district select screen in colour with clean
text. `release.bat` packs that exe with an empty `assets` folder, `README.md` and the SDK's
`THIRD_PARTY.txt` into `Urbz.zip` (5.6 MiB).

## Keyboard and mouse

Recorded 2026-10-16.

The guest has one input path and it is an Xbox pad (`XInputGetState`, section "Input path" of
`docs/input-and-draw-path.md`), so `src/common/input_kbm.cpp` translates the keyboard and the mouse
into a pad report on port 0 rather than inventing a second path. It hangs off the two hooks the SDK
already provides: `app::GameInfo::message` (the window thread, after ImGui, never taking a message so
F1/F2/F3 still work) collects key, mouse-button, mouse-move and wheel state into atomics, and
`app::GameInfo::pad` (`PadHook` in `game_info.cpp`, after the `URBZ_AUTOSTART`/`URBZ_STEP` script
pad) ORs that state into the polled `input::Pad`.

Rules it keeps:

- **Additive.** Buttons are ORed, an axis only moves if the keys push it further than a real stick
  already did, so a controller and the keyboard work at once and neither cancels the other.
- **Port 0 is always connected.** `xdk/xapi.cpp UpdatePorts()` derives `XPP_GamepadType.Current`
  from `input::Poll(p).connected`; forcing port 0 means the game opens a pad at boot on a PC with no
  controller plugged in.
- **Focus-gated.** `WM_KILLFOCUS`/`WM_ACTIVATE(WA_INACTIVE)` drop every held key (alt-tab produces no
  `WM_KEYUP`), and nothing reaches the game while the F1 settings panel is up
  (`overlay::SettingsOpen()`, the render thread's panel flag published as an atomic).
- **Round sticks.** A digital diagonal is normalised to the circle a real stick reaches instead of
  1.41x a cardinal; the wheel is a notch, not a position, so each notch holds the zoom axis for 90 ms
  (capped at 450 ms) on a steady clock rather than for a number of polls, which the 250 ms port
  poller would otherwise consume.

The layout is in `README.md`: WASD the left stick (walk), arrows the D-pad (menus), right-button
drag plus Q/E/R/F the right stick (the camera), wheel the zoom pulse, space A, escape/backspace B,
enter Start, tab Back.

The bindings were chosen from the image, not from habit:

- **The camera is the right stick, read as an analog value, and nothing else.** Camera update
  `sub_0001F4A0` calls the rotate handler `sub_0001F970`, which scales the history float at +0x30
  (sThumbRX) by `[pad+0x19C]` and adds it to the yaw at `cam+0x454`, negated when
  `OPT_invert_cam_rot` is set, and the zoom handler `sub_0001FC20`, which takes the float at +0x34
  (sThumbRY) and applies `cam+0x450 -= 1.3 * RY` clamped, then drives pitch `cam+0x458` and the
  field of view off that same distance (the "MAX FOV"/"MIN FOV"/"Camera Pos" debug printer
  `sub_00022540` prints the lerp). So stick up zooms in, there is no separate tilt control, and the
  triggers, black/white and the stick clicks move no camera at all - which is why the wheel drives
  the Y axis rather than a trigger byte. Deflection under ~24% of full is inside the game's own
  deadzone, and the digital right-stick bits only feed the "a UI panel has claimed the camera"
  tests (action 15 = all four right-stick bits).
- **Menu navigation reads the D-pad and the left stick as one action.** Input is table-driven: 58
  rows of `{action, button mask}` at guest `0x2E0FB8` (count at `0x2E1188`), installed by
  `sub_000118B0` through `sub_001920E0`, queried per action by `sub_0001CAA0` (pressed),
  `sub_0001CA30` (held) and `sub_0012E840` (repeat). Rows 21-28 give the four navigation actions
  both families - action 1 = `0x1000 | 0x10000`, 2 = `0x4000 | 0x40000`, 3 = `0x8000 | 0x80000`,
  4 = `0x2000 | 0x20000` - which is why WASD navigates most screens and the arrows, on the D-pad,
  navigate the rest.
- This corrects `docs/input-and-draw-path.md`: `sub_001D0340`/`sub_001D03F0`/`sub_001D0490` are not
  masked history queries. They reset the history, roll it forward and copy history 1 into history 2
  (sticks included); their callers pass a 0/1 flag, never a mask.

Not proven: this has only been built and read, not played. The translation itself was exercised
off-line - a throwaway harness drove `kbm::Message` with synthetic `WM_KEYDOWN`/`WM_MOUSEWHEEL`/
`WM_RBUTTONDOWN` messages and checked `kbm::Apply`'s report (18 checks: focus gating, panel gating,
the circle clamp on diagonals, the 90 ms wheel pulse and its expiry, drag deflection, OR-only
merging with a controller's own report, alt-tab releasing held keys, port 1 untouched).

## Frame rate and game speed

Recorded 2026-10-16. Reported defect: above 60 fps the whole game ran fast.

Measured first: `app::Pace()` does hold the cap (logged `pace: 61 swaps/s, cap 60`, then
`pace: 144 swaps/s, cap 144` when the cap was switched mid-run), so the host pacer was not at
fault - the engine's own step was.

**The engine measures real time and then throws the precision away.** The clock object at
`[0x358914]` reads `rdtsc` through XAPI `QueryPerformanceCounter` over a fixed 733,333,333 Hz, so
its elapsed seconds are real. The frame tick `sub_0018E480` then calls `sub_0018E360`, which does

```
ticks = clamp((int)(elapsed / [0x2F024C] + 0.5), 1, 15)      // 0x2F024C = 1/60 s
dt    = ticks * [0x2F024C]                                   // 0x18E580, into [0x2E2A80]
```

`[0x2E2A80]` is the single delta the rest of the engine multiplies by - 97 reads in 63 functions -
and with a 1/60 s tick and a floor of one tick, **every frame shorter than 25 ms still advanced the
game by a whole 1/60 s**. At 144 fps that is 144 x 1/60 = 2.4 s of game time per real second,
exactly the 2.4x that was seen.

The fix (`src/game/hooks.cpp`) keeps the engine's measurement and replaces only the quantiser: a
host `sub_0018E360` takes ticks of 1 ms and writes back a seconds-per-tick of `dt / ticks`, so
`ticks * [0x2F024C]` is exactly the time that really passed, at any frame rate. The engine's own
0.25 s ceiling (its 15 ticks of 1/60 s) is kept, so a stall still cannot advance the world in one
jump. Changing the unit is safe because nothing else depends on it: the tick count `[0x2E2A7C]`
has one other reader, `sub_001A7F80` at 0x1A7FAE, which forwards it only on the smoothing path the
game never enables (clock+5 is cleared at 0x18E0E2 and 0x1B02EE) and whose field the render
thread's `sub_001D44D0` discards; the running total `[0x358898]` is copied to `[0x3588BC]`, which
nothing reads.

Two subsystems stepped per *call* rather than per delta and are wrapped the same way:

- **Camera zoom** `sub_0001FC20`: `distance(cam+0x450) -= rate * RY` with rate `[[cam+0x08]+0x54]`
  = 1.3 and no frame-time factor, plus a 0.1-per-call out-of-bounds dolly at `[0x2E5DE8]`. The
  host scales both by `dt * 60` for the duration of the call and restores them.
- **Button auto-repeat** `sub_0018D6D0`: counts each held button's timer down by one per call,
  reloading from the pad's delay byte +0x1D8 (10) and rate byte +0x1D9 (3), and the manager calls
  it twice a frame - so menus scrolled 2.4x fast at 144 fps. The host rescales those two counts to
  the call rate it measures itself (smoothed), which keeps the original logic running on every
  call at console timing.

Everything else was checked and is already delta-driven: animation channels and blend weights
(`sub_001A31A0`, `sub_001A6770`), camera rotate/follow/recenter (`sub_0001F970`, `sub_0001EA30`,
`sub_00021B10`), needs decay and the game clock. Movie playback is not frame-driven at all - the
XMV decoder is paced by the D3D vertical-blank count (host thread, 16667 us) and corrected by the
audio clock - and the Flash menus and title idle timer are wall-clock.

Verified: `fps_cap` 60, 144 and unlimited, with a throwaway probe summing the engine's own dt
against the wall clock - **1.000 game seconds per real second in all three**, at 60, 138 and 2474
frames per second respectively, no exceptions. The probe was removed before shipping.
