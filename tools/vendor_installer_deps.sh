#!/usr/bin/env bash
#
# Mirrors esp-web-tools into web/installer/vendor/ so the installer page serves
# every byte it runs from its own origin.
#
# Why this exists rather than a <script src="https://unpkg.com/esp-web-tools@10">
# tag: that page flashes firmware onto a device. The entry module is only ~3 KB
# and pulls the code that actually does the flashing in through dynamic imports,
# so subresource integrity on the tag would have covered almost none of it — and
# the specifier floated to any 10.x, so what the page ran could change without
# this repository changing at all. It was also a live dependency at the moment of
# flashing: while writing this, unpkg was returning HTTP 500 for one of the
# chunks, which would have failed a flash halfway with nothing to point at.
#
# The tarball comes from the npm registry and is checked against the integrity
# hash the registry publishes for that exact version, so a corrupted or
# substituted download is caught here rather than shipped.
#
# Re-run to move to a new version, and commit the result. The chunk names are
# content hashes chosen by the upstream bundler and change with every release,
# which is why this mirrors a directory rather than a list of files.
#
# Usage:  tools/vendor_installer_deps.sh [version]

set -euo pipefail

VERSION="${1:-10.4.0}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out_dir="${repo_root}/web/installer/vendor/esp-web-tools"
work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

echo "Fetching esp-web-tools ${VERSION} from the npm registry"
meta="$(curl -fsSL "https://registry.npmjs.org/esp-web-tools/${VERSION}")"
tarball="$(printf '%s' "${meta}" | python3 -c 'import sys,json;print(json.load(sys.stdin)["dist"]["tarball"])')"
expected="$(printf '%s' "${meta}" | python3 -c 'import sys,json;print(json.load(sys.stdin)["dist"]["integrity"])')"

curl -fsSL "${tarball}" -o "${work_dir}/pkg.tgz"

# The registry publishes this as "sha512-<base64>", the same format npm itself
# verifies against.
actual="sha512-$(openssl dgst -sha512 -binary "${work_dir}/pkg.tgz" | openssl base64 -A)"
if [ "${actual}" != "${expected}" ]; then
	echo "Integrity mismatch for esp-web-tools ${VERSION}" >&2
	echo "  expected ${expected}" >&2
	echo "  got      ${actual}" >&2
	exit 1
fi
echo "Integrity verified: ${expected}"

tar xzf "${work_dir}/pkg.tgz" -C "${work_dir}"

# dist/web is the browser build. Its modules reference each other with relative
# specifiers only — no bare imports — so they need no rewriting to be served
# from a static host.
rm -rf "${out_dir}"
mkdir -p "${out_dir}"
cp "${work_dir}"/package/dist/web/*.js "${out_dir}/"

{
	echo "esp-web-tools ${VERSION}"
	echo "${expected}"
	echo "Mirrored by tools/vendor_installer_deps.sh from ${tarball}"
	echo "Upstream: https://github.com/esphome/esp-web-tools (Apache-2.0)"
} > "${out_dir}/VERSION"
cp "${work_dir}"/package/LICENSE* "${out_dir}/" 2>/dev/null || true

echo
echo "$(ls -1 "${out_dir}"/*.js | wc -l | tr -d ' ') files, $(du -sh "${out_dir}" | cut -f1), in web/installer/vendor/esp-web-tools/"
