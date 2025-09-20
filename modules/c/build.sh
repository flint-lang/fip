#!/usr/bin/env bash

ROOT="$(pwd)";

cd "$ROOT/modules/c" || exit 1

declare -a flags
flags=(
	"-I$ROOT"
	"-g"
	"-std=c17"
	"-Wall"
	"-Wextra"
	"-Werror"
	"-Wno-unused-variable"
	"-Wno-deprecated-declarations"
	"-fno-omit-frame-pointer"
	"-funwind-tables"
)

clang "${flags[@]}" -g -O0 -funwind-tables -fno-omit-frame-pointer -o example_master example_master.c "$ROOT/toml/tomlc17.c" || exit 1
clang "${flags[@]}" -g -O0 -funwind-tables -fno-omit-frame-pointer -o fip-c fip.c "$ROOT/toml/tomlc17.c" -lclang || exit 1

cd "$ROOT" || exit 1

cp "$ROOT/modules/c/fip-c" "$ROOT/.fip/modules/fip-c"

"$ROOT/modules/c/example_master"
