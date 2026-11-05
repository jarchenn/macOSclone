#!/bin/sh
# Build the bootable image(s). Usage: tools/build_image.sh [release|test|panic ...]
set -e
cd "$(dirname "$0")/.."
[ $# -eq 0 ] && set -- release
exec python3 tools/build.py build --variant "$@"
