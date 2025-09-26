#!/usr/bin/env bash

ROOT="$(cd "$(dirname "$0")" && cd ../.. && pwd)"
cd "$ROOT/modules/c" || exit 1

# Path to the flintc project where LLVM is built
FLINTC_ROOT="$(cd "$ROOT/../flintc" && pwd)"

# $1 - Whether to get libs for 'linux' or 'mingw'
get_clang_libs() {
    local platform="$1"
    local lib_dir="$FLINTC_ROOT/build/llvm-${platform}/lib"

    # Use all clang and LLVM static libraries - let --gc-sections handle the rest
    for lib in "$lib_dir"/libclang*.a "$lib_dir"/libLLVM*.a; do
        if [ -f "$lib" ]; then
            # Skip import libraries (.dll.a files)
            if [[ "$lib" == *.dll.a ]]; then
                continue
            fi
            echo -n "$lib "
        fi
    done
}

read -ra clang_libs <<< "$(get_clang_libs "linux")"
# echo "Clang Libraries: ${clang_libs[*]}"

declare -a base_flags
base_flags=(
	"-I$ROOT"
	"-I$FLINTC_ROOT/vendor/llvm-linux/include"
	"-std=c17"
	"-Wall"
	"-Wextra"
	"-Werror"
	"-Wno-unused-variable"
	"-Wno-deprecated-declarations"
	"-ffunction-sections"
	"-fdata-sections"
	"-O3"
	"-flto"
	"-DNDEBUG"
	"-ffast-math"
	"-march=x86-64"
	"-mtune=generic"
)

echo "Building Linux binaries..."

# Build example_master (doesn't need clang)
echo "  Building example_master..."
clang "${base_flags[@]}" -o \
    example_master example_master.c "$ROOT/toml/tomlc17.c" \
    -Wl,--gc-sections -Wl,--as-needed -Wl,--strip-all || {
    echo "Error: Failed to build example_master"
    exit 1
}

# Build fip-c with dead code elimination and LTO
echo "  Building fip-c..."
clang "${base_flags[@]}" \
    -Wl,--gc-sections -Wl,--as-needed -Wl,--strip-all -Wl,-O1 \
    -o fip-c fip.c "$ROOT/toml/tomlc17.c" \
    -Wl,--start-group "${clang_libs[@]}" -Wl,--end-group \
    -lstdc++ -lm -lpthread -ldl -lz || {
    echo "Error: Failed to build fip-c"
    exit 1
}

# Post-build size optimization
echo "  Optimizing binary size..."
if command -v strip >/dev/null 2>&1; then
    ORIGINAL_SIZE=$(stat -c%s fip-c)
    strip --strip-all fip-c || echo "Warning: strip failed"
    STRIPPED_SIZE=$(stat -c%s fip-c)
    echo "  fip-c original size: $((ORIGINAL_SIZE / 1024 / 1024))MB"
    echo "  fip-c stripped size: $((STRIPPED_SIZE / 1024 / 1024))MB"
else
    echo "  strip command not available"
fi

echo "Building Windows binaries with MinGW..."
# Check if MinGW is available
if command -v x86_64-w64-mingw32-gcc &> /dev/null; then
    MINGW_CC="x86_64-w64-mingw32-gcc"
elif command -v mingw32-gcc &> /dev/null; then
    MINGW_CC="mingw32-gcc"
else
    echo "Warning: MinGW not found, skipping Windows build"
    MINGW_CC=""
fi

if [ -n "$MINGW_CC" ]; then
    # Windows-specific flags
    declare -a win_base_flags
    win_base_flags=(
        "-I$ROOT"
        "-I$FLINTC_ROOT/vendor/llvm-mingw/include"
        "-std=c17"
        "-static"
        "-Wall"
        "-Wextra"
        "-Wno-unused-variable"
        "-Wno-deprecated-declarations"
        "-Wno-stringop-truncation"
        "-Wno-attributes"
        "-Wno-sizeof-pointer-memaccess"
        "-Wno-format-truncation"
        "-ffunction-sections"
        "-fdata-sections"
        "-O3"
        "-flto"
        "-DNDEBUG"
        "-ffast-math"
        "-march=x86-64"
        "-mtune=generic"
        "-D__WIN32__"
        "-DCINDEX_LINKAGE="
        "-DLLVM_STATIC_LINK"
        "-DLIBCLANG_STATIC_LINK"
        "-DCINDEX_NO_EXPORTS"
        "-D_CINDEX_LIB_"
    )

    read -ra win_clang_libs <<< "$(get_clang_libs "mingw")"
    # echo "Windows Clang Libraries: ${win_clang_libs[*]}"

    echo "  Building example_master.exe..."
    $MINGW_CC "${win_base_flags[@]}" -o \
        example_master.exe example_master.c "$ROOT/toml/tomlc17.c" \
        -Wl,--gc-sections -Wl,--as-needed -Wl,--strip-all || {
        echo "Error: Failed to build example_master.exe"
        exit 1
    }

    echo "  Building fip-c.exe..."
    $MINGW_CC "${win_base_flags[@]}" \
        -Wl,--gc-sections -Wl,--as-needed -Wl,--strip-all -Wl,-O1 \
        -o fip-c.exe fip.c "$ROOT/toml/tomlc17.c" \
        -Wl,--start-group "${win_clang_libs[@]}" -Wl,--end-group \
        -lstdc++ -lm -lpthread -lole32 -luuid -ladvapi32 \
        -lshell32 -lpsapi -ldbghelp -limagehlp -lversion -lntdll || {
        echo "Error: Failed to build fip-c.exe"
        exit 1
    }

    # Strip Windows binary
    echo "  Optimizing fip-c.exe size..."
    if command -v x86_64-w64-mingw32-strip >/dev/null 2>&1; then
        ORIGINAL_SIZE=$(stat -c%s fip-c.exe)
        x86_64-w64-mingw32-strip --strip-all fip-c.exe || echo "Warning: strip failed for Windows binary"
        STRIPPED_SIZE=$(stat -c%s fip-c.exe)
        echo "  fip-c.exe original size: $((ORIGINAL_SIZE / 1024 / 1024))MB"
        echo "  fip-c.exe stripped size: $((STRIPPED_SIZE / 1024 / 1024))MB"
    else
        echo "  strip command not available"
    fi
fi

cd "$ROOT" || exit 1

cp "$ROOT/modules/c/fip-c" "$ROOT/.fip/modules/fip-c"

"$ROOT/modules/c/example_master"
