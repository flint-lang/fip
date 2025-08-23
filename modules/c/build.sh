#!/usr/bin/env sh

ROOT="$(pwd)";

cd "$ROOT/modules/c" || exit 1

clang "-I$ROOT" -g -std=c17 -o example_master example_master.c "$ROOT/toml/tomlc17.c" || exit 1
clang "-I$ROOT" -g -std=c17 -o fip-c fip.c "$ROOT/toml/tomlc17.c" || exit 1

cd "$ROOT" || exit 1

"$ROOT/modules/c/example_master"
