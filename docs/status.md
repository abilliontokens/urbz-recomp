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

## City frame rate: attribution, then the fixes

Recorded 2026-10-16. Reported defect: the frame rate drops in the city districts, worst with the
camera zoomed out.

### What the profile said, before any optimisation

Measured in a district from a save, windowed 640x480, camera director idle, at three camera
distances (`cam+0x450` read live: 5.0, 15.7-19.4, 30.0). Each row is a frame-weighted mean over
one-second perf blocks that passed a filter on camera angle, distance band, director state and Sim
movement; block and frame counts are in the raw logs under `out/perf/`.

| zoomed | fps (cap 60) | ms/frame | render thread `host` | title `in XDK draws` | of it `sendstate` | draws/frame |
| --- | --- | --- | --- | --- | --- | --- |
| in (d=5) | 56.7 | 17.63 | 5.09 | 7.08 | 4.84 | 801 |
| mid | 47.8 | 20.93 | 5.43 | 8.13 | 5.21 | 855 |
| out (d=30) | 43.4 | 23.03 | 5.96 | 8.81 | 5.74 | 929 |

Three findings decided everything that followed:

1. **The title thread is the critical path, not the GPU.** `waiting` was 0.00 ms with 0 waits in
   every window at every distance: the title thread never blocked on the render thread, which was
   busy 4.3-6.3 ms of a 15.6-24.0 ms frame and idle for the rest. Optimising the D3D12 backend
   could therefore not shorten a frame - only make headroom.
2. **The single largest renderer cost was on that thread**: `SendState`, the recorder's comparison
   of all 14,424 bytes of draw state (226 pieces) on *every* draw and clear - 4.8-5.7 ms/frame,
   about a quarter of the frame.
3. **Zooming out costs more because more objects pass the frustum, not because they get bigger.**
   The engine has no LOD selector and a fixed far plane of 2000 (`sub_0019CA90` stores near/far at
   view+0x2F0/+0x2F8; the box test is `sub_0019E4F0`, called from the level pass `sub_001AC8D0`,
   the static-model visibility method `sub_001DE0E0`, walls `sub_00035140` and Sims
   `sub_0003CC90`). Between d=5 and d=30 the FOV widens 55 to 65 degrees and the pitch goes 13 to
   58.5 degrees, which in a compact area covers 3.2-3.7x the ground. Verts per draw are the same at
   both zooms (47); the draw count is what rises. The one explicit switch is a room-object cull at
   0x1AD013 that only applies while the eye is below z = 5.0, i.e. d < ~10.

### What was changed

- **The host runtime was being compiled unoptimised.** `CMAKE_CXX_FLAGS_RELEASE` was empty for
  clang targeting the MSVC ABI, so only the generated guest code carried `-O2` (set per source).
  `sdk/cmake/XbeRecomp.cmake` now adds `-O2` for Release and RelWithDebInfo. `NDEBUG` stays
  undefined deliberately: the logger's verbose mode keys off it.
- **`SendState` is proportional to what changed** (`runtime/gpu/render_thread.cpp`,
  `runtime/gpu/state.h`): every writer of `gpu::State` marks the 64-byte blocks it touched (write
  first, then mark), the mask is 12 atomic words taken with `exchange(0)`, and only marked pieces
  are compared and sent. `XBE_NO_STATE_DIRTY=1` restores the full comparison;
  `XBE_STATE_VERIFY=1` runs both and logs any piece that changed without a mark, naming the field.
  *(Note, 2026-10-18: the `XBE_NO_DRAW_REUSE`, `XBE_SHADER_THREADS` and `XBE_NO_SHADER_CACHE`
  switches described here were removed with the renderer rework in sdk commit `15dcf87` and no
  longer exist in the build; the measurements below are historical. The title-thread state marks,
  with `XBE_NO_STATE_DIRTY` and `XBE_STATE_VERIFY`, came back the same day on their own: without
  the deferred-state gate (the deferred states are compared on every draw, so none is ever a frame
  stale), with every write to the title's `gpu::State` forced through a marking helper, and with the
  verifier naming byte offsets rather than fields.)*
