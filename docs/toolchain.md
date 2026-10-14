# Toolchain

## Components

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

## Why Clang

The SDK's generated C++ and its CMake presets require clang/clang++ with Ninja; MSVC alone will not build it.

## Usage

Run `tools\env.bat` from `D:\Urbz` before invoking `build.bat`:

```bat
cd /d D:\Urbz
tools\env.bat
build.bat
```

The script prepends `C:\Program Files\LLVM\bin` to `PATH` and imports the MSVC x64 environment (via `vcvars64.bat`) into the current shell. An already-open shell does not see the LLVM install until PATH is refreshed — either run `tools\env.bat` in that shell or open a new one after installing LLVM.

## Verified output

Transcript of `cmd /c tools\env.bat` run from `D:\Urbz` (2026-10-14):

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

CMake suite maintained and supported by Kitware (kitware.com/cmake).
```
