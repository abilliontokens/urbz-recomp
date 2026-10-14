# Input path and raw draw path — The Urbz: Sims in the City (XBE static recomp)

All addresses are guest addresses in `out/game.exe` (flat 1:1 map, base 0x10000).
"Matched" means `config/game.toml [symbols]` has a non-zero address for the name, so the
SDK interposes a host implementation; "unmatched" (`= 0`) means the image's original XDK
code runs as guest code. Everything below was derived from the IDA database and the
generated sources; statements that rest on inference rather than direct evidence are
marked **[INFERENCE]**.

Update context: the Start-press and post-title black-screen issues are already fixed.
The black screen was caused by this engine's own push-buffer writer at `sub_001DF070`
(vertex-shader constants inline, methods `0x1EA4` + `0xB80`); the host parser now
handles them. This report therefore concentrates on **the rest of the raw-packet
writer family** and what else it pushes that a host parser may still skip.

---

## 1. Input path

### 1.1 Who polls, and how often

| Stage | Address | Role |
|---|---|---|
| Application thread body | `sub_0018EE70` | App object (`dword_3588AC`, vtable `0x2BD0A0`) loop: `Init` (vtbl+84 = `0x18EEC0`), then `do Frame(vtbl+88 = sub_0018EED0) while (!quit)`, then `Shutdown` (vtbl+96). |
| Per-frame tick | `sub_0018E480` | Frame-time bookkeeping, then `mov ecx,[0x3588C4]; mov eax,[ecx]; call [eax+4]` at `0x18E635-0x18E642` — the input manager update. It is reached as a virtual (vtable entry at `0x2CF638`) and via thunk `sub_001B0300` (vtable `0x2D0268`), i.e. a registered per-frame process object. **[INFERENCE]** the framework iterator runs it once per rendered frame on the Application thread. |
| Input manager | singleton pointer `dword_3588C4` | constructed by `sub_001927F0` (writes vtable `0x2CF848`), from static init `sub_00246B70` (`0x246B75`). vtable slots: +0 dtor `0x192800`, **+4 `sub_001929B0` (per-frame update)**, +8 `sub_00192840` (init/open pads). |
| Pad manager init | `sub_00192840` | creates 4 pad objects (612 bytes each, vtable `0x2CF860`) into the array at `0x2F027C..0x2F028C` (`dword_3588C0` points at it). Calls the XDK inline **`XGetDevices`** helper `sub_002B1AB6` (`0x192923`) — which reads `XPP_GamepadType` (`0x2B05F0`) Current/Change/Previous words under KeRaiseIrql — and for each connected bit calls `XInputOpen` (`0x192951`), storing the handle at padobj+516 (0x204). |
| Per-frame pad update | `sub_001929B0` | calls the inline **`XGetDeviceChanges`** helper `sub_002B1AD8` (`0x1929C6`) for insertions/removals (open/close via `XInputOpen`/`XInputClose`/`XInputSetState`), then calls pad vtbl+84 on each of the 4 pads — twice per pad per invocation (once in the device-change loop `0x192A49..`, once in the tail loop over `0x2F027C..0x2F028C`). |
| Per-pad poll | `sub_00192170` (pad vtbl+84) | if padobj+516 (handle) != 0 → `XInputGetState(handle, XINPUT_STATE*)(padobj+520)` at **`0x1921C0`**; then translates and stores (below). |

So: **polling happens once per frame on the Application thread**, per connected pad.
The SDK host log `xapi: controller ports now 1` corresponds to `XPP_GamepadType.Current = 1`
set by the host's `UpdatePorts()`, which is exactly what `XGetDevices`/`XGetDeviceChanges`
consume in `sub_00192840`/`sub_001929B0`.

Boot-time registration: game main `sub_001CE6F0` calls `sub_001B0180`, which calls the
XPP helper `sub_002B1AB1(2, {XPP_GamepadType,4, XPP_MemoryUnitType,4})` at `0x1B01D3`
(unmatched, guest copy) — the `XInitDevices(2, types)` registration shape — after which
the host's matched `XInitDevices` (`0x2B0EC8`) zeroes/owns the three XPP words.

