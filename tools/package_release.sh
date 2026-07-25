#!/usr/bin/env bash
#
# Builds the release artifacts into dist/:
#
#   firmware.bin   the OTA image, for the device updating itself
#   merged.bin     the whole flash in one file, for the web installer
#   manifest.json  ESP Web Tools manifest, and what the device reads to find out
#                  whether there is a newer version
#
# One manifest serves both: the installer uses `builds` to flash, and the device
# reads `version` to compare against its own.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/.pio/build/puck"
dist="${repo_root}/dist"

# The single source of truth for the version is the firmware's own string. Taking
# it from the tag instead would let a release be cut whose manifest advertises a
# version the binary does not report — and the device compares against what the
# binary reports, so it would either update in a loop or never update at all.
version="$(sed -n 's/^#define PUCK_FW_VERSION "\(.*\)"$/\1/p' "${repo_root}/src/board_config.h")"
if [[ -z "${version}" ]]; then
	echo "could not read PUCK_FW_VERSION from src/board_config.h" >&2
	exit 1
fi

# When run from a tag, the two must agree.
if [[ -n "${GITHUB_REF_NAME:-}" && "${GITHUB_REF_NAME}" == v* ]]; then
	tag_version="${GITHUB_REF_NAME#v}"
	if [[ "${tag_version}" != "${version}" ]]; then
		echo "tag ${GITHUB_REF_NAME} does not match PUCK_FW_VERSION ${version}" >&2
		echo "bump src/board_config.h or retag; a release must not advertise a version" >&2
		echo "different from the one the firmware reports" >&2
		exit 1
	fi
fi

owner_repo="${GITHUB_REPOSITORY:-markab/SigenStorPuck}"

# Run esptool through PlatformIO rather than finding it ourselves. It is already a
# PlatformIO package (tool-esptoolpy) and pio knows which interpreter has pyserial;
# invoking the bundled script with the system python fails on that missing import.
# One code path that works on a workstation and in CI, so this script can be tested
# before a tag depends on it.

rm -rf "${dist}"
mkdir -p "${dist}"
cp "${build_dir}/firmware.bin" "${dist}/firmware.bin"

# boot_app0 lives in the framework, not the build directory.
boot_app0="$(find "${HOME}/.platformio/packages" -name boot_app0.bin -path '*partitions*' | head -1)"
if [[ -z "${boot_app0}" ]]; then
	echo "could not find boot_app0.bin in the installed framework" >&2
	exit 1
fi

# --flash_mode keep: the bootloader header PlatformIO produced is already correct
# for this board's QIO flash and octal PSRAM. Re-specifying it here is a chance to
# get it wrong for no benefit.
#
# The ESP32-S3's bootloader sits at 0x0, not the 0x1000 used on the original ESP32.
pio pkg exec -- esptool.py --chip esp32s3 merge_bin \
	-o "${dist}/merged.bin" \
	--flash_mode keep \
	--flash_size 16MB \
	0x0 "${build_dir}/bootloader.bin" \
	0x8000 "${build_dir}/partitions.bin" \
	0xe000 "${boot_app0}" \
	0x10000 "${build_dir}/firmware.bin"

# `path` is relative, and that is the whole trick.
#
# An absolute URL into the GitHub release cannot be fetched by the installer page:
# release assets send no access-control-allow-origin header, so a browser blocks the
# cross-origin request and flashing never starts. Relative means the page and the
# binary must sit on one origin, which is what the Pages job arranges by publishing
# merged.bin next to index.html.
#
# It also resolves correctly against the release itself, so pointing anything at the
# release copy of this manifest still works.
cat > "${dist}/manifest.json" <<JSON
{
  "name": "SigenStorPuck",
  "version": "${version}",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        {
          "path": "merged.bin",
          "offset": 0
        }
      ]
    }
  ]
}
JSON

echo "packaged ${version}:"
ls -la "${dist}"
