#!/usr/bin/env bash
#
# Regenerates the bitmap fonts in src/ui/fonts/.
#
# LVGL's built-in Montserrat fonts only cover ASCII, so they cannot render "£"
# (U+00A3) or "°" (U+00B0) — both of which the Puck needs: today's saving is in
# pounds and the battery screen shows a cell temperature. These subsets are the
# same typeface at the same weight, plus those glyphs.
#
# The TTF is pulled from the LVGL tag we build against rather than vendored, so
# the output is byte-for-byte reproducible without a 240 KB binary in the tree.
#
# Only needs re-running when a size or the glyph set changes; the generated .c
# files are committed.
#
# Requires Node (for npx lv_font_conv).

set -euo pipefail

LVGL_TAG="v8.4.0"
TTF_URL="https://raw.githubusercontent.com/lvgl/lvgl/${LVGL_TAG}/scripts/built_in_font/Montserrat-Medium.ttf"

# Printable ASCII, plus pound sign, degree sign and middle dot.
RANGE="0x20-0x7E,0xA3,0xB0,0xB7"

# 4 bits per pixel matches LVGL's own built-in Montserrat antialiasing.
BPP=4

SIZES=(14 20 28 48)

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out_dir="${repo_root}/src/ui/fonts"
work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

mkdir -p "${out_dir}"

echo "Fetching Montserrat-Medium.ttf from LVGL ${LVGL_TAG}"
curl -fsSL "${TTF_URL}" -o "${work_dir}/Montserrat-Medium.ttf"

# Run from the work directory and pass bare filenames. lv_font_conv copies its
# own invocation verbatim into a comment at the top of each generated file, so
# absolute paths here end up committed — which put the machine's home directory,
# username and a mktemp path into four tracked source files.
cd "${work_dir}"

for size in "${SIZES[@]}"; do
	echo "Generating puck_font_${size}"
	npx --yes lv_font_conv \
		--font Montserrat-Medium.ttf \
		--range "${RANGE}" \
		--size "${size}" \
		--bpp "${BPP}" \
		--format lvgl \
		--lv-include lvgl.h \
		--no-compress \
		-o "puck_font_${size}.c"
	cp "puck_font_${size}.c" "${out_dir}/"
done

echo
echo "Done. Sizes are declared via LV_FONT_CUSTOM_DECLARE in include/lv_conf.h."
ls -la "${out_dir}"