### 1.2 Where the state lives

XINPUT_STATE at **padobj+520 (0x208)**: packet number +0x208, `wButtons` at **+0x20C**,
`bAnalogButtons[8]` at **+0x20E..0x215** (A,B,X,Y,Black,White,LTrigger,RTrigger),
`sThumbLX/LY/RX/RY` at **+0x216/0x218/0x21A/0x21C**. XINPUT_VIBRATION at +0x21E/+0x220.

Per-pad history object (120 bytes, allocated in `sub_0018D4B0`, pointer at **padobj+508 (0x1FC)**):

| offset | meaning (from `sub_001D0340/0x1D03F0/0x1D0490`) |
|---|---|
| +0x00 | current button word (game encoding) |
| +0x04 | previous frame button word |
| +0x08/+0x10/+0x18/+0x20 | accumulators: ever-pressed / ever-released / held-or-current / secondary |
| +0x70 (112) | **"newly pressed this frame" bits (rising edges)** |
| +0x74 (116) | latched "already reported" bits |

Every poll ends with `sub_0018DBE0` (`0x18DBE0`): `hist[1] = hist[0]; hist[0] = new_mask;`
then falls into the edge scanner **`sub_0018DC16`**:

```c
// 0x18DC30 test eax, ebx ; 0x18DC36 and ebx, ebp (new & bit)
// 0x18DC3A test edi, eax (old & bit) ; 0x18DC3E or [esi+0x70], eax
if ((new & bit) && !(old & bit)) hist[0x70] |= bit;   // rising edge
else if (!(new & bit) && (old & bit)) hist[0x70] &= ~bit;
hist[0x08] |= hist[0x00]; hist[0x10] |= ~hist[0x00]; ...
```

Auto-repeat: `sub_0018D6D0` (`0x18D6D0`) counts down per-button timers at
padobj+440..471 (delay byte padobj+472 = 10, rate padobj+473 = 3, set in the ctor
`sub_0018D4B0`) and ORs held bits into padobj+436 ("repeat fire" word).

### 1.3 Raw → game button mapping

The mapping from `XINPUT_GAMEPAD` to the game's 32-bit button word is done in
**`sub_001923A0`** (called from `sub_00192170` at `0x1921E6`):

```asm
001923A0  mov dx, [ecx+0x20C]      ; wButtons
001923A9  test dl, 0x10           ; Start
001923AE  mov eax, 0x800          ; -> game bit 0x0800
001923B3  test dl, 0x20           ; Back  -> 0x0100
```

| Source field | Xbox bit / byte | Game bit |
|---|---|---|
| wButtons Start | 0x0010 | **0x0800** |
| wButtons Back | 0x0020 | 0x0100 |
| wButtons D-pad | 0x0001/0x0002/0x0004/0x0008 | 0x1000/0x4000/0x8000/0x2000 |
| wButtons L/R thumb | 0x0040/0x0080 | 0x0200/0x0400 |
| bAnalogButtons A/B/X/Y (+0x20E..0x211) | != 0 | 0x40/0x20/0x80/0x10 |
| bAnalogButtons Black/White (+0x212/0x213) | != 0 | 0x02/0x01 |
| bAnalogButtons LTrig/RTrig (+0x214/0x215) | != 0 | 0x04/0x08 |
| sThumbLX (this+0x216) | > +dz → 0x020000, < −dz → 0x080000 | (deadzone `dword_2D2720`, scale `2D2718`, offsets `2D2658`) |
| sThumbLY (+0x218) | ± | 0x10000 / 0x40000 |
| sThumbRX (+0x21A) | ± | 0x200000 / 0x800000 |
| sThumbRY (+0x21C) | ± | 0x100000 / 0x400000 |

The game reads **everything**: the full `wButtons` word, all eight analogue button
bytes (digitalised: any non-zero byte counts), and all four thumb axes with a
deadzone/scale pass. Trigger and face-button *depth* is discarded (byte != 0 test only).

### 1.4 The Start edge test

