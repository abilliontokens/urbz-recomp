@echo off
rem Toolchain environment for the Urzb port: LLVM/Clang + MSVC + Ninja.
rem Prepends LLVM to PATH and imports the MSVC amd64 environment into the
rem caller's shell (no setlocal, so PATH changes persist).

set "LLVM_BIN=C:\Program Files\LLVM\bin"

if exist "%LLVM_BIN%\clang++.exe" (
    set "PATH=%LLVM_BIN%;%PATH%"
) else (
    echo ERROR: clang++ not found. Expected LLVM at "%LLVM_BIN%". 1>&2
    exit /b 1
)

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

echo.
echo === Toolchain versions ===
clang++ --version
ninja --version
cmake --version
