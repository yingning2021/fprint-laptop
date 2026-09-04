#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-${project_root}/build/libfprint-src}"
commit="$(tr -d '[:space:]' < "${project_root}/libfprint/UPSTREAM_COMMIT")"

if [[ ! -d "${source_dir}/.git" ]]; then
  git clone https://gitlab.freedesktop.org/libfprint/libfprint.git "${source_dir}"
fi

actual_commit="$(git -C "${source_dir}" rev-parse HEAD)"
if [[ "${actual_commit}" != "${commit}" ]]; then
  printf 'Refusing to modify %s: expected commit %s, found %s\n' \
    "${source_dir}" "${commit}" "${actual_commit}" >&2
  exit 1
fi

patch_file="${project_root}/libfprint/integration.patch"
if git -C "${source_dir}" apply --check "${patch_file}" 2>/dev/null; then
  git -C "${source_dir}" apply "${patch_file}"
elif ! git -C "${source_dir}" apply --reverse --check "${patch_file}" 2>/dev/null; then
  printf 'The libfprint integration patch does not apply cleanly to %s\n' \
    "${source_dir}" >&2
  exit 1
fi

install -d "${source_dir}/libfprint/drivers/oms2388"
cp "${project_root}/libfprint/oms2388/oms2388.c" \
   "${project_root}/libfprint/oms2388/oms2388.h" \
   "${source_dir}/libfprint/drivers/oms2388/"

printf 'Prepared libfprint source at %s\n' "${source_dir}"