- **Per-draw redundancy in the D3D12 backend** (`runtime/gpu/d3d12.cpp`): descriptor tables,
  samplers, texture stages, shader/pipeline keys and constant uploads are reused when a byte-exact
  compare says nothing changed; the second `BindTargets` and the depth-copy probe are gated; the
  ~18 per-draw timer reads only happen under the perf switch. Each has its own `XBE_NO_*` opt-out.
- **Shader and pipeline creation left the critical path**: compiles run on a worker pool
  (`XBE_SHADER_THREADS=N`, default cores-2) and results are cached on disk in `shadercache/`
  beside the exe, keyed by content hash and pinned to the adapter, driver and compiler
  (`XBE_NO_SHADER_CACHE=1` disables). A draw whose pipeline is not ready is recorded into its own
  command list and chained in order - no draw is skipped or substituted.
- **Texture residency follows the real VRAM budget** (`IDXGIAdapter3::QueryVideoMemoryInfo`)
  instead of a fixed sweep schedule.
- **A second frame-rate-dependent camera bug** was found while mapping the engine and fixed in
  `src/game/hooks.cpp`: the idle auto-return `sub_0001F0E0` grew its easing speed by a constant
  per call (`[0x2E4D9C]` = 0.05 at 0x1F295) before dt-scaling the step, so the camera snapped back
  harder the higher the frame rate. The acceleration is now scaled by the frame's real time.

### Result

Same save, same scene, same three distances, same block rules. Capped at 60, every distance now
holds the cap - including zoomed out, which was 43.4 fps:

| zoomed | before (cap 60) | after (cap 60) | before uncapped | after uncapped |
| --- | --- | --- | --- | --- |
| in (d=5) | 56.7 fps | 60.0 (cap) | 64.0 fps | 278.3 fps |
| mid | 47.8 fps | 60.0 (cap) | 47.4 fps | 206.1 fps |
| out (d=30) | 43.4 fps | 60.0 (cap) | 41.6 fps | 183.2 fps |

Draw-normalised at 850 draws/frame, uncapped, the frame goes 20.43 ms to 4.59 ms (-78%). The 2x2
against the compiler flag, measured rather than assumed:

| at 850 draws | old code `-O0` | old code `-O2` | new code `-O0` | new code `-O2` |
| --- | --- | --- | --- | --- |
| ms/frame | 20.43 | 5.68 | 16.12 | **4.59** |
| `sendstate` | 5.19 | 1.07 | 1.58 | 0.39 |
| `in XDK draws` | 7.91 | 1.64 | 3.70 | 0.95 |
| `host` (render thread) | 5.40 | 2.32 | 4.52 | 2.10 |
| upload MB/frame | 5.46 | 5.45 | 3.49 | 3.51 |

The two overlap, so the honest statement is multiplicative: the flag is x0.28, the renderer work
x0.81 on top of it, combined x0.22. Ordered flag-first the renderer work is 7% of the saving;
ordered renderer-first it is 27%. The renderer's share is still almost entirely title-thread work -
at `-O2`, recording 1.21 to 0.53 ms and `sendstate` 1.07 to 0.39 ms, while `host` moves 2.32 to
2.10 ms and `waiting` stays 0.

**First entry into a district**, which is what "drops badly" felt like, was a synchronous compile
stall: 452.8 ms in a single frame and 1.9 fps in the reference capture
(`out/sub_stdout.log:1416-1419`), 647 ms of compiling in the worst second from a save. Now, cold
cache: 239 ms of compiling in the worst second, worst block 40.9 fps, longest single frame 206.6
ms. Warm cache (77 shaders, 103 pipeline blobs loaded): **no compiling at all**, worst block 53.8
fps, longest frame 85.9 ms.

### Verification

