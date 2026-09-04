#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${LIBFPRINT_SOURCE_DIR:-${project_root}/build/libfprint-src}"
build_dir="${LIBFPRINT_BUILD_DIR:-${project_root}/build/libfprint-oms-repro}"

"${project_root}/scripts/prepare-libfprint.sh" "${source_dir}"

meson_options=(
  -Ddrivers=oms2388
  -Dintrospection=true
  -Ddoc=false
  -Dinstalled-tests=false
  -Dudev_rules=disabled
  -Dudev_hwdb=disabled
  -Dwerror=true
)

if [[ -f "${build_dir}/build.ninja" ]]; then
  meson setup --reconfigure "${build_dir}" "${source_dir}" "${meson_options[@]}"
else
  meson setup "${build_dir}" "${source_dir}" "${meson_options[@]}"
fi

meson compile -C "${build_dir}"
meson test -C "${build_dir}" fpi-device fpi-ssm fpi-assembling udev-hwdb \
  --print-errorlogs
