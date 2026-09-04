#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sound_pid=""

stop_sound() {
  if [[ -n "${sound_pid}" ]]; then
    kill "${sound_pid}" 2>/dev/null || true
    wait "${sound_pid}" 2>/dev/null || true
  fi
}

trap stop_sound EXIT INT TERM
"${project_root}/tools/attention-sound.sh" &
sound_pid=$!
"$@"