- `XBE_STATE_VERIFY=1` over a whole session - boot, district load, two rounds of the three camera
  distances, two zoom sweeps, a stick walk and a keyboard walk that moved the Sim between rooms,
  camera rotation, pause open and close: **zero** "changed with no mark" lines.
- The verifier did log three `deferred render state 127 changed ... with no XDK dirty flag` lines
  during boot. State 127 is `kRsPresentationInterval`, which nothing in `runtime/gpu` or
  `runtime/xdk` reads, so it changes nothing today - but it proves the XDK dirty-flag word is not
  authoritative, so the deferred-state gate also does a full compare once per presented frame,
  which bounds any unflagged change to one frame of staleness.
- All five measured runs: exit 0, zero `[error]` lines, zero `pipeline creation failed`, no crash
  dumps. The `[warn]` set is identical to the baseline's (dsound exports running original code,
  three ignored `wbinvd`).
- Screenshots at the same view as the baseline: static scenery is pixel-identical (mean per-channel
  difference 0.26 and 0.00 on the two walls; whole-frame 2.28 is the Sim's pose and the HUD clock).
  No missing geometry, no stale textures. Texture uploads per frame are 0 in steady state in every
  configuration including the baseline, and the first-entry totals match (81-99 versus 86-104), so
  nothing is being skipped that the scene needs.
- Game speed is unchanged by the frame rate: the in-game clock advances 0.793, 0.794 and 0.793
  game-minutes per wall second at capped 60, uncapped ~180-280 fps, and the uncapped control.

### What still costs, and the ceiling

Zooming out still costs +1.87 ms/frame uncapped at `-O2`, and +1.49 ms of that is guest work
outside the XDK draw calls - the engine's own per-object culling, sorting into 46 distance buckets
(`sub_001AD5D0`, `sub_001AD880`) and submission, which is recompiled title code. The renderer side
of a zoom-out is now 0.38 ms. Nothing further on the host can remove that 1.49 ms; it would take
changing what the title submits.

One unexplained observation, recorded rather than hidden: two discarded uncapped attempts rendered
a black world (HUD live, clock advancing, district files opened, ~250 draws) while an external
2 kHz memory sampler was attached during the load. Six runs without the sampler, and both capped
runs with it, loaded correctly, and it was never reproduced in a measured run.

### Reading the perf leaves after this change

The draw-split leaves moved scope, so they are not comparable with older logs (the parent `draw`
and `host` totals are): `geom` now covers geometry upload plus all draw-state recording (pipeline,
root CBVs, descriptor tables, viewport, scissor, blend factor, stencil ref) plus the draw call;
`views` is descriptor and sampler preparation only; `consts` is constant computation and ring
upload only; `pso` is shader and pipeline resolution as before. `compile` additionally accumulates
render-thread stalls waiting for a pending pipeline when the worker pool is on.

## Camera and cursor rates after the frame-rate work

Recorded 2026-10-16. Owner report once the city ran fast: the zoom sometimes did not hold and
drifted out, zoom felt fast above 60 fps, and the blue selection cursor felt much too fast above
60 fps. Measured in a district at 60 fps capped against 254-292 fps uncapped, sampling guest state
at 100 Hz (camera distance `cam+0x450`, idle countdown `cam+0x3C4`, return speed `cam+0x3CC`,
cursor `SimInteractor+0x0C`, Sim position through the camera target `cam+0x444`):

| what | at 60 fps | uncapped | ratio | verdict |
| --- | --- | --- | --- | --- |
| held zoom | 77.98 u/s | 77.24 u/s | 0.99 | already correct |
| idle auto-return (countdown, ease, peak speed) | 0.84 s / 2.15 s / 3.95 | 0.85 s / 2.12 s / 3.90 | ~1.00 | already correct |
| free-roam selection cursor | 4.611 u/s | 4.722 u/s | 1.02 | already correct |
| Sim walk | 4.639 u/s | 4.663 u/s | 1.01 | already correct |

So neither the zoom nor the free-roam cursor was frame-rate dependent. The camera jumping from
distance 5 to 30 while walking is the game's own outdoor reframe when the Sim leaves a building,
and the zoom "not holding" is the designed auto-return: 2.0 s (`[0x2E4D94]`) after the last camera
input, `sub_0001F0E0` eases back to the home distance `[[cam+8]+0x84]` = 5.0, at the same rate at
any frame rate.

What was genuinely wrong and is fixed:

- `sub_0001FC20` scaled the out-of-bounds dolly `[0x2E5DE8]` as well as the zoom rate, but the
  dolly multiplies a delta the rate had already scaled - it was scaling squared. Only the rate is
  scaled now.
- `sub_0001F320` (shot-mode camera glide, `[0x2E4DA4]` = 0.6 per call) had no frame-time factor
  and is wrapped like its siblings.

One attempt in this round was wrong and is recorded because the failure is instructive: the script
binding executor `sub_00173300` hands raw stick floats to bound handlers with no frame-time
factor, which looked like the cursor's mover. Scaling the sticks for every binding group made
**walking ten times too slow uncapped** (5.0 u/s at 60 fps against 0.51 at 255 fps), because the
movement handler already applies frame time. It was reverted the same session. Instrumentation
left behind: `XBE_BIND_LOG=1` reports each distinct handler the executor dispatches to (in this
title exactly one ever does - `0x0017F600`, the movement handler, vtable `0x002CF0C0`), and
`XBE_BIND_SCALE=<addresses>` scales only named handlers. Both default to off.

The free-roam cursor is the `SimInteractor` object's `+0x0C` world position, written by
`sub_001764B0` from `sub_0017E400`, which multiplies the displacement by `[0x2E2A80]` at
0x17653A-0x176553 - already correct. The decorating interactors (`sub_001766F0`, used by
FloorPainter `sub_00171830`, WallManipulator `sub_001818D0`, WallPainter `sub_001842B0`) do not
read the delta, but every caller gates them with a press-transition pulse, so they emit one fixed
step per press rather than a per-frame rate. If a fast cursor is ever seen again, those modes are
where to look, and the method is above: sample that interactor's `+0x0C` at both frame rates.

## Menu repeat speed, and a driver crash after selecting an Urbie

Recorded 2026-10-16.

**Menus and name entry repeated too fast at high frame rates.** Not the pad path: the
Create-an-Urb name keyboard (`tsc3_cas_keyboard`, an Apt movie) and every menu move on router
events, and the router's held-direction gate `sub_0001D530` is the rate limiter. It advances a
global `[0x378B60]` by the honest clock delta `[0x358494]`, but then adds `int([0x378B60] * 30)`
- the *cumulative* tick count, clamped to 4 - into each direction's accumulator
(`[0x3584D8 + (pad*4+dir)*4]`, threshold `[0x3584B8 + ...]` starting at 22, -3 per fire, floor
12). Adding a cumulative count once per call makes the rate quadratic in the call rate:
simulated from the guest algorithm, ~8.8 repeats/s at 60 fps, 15.8/s at 144, 27.8/s at 240.
`src/game/hooks.cpp` now gates `sub_0001D530` per (pad, direction) to console cadence: the
original runs once per 1/60 s with `[0x358494]` (whose only reader is this function) pinned to
exactly 1/60 for the call and restored after; skipped calls return 0 and pop the arguments
(ret 8). The pad auto-repeat `sub_0018D6D0`, whose repeat word only one widget reads
(`sub_0012F2D0` mode 4), was also corrected: its delay/rate counts are rescaled by
`1/(60 * dt)` - 2 calls per pad per frame over the console's 120 calls/s - where an earlier
version smoothed gaps between all 8 back-to-back calls a frame and ran 4-6x too *slow*.

**Crash after selecting an Urbie (2026-10-16 19:41).** `Urbz.exe - Application Error`, read of
`0x28`. The minidump (`CrashDumps/Urbz.2026.10.16_19.41.15.dmp`, beside the exe in
`out/build/release`) names the faulting module: `nvwgf2umx.dll` (the NVIDIA D3D12 user-mode
driver) at RVA 0x341259, reading address 0x28, exception code
0xC0000005 on thread 15900. There is no `exception` line in `logs/urbz_015.log` - the host's
crash handler logs guest-thread exceptions with guest registers, and this fault was on a host
thread, so the crash is in submitted graphics work reaching the driver, not in guest code and
not at a guest address. The log's last activity is a burst of fresh vertex-shader compilations
(`d3d: vertex shader B0108E81`...) at 148.6 s during the character-select transition. The
`sub_0001FC20` zoom wrapper gained a defensive guard in the same change (a null `[cam+8]` or an
implausible rate now runs the original untouched instead of writing into guest memory), but the
dump's fault address is in the driver, not at anything that wrapper writes. If it recurs, keep
the .dmp and the matching log; the pair is what a driver-side triage needs.

## The level-load crash, and what it cost

Recorded 2026-10-16. The owner's game died every time at the Create-an-Urb to city transition.
Five minidumps in `out/build/release/CrashDumps/`, all the same fault: `nvwgf2umx.dll+0x341259`
(NVIDIA's D3D12 user mode driver), access violation reading `0x28`, on a **driver owned thread**
whose stack carries no frame of ours, during the burst of brand new shaders the city load makes.
No `exception` line appears in any of the logs, because the host's handler logs guest thread
faults with guest registers - this was never guest code.

Each suspect was killed by a run, not by argument:

| Suspect | Killed by |
| --- | --- |
| Shader content | the same handles compiled fine in runs that reached the city (urbz_010, urbz_014) |
| Stale disk cache | crash 1 ran with `0 shaders, 0 pipeline blobs` |
| Concurrent `CreateGraphicsPipelineState` | serialised behind a mutex, crash 3 |
| Anisotropic filtering (the only setting that split the matrix) | aniso 0, crash 4 |
| Sampler heap wrap (`Submit(true)` mid draw) | instrumented with a warning; it never fired |
| vsync, the tearing/direct-flip present path | vsync on, crash 5 |
| Every per-call guest wrapper (`sub_0001FC20`, `sub_0018D6D0`, `sub_0001D530`, `sub_0001F0E0`, `sub_00173300`, `sub_0001F320`) | all disabled together through the new `URBZ_NO_*` switches, still crashed |

That left one delta between the owner's last good session and the crashing builds: the SDK
renderer rework (`04bea96`, "cut the city frame"). Reverting its six source files to `5bbdb3d`
while keeping the same `-O2` host build stopped the crash - the owner played through the
transition and into a district. The revert is `sdk` commit `15dcf87`.

The defect is still inside that diff. The standing suspect is the deferred-draw path
(`Defer`/`Pending`/`RunPending`/`RecordDeferred`): a parked draw holds GPU descriptor handles
(`dc.samplers`, `dc.views`) and upload-ring offsets, and a mid-frame `Submit(true)` resets the
ring (`f.used = 0`), the SRV counter (`f.srv = 0`) and the allocators before it is replayed.
That reads as garbage descriptors or vertex data to the GPU, which is the shape of a null
dereference on a driver thread. Frame rate correlated loosely (144 crashed, 60 survived), which
fits shorter frames leaving more pipelines unfinished and more draws parked.

What the revert costs until it is re-landed: the shader disk cache and compile worker pool (so
first sight of an effect compiles inline, and nothing is cached between runs), dirty-tracked
`SendState`, and the per-draw reuse fast paths. The same section's profile says the city is
title-thread bound and the critical-path win was the `-O0` to `-O2` flag, which stays in
`sdk/cmake/XbeRecomp.cmake`; the owner reports good frame rate in a district on this build.

Switches added while bisecting, kept because they make the next one cheaper: every frame-rate
wrapper in `src/game/hooks.cpp` can be turned off for a run - `URBZ_NO_STEP`, `URBZ_NO_ZOOM`,
`URBZ_NO_REPEAT`, `URBZ_NO_UIGATE`, `URBZ_NO_IDLECAM`, `URBZ_NO_BINDINGS`, `URBZ_NO_SHOTCAM`.

## The modern HUD

Recorded 2026-10-17. The game's own HUD is a 640x480 movie stretched over a 4K output: soft icons,
pixelated text, a bitmap star. `src/common/hud_draw.cpp` redraws what it shows as ImGui draw-list
vectors at the output's own resolution, `src/game/hud_state.cpp` reads the values out of the guest,
and the setting `modern_hud` (F1 panel, on by default) switches back to the original.

### Reading the values

| Shown | Where it comes from |
| --- | --- |
| Five motives and mood | `H + 0xA8 + 6*player + 0..5`, percent bytes 0..100 |
| Reputation number | `H + 0x970 + 2*player` - `HUD_rep`, what `sub_142180` returns |
| Bar beside it | `H + 0x96C` (base) to `H + 0x974` (next unlock) |
| Clock | `G = *[0x2EDF60]`, `int16 [G+0x14]` hour, `[G+0x1E]` minute |
| Action queue | controller from `sub_12FF00` (`this` in esi): count at `+336`, running slot at `+777` |
| Dialogue | `[H+0xE0]` state, `[H+0xD4]` record: body, speaker and continue label as string objects |

`H` is the HUD object, captured from the game's own variable getter (`sub_1416C0`, `this` in ecx)
rather than trusted from `*[0x35A740]`, which happens to agree but is not what the game passes.

**The motive bytes are a cache, not state.** `sub_142430` asks the sim for one motive and writes
the byte the HUD reads; nothing refreshes it on its own - the movie's bars pull it while they are
on screen. Hiding those bars therefore froze the modern HUD at zeroes while the game's own bars had
been full. The walk hook now makes those six calls itself each frame, which is exactly what the
game does when its bars are up.

### Hiding the old HUD, the game's own way

The display-list walk (`sub_229760`) renders a child only when bit `0x8000` of `[child+0x04]` is
set - the bit ActionScript's `_visible` writes. Clearing it on the status clips makes the game skip
them and their subtrees, so nothing is drawn and then covered. The clips are resolved by name
through the game's own child lookup (`sub_1FA850`), which takes **one path segment per call**:
`ingame` -> `hud0` -> the clip. Whole-path probes matched nothing, which is why an earlier attempt
found no clips at all. The names are the movie's own, read out of `DATA/flashes.arc`:
`mc_meters`, `mc_meters_job`, `mc_simoleons`, `mc_meterbar0..4`, `mc_meterClockPoll`, `mc_jobIcons`,
`mc_ringtone`, `mc_npcDialog`, `mc_npc2Dialog`, and the action queue's `aq%d`/`aqa%d` slot plates.
The pie menu (`mc_actionProto`), the lot name and the queue's `aqi%d` pictures stay: those are
interaction, and the pictures are rendered by the game into clips the host cannot redraw.

The bit stays cleared. Restoring it at the end of the walk put the HUD straight back on screen -
the draw happens after the walk returns - and re-clearing every frame covers the script turning its
clips back on while it animates the HUD in.

### Two reads that were wrong, and how they showed

- **Every pointer was rejected.** `GuestPtr` capped at `0x8000000`, but every Apt clip lives in the
  physical window at `0x80000000` (`0x83xxxxxx`) and host scratch in the system pool at
  `0xB0000000`. Probes, clip identity and the dataset walk all silently returned nothing. The check
  now mirrors `sdk/runtime/kernel/memory.h` and additionally requires every page it spans to be
  committed - without that, string bytes that look like an address (`"ise"` -> `0x00657369`) pass a
  range test and fault the host, which is exactly how the first child-list walk crashed.
- **The box is not a level.** It was read out of the `RepTitleTable` dataset, whose cell holds a
  pointer to a pointer to a localized string ("Level 1"); the number the game actually puts in the
  box is `HUD_rep`, the reputation itself. The dataset walk is gone.

### Verified in play

Driven from `tools/drive.py` (menu -> Load Game -> the owner's save -> city, by matching each screen
against a reference frame): motives track the sim and refill after using a toilet, the clock matches
the game's own, the reputation box reads the same number the original drew, the legacy status clips
and the bottom-right queue plates are gone, and NPC dialogue renders in the modern card with the
game's own colour spans. Placement is measured, not guessed: the title letterboxes its picture
(rows 0-65 and 2108-2159 are black at 3840x2160), and the original HUD's last row sat at 0.894 of
the output height, which is where the modern one sits.

## Console cheats

Recorded 2026-10-18. The F1 panel's **Cheats** section enters the game's own pad codes the game's own
way (`src/game/cheats.cpp`, `src/common/cheats.h`). Built, not yet verified in play.

- **Recogniser.** `sub_126A0`, called at the top of the per-frame update `sub_11E00`, keeps the last
  six single-button presses (each within a second of the last) and matches them against a 12-row table
  that `sub_4A300` writes at `0x35A86E` (rows 12 bytes apart, up to six masks each, in the game's pad
  layout from `sub_1923A0`). The table is built in code, which is why no byte search of the XBE finds
  the sequences. A match calls `sub_4A280(eax = 0x35A868, ecx = row)`: it flips bit `row` of the word
  at `0x35A916` and plays the cheat-on or cheat-off sound; row 11 only plays a sound. The front end's
  state entry `sub_A87F0` clears the word.
- **Effect.** The word's only reader is the SimAntics global getter `sub_116200` (global 46). The
  cheat gnome (`zz_move_a__tron` in `DATA/edithtre.arc`) appears while flag 1, 2 or 3 is set and
  offers one pie-menu entry per set flag (Edith flags count from 1: flag n+1 is row n).
- **Host entry.** The override of `sub_11E00` makes the `sub_4A280` call for each code the panel
  queued, on the title thread, ahead of the game's own recogniser - only while the flow
  (`[[SIMS system]+0x88]`, request `+0x84` and current `+0x88`) is 4, "ingame", and a lot's
  SimAntics globals exist. `kGetCheatCode` (primitive 53, `sub_F1CB0`, reading `0x35AB40`) has no
  writer in this build and nothing in the trees calls it.
- **Team photo.** The credits movie's `on_input` (`tsc3_credits.apt`) opens the development team's
  photo after DPAD_UP DPAD_DOWN SELECT DPAD_UP DPAD_DOWN SELECT; native code posts SELECT for action 5,
  the A button (`sub_1D630`). The panel feeds those six presses to port 0 through `PadHook`, 130 ms
  down and 130 ms up, only while the flow is 19 (Credits), and stops if the credits close.

| Row | Xbox sequence | Gnome menu entry |
| --- | --- | --- |
| 0 | Left Y Down A X | (Get Shorty: the gnome appears) |
| 1 | White B R L Black | Give Sim Money |
| 2 | R L Down Black Left B | Fill All Motives |
| 3 | L Black Right X Left | Skill Objects |
| 4 | R Left B Down White | Unlock All Districts |
| 5 | Black Up Y Down R | Unlock All Socials |
| 6 | Black Down R L Down Y | Jump Ahead 6 Hours |
| 7 | L R A Down Black | Max Physical Skill |
| 8 | L B A Black Down Y | Max Mental Skill |
| 9 | Y Down Black A B | Max Artistic Skill |
| 10 | Down Black Right X Left | Give All Powersocial Items |
| 11 | L R R L Right | none: plays a sound only |

The panel offers rows 0, 2, 3, 5, 7, 8, 9 and 10 and the team photo, the cheats IGN's list names.
