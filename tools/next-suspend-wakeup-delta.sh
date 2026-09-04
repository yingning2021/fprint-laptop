#!/usr/bin/env bash
set -euo pipefail

wakeup_sources=/sys/kernel/debug/wakeup_sources
suspend_success=/sys/power/suspend_stats/success

if [[ ! -r "${wakeup_sources}" ]]; then
  printf 'Root access to %s is required.\n' "${wakeup_sources}" >&2
  exit 1
fi

declare -A before_active before_event before_wakeup before_expire

while read -r name active event wakeup expire _; do
  [[ "${name}" == name ]] && continue
  before_active["${name}"]="${active}"
  before_event["${name}"]="${event}"
  before_wakeup["${name}"]="${wakeup}"
  before_expire["${name}"]="${expire}"
done <"${wakeup_sources}"

before_success="$(<"${suspend_success}")"
printf 'READY suspend_success=%s\n' "${before_success}"

for _ in {1..600}; do
  after_success="$(<"${suspend_success}")"
  if (( after_success > before_success )); then
    printf 'SUSPEND_COMPLETE suspend_success=%s\n' "${after_success}"
    break
  fi
  sleep 0.2
done

if (( after_success <= before_success )); then
  printf 'Timed out waiting for the next successful suspend.\n' >&2
  exit 1
fi

printf 'name active_delta event_delta wakeup_delta expire_delta\n'
while read -r name active event wakeup expire _; do
  [[ "${name}" == name ]] && continue
  old_active="${before_active[${name}]:-0}"
  old_event="${before_event[${name}]:-0}"
  old_wakeup="${before_wakeup[${name}]:-0}"
  old_expire="${before_expire[${name}]:-0}"

  if (( active != old_active || event != old_event ||
        wakeup != old_wakeup || expire != old_expire )); then
    printf '%s %+d %+d %+d %+d\n' "${name}" \
      "$((active - old_active))" \
      "$((event - old_event))" \
      "$((wakeup - old_wakeup))" \
      "$((expire - old_expire))"
  fi
done <"${wakeup_sources}"