- Current word: `hist[0]` at `*(padobj+0x1FC)+0`; previous word: `hist[1]`.
- Start bit: `0x0800`, created at `0x1923A9-0x1923AE`.
- Rising-edge store: **`0x18DC3E`** (`or [esi+0x70], eax` inside `sub_0018DC16`), i.e.
  "Start is newly pressed this frame" = `bit 0x800` of `hist+0x70`.
- Consumers do not test `0x800` as an immediate anywhere in the generated code — the
  UI/game query the history words with masks that come from data (the UI is data-driven;
  e.g. the startscreen logic in `sub_000A8581` combines pad state into the flag word
  `dword_35A7FC`, bits 0x2/0x8/0x10/0x20/0x80). **[INFERENCE]** menu code asks
  "pressed-this-frame / held / repeated" per button through the history object rather
  than through hard-coded constants, which is why no `test ..., 0x800` site exists.

---

## 2. Title / attract state machine

- Boot: CRT start `sub_001F0DFF` → game main `sub_001CE6F0` (`sub_001B0180` = XInitDevices
  registration, `sub_0018F100` App init, big init `sub_000118B0`) → Application thread
  `sub_0018EE70` → frame loop `sub_0018EED0`.
- `App::Frame` (`sub_0018EED0`) is a small scene state machine on the App object:
  fields App+304 (`this+76`) current state, +308 next state, +312 current scene object,
  +316 requested scene. Loading a scene goes through the scene manager singleton at
  `0x36BF68` (vtable `0x2CF818`): `vtbl+12` = `LoadScene(name)` (`0x191BE0` → core
  `sub_0018BAB0`), `sub_00191E10` = "is scene loaded", called with hashed names.
- Movie scenes: **`sub_001EC360`** dispatches on a scene-name hash to a movie file —
  `urbz_attract.xmv` (`0x1114865B`, string `0x2D240C`), `urbz_intro.xmv`, `ea_games*.xmv`,
  `urbz_ending*.xmv`, PAL variants. It opens the file, builds the decoder with the
  (unmatched) XMV code at `0x288B20`, allocates a target via `0x249F20`/`0x249F70`, and
  kicks the renderer with `sub_001D3FE0(dword_3589EC, 1, 1)`. Per-frame decode runs in
  `0x1EC240`/`0x1EC9B0`/`0x1ECA20` (more unmatched XMV calls `0x288E37`/`0x288F1B`/`0x288F88`/`0x289107`).
- Title screen: the scene built by **`sub_000A66A0`** (requests id string **`"startscreen"`**
  `0x2C2060`); it creates the "Press Start" UI element (text id **`"press_start_xbox"`**
  `0x2C016C`, referenced at `0x59D4C`/`0x59DD5`) plus five more layer nodes.
- Advance: the startscreen update `sub_000A8581` manipulates the status word
  `dword_35A7FC` — bits 0x2/0x8 gate a fade-out (`sub_000450B0(&dword_35A728)`), bit
  0x20 is set after a timer comparison, bit 0x80 = "already advanced"; when the fade
  completes it marks the screen done (`*(this+24) = 1`) and plays a UI sound
  (`sub_000B4650(...,36,2,...)`). `sub_000A8490` then loads **`"mainmenu"`** (`0x2C25D0`)
  via `vtbl+12` of the scene manager once `sub_00191E10("mainmenu") == 0`.
  `sub_000A6180`/`sub_000A88F0` are the scene enter/leave hooks that set/clear those bits.
