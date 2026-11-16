#!/usr/bin/env bash
set -euo pipefail
: "${DEVKITPRO:?Set DEVKITPRO to the devkitPro installation directory}"
output=${1:?Usage: build-positional-libnx.sh OUTPUT_DIRECTORY}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_dir="$output/source"
revision=7644c9b26099aa2d2145bc72a21ee24190e92085
if command -v dkp-pacman >/dev/null; then package_manager=dkp-pacman
else package_manager=pacman; fi
installed=$($package_manager -Q libnx)
case "$installed" in
    'libnx 4.12.0-'*) ;;
    *) echo "Unsupported libnx package: $installed; require audited 4.12.0 source" >&2; exit 1 ;;
esac
mkdir -p "$output/lib" "$source_dir"
git -C "$source_dir" init -q
git -C "$source_dir" fetch -q --depth 1 https://github.com/switchbrew/libnx.git "$revision"
git -C "$source_dir" checkout -q --detach FETCH_HEAD
test "$(git -C "$source_dir" rev-parse HEAD)" = "$revision"
cp "$script_dir/../source/horizon_fsdev_positional_io.inc" "$source_dir/nx/source/runtime/devices/"
printf '\n#include "horizon_fsdev_positional_io.inc"\n' >> "$source_dir/nx/source/runtime/devices/fs_dev.c"
"$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc" \
    -O2 -g -Wall -Wextra -Werror -Wno-error=old-style-declaration -Wno-error=sign-compare -Wno-error=unused-parameter -ffunction-sections -fdata-sections \
    -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec \
    -D__SWITCH__ -DLIBNX_NO_DEPRECATION \
    -I"$DEVKITPRO/libnx/include" -iquote "$DEVKITPRO/libnx/include/switch" \
    -c "$source_dir/nx/source/runtime/devices/fs_dev.c" -o "$output/fs_dev.o"
cp "$DEVKITPRO/libnx/lib/libnx.a" "$output/lib/libnx.a"
"$DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar" t "$output/lib/libnx.a" | grep -qx fs_dev.o
"$DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar" rcs "$output/lib/libnx.a" "$output/fs_dev.o"
"$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm" "$output/lib/libnx.a" > "$output/symbols.txt"
grep -q ' T morrowFsdevPread$' "$output/symbols.txt"
grep -q ' T morrowFsdevPwrite$' "$output/symbols.txt"
printf 'Built positional I/O libnx from %s\n' "$revision"
