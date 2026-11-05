#!/bin/sh
# Launch Aurora OS in QEMU. Usage: tools/run_qemu.sh [MEMORY] [--bios]
# The exact QEMU command line is printed before launch.
set -e
cd "$(dirname "$0")/.."
mem="${1:-256M}"
shift 2>/dev/null || true
exec python3 tools/build.py run --memory "$mem" "$@"
