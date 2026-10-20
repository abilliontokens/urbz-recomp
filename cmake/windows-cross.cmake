# Cross-compiles the Windows x64 target on a Linux host: clang-cl and lld-link against Microsoft's
# CRT and Windows SDK, downloaded and laid out by xwin (https://github.com/Jake-Shadle/xwin).
# One-time setup, then the windows-cross preset:
#   cargo install xwin --locked
#   xwin --accept-license --arch x86_64 --cache-dir ~/.cache/xwin \
#       splat --use-winsysroot-style --preserve-ms-arch-notation --output ~/.cache/xwin/sysroot
# XWIN_DIR, as a cache or environment variable, points elsewhere.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT XWIN_DIR)
    if(DEFINED ENV{XWIN_DIR})
        set(XWIN_DIR "$ENV{XWIN_DIR}")
    else()
        set(XWIN_DIR "$ENV{HOME}/.cache/xwin/sysroot")
    endif()
endif()
set(XWIN_DIR "${XWIN_DIR}" CACHE PATH "xwin splat output in the /winsysroot layout (VC/Tools/MSVC, Windows Kits/10)")
if(NOT IS_DIRECTORY "${XWIN_DIR}/VC/Tools/MSVC" OR NOT IS_DIRECTORY "${XWIN_DIR}/Windows Kits/10/Lib")
    message(FATAL_ERROR "XWIN_DIR=\"${XWIN_DIR}\" is not an xwin splat made with --use-winsysroot-style "
        "--preserve-ms-arch-notation (see the top of ${CMAKE_CURRENT_LIST_FILE})")
endif()
# try_compile projects re-read this file without the cache
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR)
# xwin lays out the release CRT only (no msvcrtd.lib), so CMake's own test programs must not be Debug
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

find_program(CMAKE_C_COMPILER clang-cl REQUIRED)
find_program(CMAKE_CXX_COMPILER clang-cl REQUIRED)
# lld-link, not link.exe: it keeps the recompiled weak functions overridable by the host ones
find_program(CMAKE_LINKER lld-link REQUIRED)
find_program(CMAKE_AR llvm-lib REQUIRED)
find_program(CMAKE_RC_COMPILER llvm-rc REQUIRED)
find_program(CMAKE_MT llvm-mt REQUIRED)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# the _INIT forms: CMake appends its MSVC defaults (/DWIN32 /D_WINDOWS /GR /EHsc) to these, where
# setting CMAKE_<LANG>_FLAGS itself would replace them. -march matches the native Windows presets.
set(CMAKE_C_FLAGS_INIT "/winsysroot \"${XWIN_DIR}\" -march=x86-64-v2")
set(CMAKE_CXX_FLAGS_INIT "/winsysroot \"${XWIN_DIR}\" -march=x86-64-v2")
foreach(kind EXE SHARED MODULE)
    set(CMAKE_${kind}_LINKER_FLAGS_INIT "/winsysroot:\"${XWIN_DIR}\"")
endforeach()

# headers and libraries come from the Windows sysroot only; programs (the host tools) from this machine
set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
