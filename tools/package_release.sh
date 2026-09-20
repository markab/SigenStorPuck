#!/usr/bin/env bash
#
# Builds the release artifacts for every board variant into dist/:
#
#   dist/<slug>/firmware.bin   the OTA image, for that board updating itself
#   dist/<slug>/merged.bin     the whole flash in one file, for the web installer
#   dist/<slug>/manifest.json  ESP Web Tools manifest, and what the device reads
#                              to find out whether there is a newer version
#   dist/firmware-<slug>.bin   distinctly-named copies for the GitHub Release,
#   dist/merged-<slug>.bin     whose asset namespace is flat (no subdirectories)
#
# One manifest per variant serves both consumers: the installer uses `builds` to
# flash, and the device reads `version`. Because both boards are ESP32-S3, the
# installer cannot tell them apart from the chip — so each variant gets its own
# manifest, and the device fetches its own by slug (see updater.cpp). The `board`
# field guards against flashing one board's image onto the other, since the two
# share a version.
#
# Every variant is the same ESP32-S3R8 / 16 MB flash, so the chip, flash size,
# partition offsets and merge layout are identical — only firmware.bin's contents
# differ. This is why one packager serves both.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dist="${repo_root}/dist"

# env : slug : human-readable name. The slug names the release directory on Pages
# and the OTA manifest, and must match PUCK_BOARD_SLUG in each board profile.
variants=(
	"puck:puck-1.75:Waveshare ESP32-S3-Touch-AMOLED-1.75 (round)"
	"puck241:puck-2.41:Waveshare ESP32-S3-Touch-AMOLED-2.41 (landscape)"
)

# The single source of truth for the version is the firmware's own string, shared
# by both variants. Taking it from the tag instead would let a release be cut
# whose manifest advertises a version the binary does not report — and the device
# compares against what the binary reports, so it would either update in a loop or
# never update at all.
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

# boot_app0 lives in the framework, not the build directory. Shared by every
# variant (same partition table).
boot_app0="$(find "${HOME}/.platformio/packages" -name boot_app0.bin -path '*partitions*' | head -1)"
if [[ -z "${boot_app0}" ]]; then
	echo "could not find boot_app0.bin in the installed framework" >&2
	exit 1
fi

rm -rf "${dist}"
mkdir -p "${dist}"

for entry in "${variants[@]}"; do
	IFS=: read -r env slug name <<<"${entry}"
	build_dir="${repo_root}/.pio/build/${env}"
	out="${dist}/${slug}"

	if [[ ! -f "${build_dir}/firmware.bin" ]]; then
		echo "no build for ${env} at ${build_dir} — run 'pio run -e ${env}' first" >&2
		exit 1
	fi

	mkdir -p "${out}"
	cp "${build_dir}/firmware.bin" "${out}/firmware.bin"

	# --flash_mode keep: the bootloader header PlatformIO produced is already correct
	# for this board's QIO flash and octal PSRAM. The ESP32-S3's bootloader sits at
	# 0x0, not the 0x1000 used on the original ESP32.
	pio pkg exec -- esptool.py --chip esp32s3 merge_bin \
		-o "${out}/merged.bin" \
		--flash_mode keep \
		--flash_size 16MB \
		0x0 "${build_dir}/bootloader.bin" \
		0x8000 "${build_dir}/partitions.bin" \
		0xe000 "${boot_app0}" \
		0x10000 "${build_dir}/firmware.bin"

	# `path` is relative, and that is the whole trick: release assets send no CORS
	# header, so the installer page must fetch the binary from its own origin. The
	# Pages job publishes this manifest and merged.bin together under <slug>/, so
	# "merged.bin" resolves next to the manifest. `board` is the cross-flash guard.
	cat >"${out}/manifest.json" <<JSON
{
  "name": "SigenStorPuck — ${name}",
  "version": "${version}",
  "board": "${slug}",
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

	# Distinctly-named copies for the GitHub Release (flat asset namespace).
	cp "${out}/firmware.bin" "${dist}/firmware-${slug}.bin"
	cp "${out}/merged.bin" "${dist}/merged-${slug}.bin"

	echo "packaged ${slug} (${env}) ${version}"
done

# Backwards compatibility for the flat, pre-split URL.
#
# 1.75 boards already in the field run firmware whose updater polls
# SigenStorPuck/manifest.json (no slug) — the layout from before this split. That
# path must keep working, or those devices can never make the one OTA hop to
# firmware that uses the per-slug URL: they would poll a 404 forever and be stuck
# on the old version until re-flashed over USB. The 1.75 is the original board, so
# it owns the flat path; new 1.75 firmware polls puck-1.75/ and this alias is only
# ever read by not-yet-updated devices.
cp "${dist}/puck-1.75/manifest.json" "${dist}/manifest.json"
cp "${dist}/puck-1.75/firmware.bin" "${dist}/firmware.bin"
cp "${dist}/puck-1.75/merged.bin" "${dist}/merged.bin"

echo "---"
find "${dist}" -type f | sort