- A Start press is (was) ignored while: the intro/attract movie scene is still the
  active scene and has not delivered its "finished" state into App+308 (App::Frame only
  transitions on the scene's vtbl+16 completion call at the end of `sub_0018EED0`), or a
  fade is still in flight (`dword_35A7FC & 0x2/0x8`), or `skip_startup_movies`
  (`0x2BD91C`, read in `sub_00027068`) short-circuits the movie scene entirely.
  With the current fixed build a Start press advances both intro and title, so the
  relevant path was the movie-finished/completion signal, not the button word itself.

---

## 3. `MakeSpace from 001DF091` and the raw push-buffer writer family

### 3.1 `sub_001DF070` — inline vertex-constant writer

```c
// _sub_001DF070 @ 0x1DF070-0x1DF0E9
n = a1 + 3;                                   // dwords, rounded up
if (n >= 0x80) Space = sub_0024E830(n);       // 0x24E830, guest D3D "get sub-buffer space"
else { Space = buf__0; if (buf__0 >= buf__2) Space = D3DDevice_MakeSpace(a2); }  // call at 0x1DF08C, ret 0x1DF091
*Space    = 269988;            // 0x41EA4 : count=1, method 0x1EA4 ("select constant")
Space[1]  = a3 + 96;           // payload = engine register + 96 (c97/c98 = regs 1/2, viewport scale/offset)
Space[2]  = (a1 << 18) + 2944; // count=a1, method 0x0B80 (inline data)
memcpy(Space+3, src, 4*a1);    // a1 dwords of float data
```

This is the engine's own writer for **vertex shader constants** (`cN` where the packet
payload is `N+96`; the fixed c97/c98 viewport scale/offset case). It normally appends
into a guest-side ring (`buf__0`/`buf__2`) and only falls back to the (matched)
`D3DDevice_MakeSpace` when the ring fills — which is the ~2×/second
`MakeSpace from 001DF091` pattern at 60 fps (~every 30 frames).

### 3.2 The sibling writers (all guest .text, all writing into the same buffer)

Low-level packet helpers in the D3D section (all **unmatched**):

| Helper | Packet(s) emitted | Count | Meaning |
|---|---|---|---|
| `sub_001DF070` | head `0x41EA4` (count 1, method `0x1EA4`) + `(N<<18)|0xB80` (count N, method `0x0B80`) | 1+N | vertex-shader constant: payload `reg+96`, then N dwords of float data |
| `sub_0024EE60` | head `8*t + 0x81880` → count 2, method `0x1880+8*t` | 2 | per-vertex 2-dword attribute for engine slot t (t=4,5: UV pairs; t=9: 12.12 fixed-point UV × `dword_2D2680`) |
| `sub_0024EEA0` | head `0x100000+16*t` (t≥0) → count 4, method `0x1A00+16*t`; head `0x101518` (t=−1) → count 4, method `0x1518` | 4 | 4-dword vertex attribute: t=−1 position (x,y,z,1.0); t=2 normal; t=0 another vec4 |
| `sub_0024EF00` | head `4*t + 0x41940` → count 1, method `0x1940+4*t` | 1 | packed-byte attribute: t=3 diffuse (RGBA bytes), t=1 specular |
| `sub_0024EF50` | head `4*t + 0x41940` → count 1, method `0x1940+4*t` | 1 | same, byte-swizzled dword |
| `sub_0024BB90` | head `(64*stage) + 0x41B20` → count 1, method `0x1B20+64*stage` | 1 | **texture bind for stage**: refcount fixup, payload `(addr bits)|fmt` — this is `D3DDevice_SwitchTexture` (see §4) |
| `sub_0024E830` | (no packet) | — | guest allocator for the shared push buffer; calls matched `D3D_MakeRequestedSpace` when >0x80 dwords needed |

Method names are **[INFERENCE]** (numbers are exact: `head & 0x1FFC` per the fixed
c97/c98 packet shape); semantics are read off the engine callers.

Engine-side writers (guest .text) and what they emit:

