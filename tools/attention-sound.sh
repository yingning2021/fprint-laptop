#!/usr/bin/env bash
set -eu

sound_file="${1:-/usr/share/sounds/freedesktop/stereo/alarm-clock-elapsed.oga}"

trap 'exit 0' INT TERM
while true; do
  /usr/bin/pw-play "$sound_file"
done
