# Driver validation matrix

Validation date: 2026-08-22. Device firmware:
`acm32_btl_ofilm_pc_000059`.

## Core libfprint API

| Capability | Implementation | Live result |
|---|---|---|
| Probe/open/close/reopen | Native USB transport and handshake | Pass |
| List stored prints | Command `1f 00`, 32-byte bitset | Pass |
| Enroll | Command `31`, six stages, Windows resend cadence | Pass |
| Identify | Command `32 05 ff ff 00 00` | Match and no-match pass |
| Verify | libfprint emulation through identify | Match and no-match pass |
| Cancel identify/verify/enroll | Command `30` with response resynchronization | Pass |
| Delete one print | Command `0c ID_be16 00 01` | Pass |
| Clear storage | Iterate every occupied bit and delete individually | Pass, including empty storage |
| Native duplicate signal | Parse `0x22` and cancel, if firmware emits it | Firmware `000059` did not emit it |
| Coordinated duplicate prevention | Identify before enroll (fprintd policy) | Existing-finger match pass |
| Persistent storage | On-chip templates survive close/reopen | Pass |
| Arch package installation | `libfprint-oms2388` provides/conflicts `libfprint` | Pass |
| fprintd system enumeration | Official `fprintd 1.94.5-2` over D-Bus | One OMS device found |
| fprintd enrollment/verify | Right index through Omarchy setup | `verify-match` |
| Omarchy authentication | sudo, Polkit, and lock-screen PAM | Configured with password fallback |
| Logical identify suspend/resume | Keep `0x32` armed and pause host polling | Match and no-match pass after resume |
| Real s2idle resume handoff | Active identify suspended/resumed by fprintd | Match delivered; lock unlocked ~0.16 s after resume |
| Integrated power/fingerprint button | ACPI wake plus pending on-chip match | Wake and automatic unlock pass |
| Touch-only s2idle wake | USB remote wake without pressing button | Not observed within 10-second window |
| fprintd sleep readiness | Packaged `--no-timeout` drop-in and enabled service | Active delay inhibitor before suspend |

The driver advertises feature mask `0x17e`: identify, verify, storage, storage
list/delete/clear, and always-on support. It deliberately does not advertise
native duplicate checking because firmware `000059` allowed a proven matching
finger to be enrolled into a second slot without returning `0x22`.

## Boundary and recovery tests

- All-zero not-ready responses are retried.
- Interrupted identify, verify, and enrollment operations cancel cleanly and the
  device reopens afterward.
- Enrollment cancellation before any touch leaves no residual template.
- A fprintd-equivalent identify-before-enroll check rejected a duplicate before
  enrollment and left the template count unchanged.
- A pure-protocol same-finger test first matched slot 0, then demonstrated that
  native `0x31` enrollment still created slot 1 without `0x22`; cleanup restored
  the original index.
- A genuinely new left thumb passed identify no-match, completed all six
  stages in slot 1, and was deleted; the count returned to one.
- Clear-storage removed two templates, survived close/reopen, and produced an
  all-zero 256-bit index. Clearing already-empty storage also succeeded.
- Default Linux USB `power/control=auto` works; forcing `on` is unnecessary.
- Three real s2idle cycles completed without suspend/resume failures. The first
  exposed a late fprintd startup race; package release 3 fixes it by keeping
  fprintd alive with its logind delay inhibitor. Controlled cycles entered
  sleep only after the lock screen's fingerprint PAM was actively waiting.
- In the final instrumented cycle, the OMS USB controller `0000:63:00.3` and
  the integrated ACPI power-button path both recorded new events. Pressing the
  combined button woke the machine and the pending match unlocked Omarchy
  about 160 ms after resume. A light touch alone did not wake it in 10 seconds.
- Standalone protocol parser tests and libfprint `fpi-device`, `fpi-ssm`, and
  `fpi-assembling` tests pass with warnings treated as errors.

## Capacity

The device returns a 32-byte index, representing 256 template IDs. The Windows
engine treats IDs 0 through 29 as its usable capacity. Linux follows the same
30-slot allocation/full policy while still listing and clearing all 256 bits.

## Intentionally not implemented

- Raw image capture: this match-on-chip protocol does not expose image data on
  the confirmed operational path.
- Template update: no confirmed protocol command and not required for MOC
  identify/verify.
- Touch-only remote wake is not synthesized in software. On this laptop the
  combined power/fingerprint button supplies the wake event; the driver keeps
  identify active across suspend so the hardware match can unlock immediately
  after resume.

Current device state after the matrix: one fprintd-managed right-index template
for user `zhe` in hardware slot 0.