| Writer | Emitted via | Used for |
|---|---|---|
| `sub_001D8BC0` (`0x1D8BC0`) | `sub_001DF070` ×12+ | per-draw vertex-constant upload: c29/c33/c37(+c41..c44 when skinning)/c45/c49 matrices, c3, c4, c5 (scalar — fog-like), c7–c30 light blocks (6 regs each, 3 lights), plus c65–c68 in `sub_001D9ED0` |
| `sub_001D9ED0` | `D3DDevice_Begin(8)` + `sub_0024EE60/EEA0/EF00` per vertex ×4, `D3DDevice_End` | **2D quad batcher (quads — UI sprites/letterbox)**: per vertex UV (t=4, t=9), diffuse (t=3), position (t=0, w=1.0) |
| `sub_001D79E0` | `Begin(6)` + `EEA0` normal (t=2), `EF00` diffuse (t=3) & specular (t=1), `EE60` UV (t=9), `EEA0` position (−1) | flexible lit-vertex path (tri strips) |
| `sub_001D73F6` | `EF00` (t=3), `EE60` (t=4), `EEA0` (−1) per corner ×2 triangles | transformed 2D quads/lines with per-vertex colour, matrix from `a3[428..442]` (the c97/c98-dependent transform) |
| `sub_001DF0F0` | `Begin(6)` + `EF50` (t=3), `EEA0` (t=2), `EE60` (t=4/t=5), `EF00` (t=1), `EEA0` (−1) | indexed inline draw, two UV sets + specular |
| `sub_001DF320` | `Begin(6)` + `EF00` (t=3), `EE60` (t=4), `EEA0` (t=2), `EF00` (t=1), `EEA0` (−1) | inline draw with optional specular |
| `sub_001D4C10`, `sub_001D4DA0`, `sub_001D4E70`, `sub_001D89E0` | `Begin(2/4/5)` + same attribute helpers | renderer dispatcher (`sub_001D4080`) draw variants (lines/line strip/triangles) |
| `sub_001D2EA0`, `sub_001D5160`, `sub_001D5D60`, `sub_001D9360`, `sub_001E7F60` | `sub_0024BB90` (SwitchTexture) | texture bind/unbind: they call **both** the matched `D3DDevice_SetTexture` *and* the raw SwitchTexture push (bind: `0x1E7F60` `0x1E80FE`; unbinds: `0x1D2F38`, `0x1D5245`, `0x1D5DAE`, `0x1D9713`) |

Renderer command dispatcher `sub_001D4080` (ids 1–11) itself stays on matched calls
(`0x24E320` InsertCallback, `D3DDevice_BlockUntilVerticalBlank`, `0x24B0E0` target
recreate for id 6, `0x24B0E0`/`0x24AE00` kicks via id 5 path `0x1D4AE0`).

### 3.3 Do any of them push pixel-shader / texture-stage / combiner state?

**No pixel-shader or combiner state is pushed raw.** The pixel pipeline is built by
`sub_001EAC90` (`0x1EAC90`), which:
- hashes a pixel-state key (`sub_001EAB60`),
- *generates Xbox pixel-shader assembly as text* at runtime (`"def c6, 1.0f,..."`,
  `"tex t0"`, `"tex t1"`, `mul/mad/xfc` combiner emulations — see the string constants and
  `sub_001EA990`/`sub_001DF2A0`), creates it via the **matched**
  `D3DDevice_CreatePixelShader`,
- binds with matched `D3DDevice_SetPixelShader`,
- uploads combiner constants with **matched** `D3DDevice_SetPixelShaderConstant`
  (c5 = fog scalar from `a2+160`, c4, c0–c3 from the material at `a2+132`),
- sets combiner-related states via **matched** `D3DDevice_SetRenderState_Simple`
  (`0x40344`, `0x40344+4`, `0x40304`-area states).

Texture **stage state** other than the bind (filters, wrap, texcoord index, border
colour) also flows through matched `D3DDevice_SetTextureState_*` /
`SetTextureStageStateNotInline` and the deferred-state block `D3D_DeferredTextureState`
(`0x25A700`). The only texture-related raw packet is SwitchTexture's single dword per
stage (method `0x1B20+64*stage`), and binds are double-pathed (matched SetTexture +
raw push), so a host that honours `D3DDevice_SetTexture` already gets correct binds.

What the host must parse from the raw stream (or these symptoms appear):
- `0x1EA4` + `0xB80` — vertex constants (**fixed** in the other workstream);
- `0x1880+8*t` (2 dwords), `0x1518` (4), `0x1A00+16*t` (4), `0x1940+4*t` (1) —
  the inline vertex data for *every* 2D/quad draw, i.e. the menu UI and backdrop
  geometry itself. If these are skipped, the UI quads draw with stale attribute
  values (wrong/absent UVs → untextured or monochrome look; wrong positions →
  "missing" UI), which matches the current symptoms;
