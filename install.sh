#!/usr/bin/env bash
# Builds chibicc-g2 and installs it for the current user:
#
#   ~/.local/lib/chibicc-g2/chibicc-g2   the compiler
#   ~/.local/lib/chibicc-g2/include/     the g2 headers (g2.h, g2lib.h, ...)
#   ~/.local/bin/chibicc-g2              symlink to the compiler
#
# The compiler searches the include/ directory next to its real location,
# so `#include <g2.h>` works from any project. Set PREFIX to install
# somewhere other than ~/.local.
set -euo pipefail

cd "$(dirname "$(realpath "$0")")"

prefix="${PREFIX:-$HOME/.local}"
libdir="$prefix/lib/chibicc-g2"
bindir="$prefix/bin"

make

rm -rf "$libdir"
mkdir -p "$libdir" "$bindir"
cp build/chibicc-g2 "$libdir/"
cp -r include "$libdir/include"
ln -sfn "$libdir/chibicc-g2" "$bindir/chibicc-g2"

echo "Installed chibicc-g2 to $libdir"
echo "Headers are in $libdir/include"
case ":$PATH:" in
  *":$bindir:"*) ;;
  *) echo "Note: $bindir is not in your PATH" ;;
esac
