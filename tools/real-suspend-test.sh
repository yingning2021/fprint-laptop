#!/usr/bin/env bash
set -euo pipefail

user_name="${1:-$(id -un)}"

# Start fprintd before PrepareForSleep so it can hold its delay inhibitor.
/usr/bin/fprintd-list "${user_name}" >/dev/null

# Establish the lock and its fingerprint PAM conversation before suspending.
/usr/share/omarchy/bin/omarchy-system-sleep-lock 5000

for _ in {1..50}; do
  status="$(omarchy-shell lock status)"
  if jq -e '.secure == true and .fingerprint == true and .authenticating == true' \
      <<<"${status}" >/dev/null; then
    /usr/bin/systemctl suspend
    exit 0
  fi
  sleep 0.1
done

printf 'Fingerprint authentication did not become active; refusing to suspend.\n' >&2
exit 1
