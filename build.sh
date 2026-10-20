#!/usr/bin/env bash
# The Urbz: Sims in the City - native Linux build (Vulkan + SDL3, no Wine).
# Mirrors build.bat: configure, build xbe_symbols and write config/game.toml
# on first run, then build the game.
set -euo pipefail

PRESET="${1:-linux-release}"
cd "$(dirname "$0")"

GAME_XBE="default.xbe"
GAME_EXE="Urbz"
OUT="out/build/${PRESET}"

echo
echo "  The Urbz: Sims in the City - build (${PRESET})"
echo "  ================================================"
echo

if [ ! -f "assets/${GAME_XBE}" ]; then
    echo "  Put the full game files in the assets folder first:" >&2
    echo "    $(pwd)/assets" >&2
    echo "  It needs ${GAME_XBE} with the game's folders beside it." >&2
    exit 1
fi

# toolchain
echo "[1/3] Toolchain"
MISSING=""
for tool in clang clang++ cmake ninja; do
    command -v "$tool" >/dev/null 2>&1 || MISSING="${MISSING} ${tool}"
done
if [ -n "$MISSING" ]; then
    echo "  missing:${MISSING}" >&2
    echo "  Install clang, cmake and ninja, then run this again." >&2
    exit 1
fi
clang++ --version | head -n 1

# symbols
echo "[2/3] XDK symbols"
cmake --preset "${PRESET}" > /dev/null
if [ -f config/game.toml ]; then
    echo "      config/game.toml already present"
else
    cmake --build --preset "${PRESET}" --target xbe_symbols > /dev/null
    mkdir -p config
    "${OUT}/xbe_symbols" "assets/${GAME_XBE}" sdk/xdk --out config/game.toml
fi

# game
echo "[3/3] Game code"
echo "      first build recompiles ${GAME_XBE}, expect a long build ..."
cmake --build --preset "${PRESET}"

echo
echo "  The Urbz: Sims in the City built."
echo "  ${OUT}/${GAME_EXE}"
echo
