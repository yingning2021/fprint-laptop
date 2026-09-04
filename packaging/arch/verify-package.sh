#!/usr/bin/env bash
set -euo pipefail

if (( $# != 1 )); then
  printf 'usage: %s PACKAGE.pkg.tar.zst\n' "$0" >&2
  exit 2
fi

package_file=$1
if [[ ! -f "${package_file}" ]]; then
  printf 'package not found: %s\n' "${package_file}" >&2
  exit 2
fi

package_name=$(pacman -Qp "${package_file}" | awk '{print $1}')
if [[ "${package_name}" != libfprint-oms2388 ]]; then
  printf 'unexpected package name: %s\n' "${package_name}" >&2
  exit 1
fi

package_info=$(pacman -Qip "${package_file}")
if ! grep -Fq 'libfprint=1.94.100' <<<"${package_info}"; then
  printf 'package does not provide the expected libfprint version\n' >&2
  exit 1
fi
if ! grep -Fq 'libfprint-2.so=2-64' <<<"${package_info}"; then
  printf 'package does not provide the expected libfprint SONAME\n' >&2
  exit 1
fi

package_files=$(bsdtar -tf "${package_file}")
for required_path in \
  usr/lib/libfprint-2.so.2.0.0 \
  usr/lib/systemd/system/fprintd.service.d/oms2388.conf \
  usr/lib/udev/rules.d/70-libfprint-2.rules \
  usr/lib/udev/hwdb.d/60-autosuspend-libfprint-2.hwdb; do
  if ! grep -Fqx "${required_path}" <<<"${package_files}"; then
    printf 'package is missing %s\n' "${required_path}" >&2
    exit 1
  fi
done

if ! bsdtar -xOf "${package_file}" \
    usr/lib/systemd/system/fprintd.service.d/oms2388.conf | \
    grep -Fq 'ExecStart=/usr/lib/fprintd --no-timeout'; then
  printf 'fprintd drop-in does not enable no-timeout mode\n' >&2
  exit 1
fi

if ! bsdtar -xOf "${package_file}" \
    usr/lib/udev/hwdb.d/60-autosuspend-libfprint-2.hwdb | \
    grep -Fq 'usb:v33A7p2388'; then
  printf 'udev hwdb does not contain USB ID 33a7:2388\n' >&2
  exit 1
fi

printf 'Verified package: %s\n' "${package_file}"
pacman -Qip "${package_file}"
