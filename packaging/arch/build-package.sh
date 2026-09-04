#!/usr/bin/env bash
set -euo pipefail

packaging_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${packaging_dir}/../.." && pwd)"
context_dir="${packaging_dir}/.build-context"
source_cache="${packaging_dir}/.source-cache"
package_dir="${packaging_dir}/out"

install -d "${context_dir}" "${source_cache}" "${package_dir}"
install -m644 "${packaging_dir}/PKGBUILD" "${context_dir}/PKGBUILD"
install -m644 "${project_root}/libfprint/integration.patch" \
  "${context_dir}/integration.patch"
install -m644 "${project_root}/libfprint/oms2388/oms2388.c" \
  "${context_dir}/oms2388.c"
install -m644 "${project_root}/libfprint/oms2388/oms2388.h" \
  "${context_dir}/oms2388.h"
install -m644 "${packaging_dir}/fprintd-oms2388.conf" \
  "${context_dir}/fprintd-oms2388.conf"

if [[ -z "${LIBFPRINT_GIT_URL:-}" &&
      -d "${project_root}/build/upstream/libfprint/.git" ]]; then
  LIBFPRINT_GIT_URL="file://${project_root}/build/upstream/libfprint"
  export LIBFPRINT_GIT_URL
  printf 'Using cached upstream repository: %s\n' "${LIBFPRINT_GIT_URL}"
fi

mapfile -t package_paths < <(
  cd "${context_dir}"
  PKGDEST="${package_dir}" makepkg --packagelist
)
main_packages=()
for package_path in "${package_paths[@]}"; do
  if [[ $(basename "${package_path}") == libfprint-oms2388-[0-9]* ]]; then
    main_packages+=("${package_path}")
  fi
done
if (( ${#main_packages[@]} != 1 )); then
  printf 'expected one main package path, found %u\n' \
    "${#main_packages[@]}" >&2
  exit 1
fi

(
  cd "${context_dir}"
  SRCDEST="${source_cache}" PKGDEST="${package_dir}" \
    makepkg --cleanbuild --clean --force --syncdeps --noconfirm
)

"${packaging_dir}/verify-package.sh" "${main_packages[0]}"