- `0x1B20+64*stage` — stage texture switch (redundant with SetTexture today, but it is
  the only texture signal inside the raw stream).

---

## 4. Unmatched XDK functions on the live path (prioritised)

Prioritised by exposure to the raw-push path and the live draw/input flow. "Candidate"
= the address I believe implements the symbol; evidence quoted.

| Priority | Symbol (toml `= 0`) | Candidate | Evidence | Consequence if left to guest code |
|---|---|---|---|---|
| **P0** | `D3DDevice_SwitchTexture` | `0x24BB90` | pushes head `(64*stage)+0x41B20` (method `0x1B20+64*stage`), refcount fixup, called by renderer bind/unbind paths `0x1D2EA0@0x1D2F38`, `0x1D5160@0x1D5245`, `0x1D5D60@0x1D5DAE`, `0x1D9360@0x1D9713`, `0x1E7F60@0x1E80FE,0x1E810E` | every texture bind emits a raw method the host must parse; if ignored, stale bindings are only masked by the parallel matched `SetTexture` calls |
| **P0** | `D3DDevice_SetVertexShaderConstant1Fast` | `0x24CF30` | `memcpy(&0x259AE0[4*reg], src, 4*cnt)` into the D3D fast-constant scratch, then calls matched `SetVertexShaderConstantNotInlineFast`; called by the per-frame constant uploader `sub_001D3420@0x1D3E23,0x1D3E31` (48 dwords at indices 129/149 — matrix palettes) | if the host's `NotInlineFast` HLE doesn't read the guest scratch at `0x259AE0`, those 96 dwords of constants never reach the shader — broken skinned/animated geometry |
| P1 | *(inner push allocator, no toml name)* | `0x24E830` | shared-space getter used by `sub_001DF070@0x1DF094` and the matched Draw*/LoadVertexShader bodies; falls back to matched `D3D_MakeRequestedSpace` | constant writes < 0x80 dwords land in the guest ring and are only flushed on fallback — the host must drain the guest ring, not just the fallback |
| P1 | XMV decoder family (no toml names) | `0x288B20` (create), `0x288E37` (destroy), `0x288F1B` (begin), `0x288F88` (set window), `0x289107`/`0x28918D` (audio) | called by the movie scene `sub_001EC360@0x1EC826,0x1EC83F`, `0x1EC240@0x1EC2DA`, `0x1EC9B0`, `0x1ECA20` | movies currently decode fine as guest code (CPU + framebuffer path); only a performance/robustness concern |
| P1 | `D3DDevice_MakeSpace` (matched) *caller set* | — | guest .text callers of `0x24E820`: only `sub_001DF070@0x1DF08C` (the fixed path) | none left after the constant fix |
| P2 | `D3DDevice_BeginPush` / `D3DDevice_EndPush` | none found | no guest .text callers of any BeginPush/EndPush-shaped function; inline vertex data uses the attribute helpers instead (§3.2) | none — safe to leave unmatched |
| P2 | `D3DDevice_SetIndices` | not located | no direct .text caller found; streams are bound inside matched `SetStreamSource` region | unknown, probably none |
| P2 | *(backbuffer info copy)* / *(device flag setter)* / *(create index-ish buffer)* | `0x24AD60` (copies 6 dwords from `buf_0+3808`; used by movie scene `0x1ECB30`), `0x24AE10` (sets `device+7608`; used by `sub_001D3420`), `0x24DAB0` (allocates a 24-byte header + data blob via `sub_00252340`; used by `0x1E8370`) | called on live movie/draw paths | work today; if the host ever takes over these structures, these three need names/behaviour |
| P2 | `Lock2DSurface` wrapper *(no toml name)* | `0x24DB60` → `Lock2DSurface`, and `0x24DB40` → `sub_00251FB0` | used by the target/lock path `sub_001D3110@0x1D32A5,0x1D3290` and `0x1D2EA0`, `0x1D9B50`, `0x1EC240`, `0x1E85E0` | surface locks run as guest code against the emulated D3D — currently working; keep an eye on it when touching surfaces |
| P3 | resource refcount helpers *(no toml names)* | `0x24DCF0` (AddRef), `0x24DD30` (recursive Release), `0x24DEA0` (offset cursor) | callers `sub_001D3110`, `sub_001D2EA0`, `sub_001D467D`, `sub_001D4905`, `sub_001D9B50`, `sub_001E8370`, `sub_001EC240` | refcounts/timing stay in guest land; leaks or premature frees would only show under stress |
| P3 | `XInputGetCapabilities` | `0x2B1598` | called by pad capability query `sub_001925A0` (`0x1925B2`), reading vibration caps (used by rumble gating in `sub_00192700`) | guest code runs; returns "has rumble" from emulated device records — match it if rumble misbehaves |
| P3 | `XInitDevices` inner registration *(no toml name)* | `0x2B1AB1` | called by boot `sub_001B0180@0x1B01D3` with {gamepad, MU} type list | benign: host `XInitDevices` re-initialises the same XPP records |
| P4 | `D3DDevice_SetLight` / `SetMaterial` / `SetPalette` / `LightEnable` / `EnableOverlay` / `PersistDisplay` / `GetVertexShaderFunction` / `D3DResource_BlockUntilNotBusy` / `GetRenderTarget2`-adjacent | not located | no guest .text callers found for any of them in the cross-section scan | none today; lighting constants are pushed via `sub_001DF070` (c7–c30) and shading via `sub_001EAC90` instead |
| P4 | `crt_atan`, `crt_cos`, `crt_log`, `crt_log10`, `crt_sin`, `crt_sqrt`, `crt_tan` | cluster `0x230440–0x230A80` (matched anchors: `_CIacos 0x230654`, `crt_acos 0x230671`, `_CIasin 0x230720`, `crt_asin 0x23073D`, `crt_pow 0x230814`; likely `crt_log*` ≈ `0x23047A`, 425 bytes) | the static CRT is linked into the image right next to the matched entries | none at runtime — the in-image copies serve all guest callers; only matters if future host-side code wants to call them by name |

