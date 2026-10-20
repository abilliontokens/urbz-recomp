# Toolchain

Two targets, built from the same tree and the same recompiled code: Windows (Direct3D 12 +
XAudio2) and Linux (Vulkan + SDL3). The recompiler's output is platform-neutral, so `generated/`
is shared.

## Windows host

| Component | Version | Path |
| --- | --- | --- |
| clang / clang++ | 23.1.3 | `C:\Program Files\LLVM\bin` |
| lld (lld-link) | 23.1.3 | `C:\Program Files\LLVM\bin\lld-link.exe` |
| ninja | 1.13.2 | on PATH |
| cmake | 4.4.3 | on PATH |
| git | 2.55.0.windows.5 | on PATH |
| python | 3.12.10 | on PATH |
| MSVC toolset | 14.44.35207 (VS 2022 Build Tools v17.14.40) | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207` |
| Windows SDK | 10.0.26100.0 | `C:\Program Files (x86)\Windows Kits\10` |

The SDK's generated C++ and its CMake presets require clang/clang++ with Ninja; MSVC alone will
not build it.

Run `tools\env.bat` before `build.bat`:

```bat
cd /d D:\Urbz
tools\env.bat
build.bat
```

The script prepends `C:\Program Files\LLVM\bin` to `PATH` and imports the MSVC x64 environment
(via `vcvars64.bat`) into the current shell. An already-open shell does not see the LLVM install
until PATH is refreshed. Transcript of `cmd /c tools\env.bat` (2026-10-14):

```text
**********************************************************************
** Visual Studio 2022 Developer Command Prompt v17.14.40
** Copyright (c) 2025 Microsoft Corporation
**********************************************************************
[vcvarsall.bat] Environment initialized for: 'x64'

=== Toolchain versions ===
clang version 23.1.3 (https://github.com/llvm/llvm-project 0d261d1ca552c95a8f007e061c787ac7132fbcbc)
Target: x86_64-pc-windows-msvc
Thread model: posix
InstalledDir: C:\Program Files\LLVM\bin
Build config: +alloc:rpmalloc
1.13.2
cmake version 4.4.3
```

## Linux host

Observed on the machine the Linux target was brought up on (CachyOS, kernel 7.2.11, KDE Plasma on
Wayland):

```
$ clang --version
clang version 23.1.3
Target: x86_64-pc-linux-gnu

$ cmake --version   -> 4.4.4
$ ninja --version   -> 1.13.2
$ python3 --version -> 3.14.8
$ glslangValidator --version
Glslang Version: 11:16.6.0
```

What the `linux-release` configure resolved:

| Need | Found |
| --- | --- |
| Vulkan loader / headers | `/usr/lib/libvulkan.so`, `/usr/include` |
| glslang (HLSL -> SPIR-V, linked) | `/usr/lib/libglslang.so`, `libSPIRV.so`, `libglslang-default-resource-limits.so` |
| DXC (HLSL -> SPIR-V, `dlopen`ed at run time) | headers `/opt/dxc/include`, `/opt/dxc/lib/libdxcompiler.so` |
| zlib (the splash PNG decoder) | `/usr/lib/libz.so` |

SDL3 (`release-3.4.16`), Dear ImGui (`v1.91.9b`), toml++ (`v3.4.0`) and Zydis (`v4.1.1`) are
fetched and linked statically by the SDK, so no system copy of any of them is used.

GPU on this machine: AMD `Cyan Skillfish [BC-250]` at `01:00.0`, reported by the runtime as
`AMD BC-250 (RADV GFX1013) (vendor 1002 device 13fe, driver 109060100)`. Presentation goes through
SDL3, so Wayland (`WAYLAND_DISPLAY=wayland-1`) and Xwayland (`DISPLAY=:0`,
`XAUTHORITY=/run/user/1000/xauth_*`) both work; a shell that inherits neither cannot open a window.

## Linux target

```
./build.sh                    # linux-release, into out/build/linux-release/Urbz
./build.sh linux-relwithdebinfo
./release.sh                  # Urbz-linux-x64.tar.zst
```

`build.sh` mirrors `build.bat`: configure, build `xbe_symbols` and write `config/game.toml` on
first run, then recompile the XBE and build the game. A full build of all 530 targets took 118 s
here.

`release.sh` relinks with `CMAKE_BUILD_RPATH='$ORIGIN/lib'` and `-Wl,--disable-new-dtags`, then
packages the binary with glslang and SPIRV-Tools in `lib/` beside it. The old-style `DT_RPATH` is
deliberate: the default `DT_RUNPATH` applies only to the binary's own direct dependencies, and
`libglslang` pulls in `libSPIRV-Tools`, so a `RUNPATH` package silently falls through to the
system copy and breaks on the machines it is for.

## Windows target, cross-compiled from Linux

This is how the released `Urbz.exe` is built. `clang-cl` and `lld-link` (the LLVM 23.1.3 above)
against Microsoft's CRT and Windows SDK as downloaded by
[xwin](https://github.com/Jake-Shadle/xwin), through `cmake/windows-cross.cmake` and the
`windows-cross` preset. Observed: xwin 0.10.0, MSVC CRT 14.44.17.14, Windows SDK 10.0.26100.

```
$ cargo install xwin --locked
$ xwin --accept-license --arch x86_64 --cache-dir ~/.cache/xwin \
      splat --use-winsysroot-style --preserve-ms-arch-notation --output ~/.cache/xwin/sysroot
$ cmake --preset host-tools    && cmake --build --preset host-tools
$ cmake --preset windows-cross && cmake --build --preset windows-cross
```

`host-tools` builds `xbe_recomp` and `xbe_symbols` for this machine; the cross build runs that
binary to recompile the XBE instead of building it for Windows (`XBE_HOST_TOOLS_DIR`). `XWIN_DIR`
(cache or environment) points at another xwin output; it must be the `/winsysroot` layout above,
and it holds only the release CRT, so a Debug cross build does not link. The exe lands in
`out/build/windows-cross/Urbz.exe` (PE32+ GUI, x86-64) and links the DLL CRT
(`VCRUNTIME140.dll`, `MSVCP140.dll`, `api-ms-win-crt-*`) exactly as the native build does, so a
clean Windows needs the Visual C++ 2015-2022 redistributable.

## One Release preset, two meanings - fixed

Targeting the MSVC ABI, clang leaves `CMAKE_<LANG>_FLAGS_<CONFIG>` empty, so the SDK adds `-O2`
itself and `NDEBUG` is never defined. Targeting Linux, clang gets CMake's own defaults, which
carry `-O3 -DNDEBUG` - and the runtime's log verbosity keys off `NDEBUG` (`sdk/runtime/core/log.h`
`kVerbose`). The same `Release` preset therefore produced a verbose, rotating `logs/urbz_NNN.log`
on Windows and, on Linux, a single `logs/urbz.log` holding the boot preamble and nothing else:
no `perf:` lines from `XBE_PERF=1`, no `vulkan: present` geometry, no pad-hook logging. The flag
block at the top of `sdk/cmake/XbeRecomp.cmake` now strips `NDEBUG` on both platforms.

## Headless harness

For GPU-less testing, `sdk/harness/build.sh` builds the harness and `sdk/harness/run.sh` runs it.
See `sdk/harness/README.md`.