---

## Three changes I would make first

1. **Teach the host push-buffer parser the inline vertex-attribute packets** — methods
   `0x1880+8*t` (2 dwords), `0x1518` (4 dwords), `0x1A00+16*t` (4 dwords),
   `0x1940+4*t` (1 dword) — emitted by `sub_0024EE60/0x24EEA0/0x24EF00/0x24EF50` from
   every 2D/quad writer (`sub_001D9ED0`, `sub_001D73F6`, `sub_001DF0F0`, `sub_001DF320`,
   the `sub_001D4*` draw variants). This is the direct analogue of the c97/c98 fix and
   the first suspect for the monochrome menu backdrop and missing UI quads: those draws'
   UVs, colours and positions live only in these packets.
2. **Match `D3DDevice_SwitchTexture` (`0x24BB90`) in `config/game.toml`** (and drain the
   guest ring via `0x24E830`'s semantics while at it). It is the only texture signal
   inside the raw stream (method `0x1B20+64*stage`), five renderer functions call it
   directly, and relying on the parallel matched `SetTexture` calls leaves stage
   bindings half-host-controlled. Same for `D3DDevice_SetVertexShaderConstant1Fast`
   (`0x24CF30`): either match it or make the host's
   `SetVertexShaderConstantNotInlineFast` read the guest scratch at `0x259AE0`, or the
   96 dwords of matrix-palette constants uploaded per frame are silently dropped.
3. **Match `XInputGetCapabilities` (`0x2B1598`)** on the input path. The pad class
   queries it (`sub_001925A0`) to decide rumble capability and the answer currently
   comes from guest emulation of the XPP records; a host implementation (like the
   existing `XInputGetState` HLE) removes the last piece of guest code between SDL and
   the pad object, and costs ~30 lines.

*(No changes were made outside this report; `sdk/`, `src/` and `config/game.toml` were
only read, never written.)*
