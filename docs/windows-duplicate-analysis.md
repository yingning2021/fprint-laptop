# Windows duplicate-enrollment analysis

This note records static analysis of the official OMS 1.0.0.29 Windows package
for USB device `33a7:2388`. The analyzed files are intentionally ignored by Git.

```text
OMSEngineAdapter.dll
sha256 83eaea20603b7e9d8c3bc6739a1fc88e977a29f462c258356e5af548fc7b9dae

OMSFpDriver.dll
sha256 bb22a1015d23673eb1fd19989b9860a18a361e0b9f9cf02645d68988384744d3
```

Addresses below are relative virtual addresses (RVA) in these exact binaries.

## WinBio engine flow

`OMSEngineAdapter.dll` exports only `WbioQueryEngineInterface`. Its returned
`WINBIO_ENGINE_INTERFACE` table maps the enrollment callbacks as follows:

| Callback | RVA |
|---|---:|
| `EngineAdapterCreateEnrollment` | `0x23a0` |
| `EngineAdapterUpdateEnrollment` | `0x24b0` |
| `EngineAdapterGetEnrollmentStatus` | `0x2850` |
| `EngineAdapterGetEnrollmentHash` | `0x28c0` |
| `EngineAdapterCheckForDuplicate` | `0x28e0` |
| `EngineAdapterCommitEnrollment` | `0x2d70` |

`EngineAdapterCreateEnrollment` requests the 32-byte device index through a
WBDI IOCTL. It reports storage full only when bytes 0 through 2 are `ff` and the
low six bits of byte 3 are set. This is exactly IDs 0 through 29; the remaining
226 bits are protocol address space, not Windows enrollment capacity.

`EngineAdapterUpdateEnrollment` reads the three enrollment error bytes returned
by the UMDF driver. If byte 0 is `0x22`, it stops enrollment and asks the driver
to discard the operation. Otherwise it returns the usual more-data status until
the capture count reaches six.

`EngineAdapterCheckForDuplicate` implements the WinBio-facing result:

1. Initialize `Duplicate` to false and the identity to wildcard.
2. Query the WinBio storage adapter by content and obtain its record count.
3. Continue only when at least one database record exists.
4. Check the UMDF enrollment result's first error byte for `0x22`.
5. On `0x22`, fetch the current database record, copy its identity/subfactor to
   the output arguments, and set `Duplicate` to true.

An `OMSConfig.ini` compatibility switch named `TASK17/ID42PATCH` can bypass the
`0x22` comparison, apparently as a product-specific workaround. The normal INF
does not install or enable this file.

`EngineAdapterCommitEnrollment` checks `0x22` again and returns HRESULT
`0x8009801c` instead of adding a new WinBio storage record.

## UMDF device flow

The relevant routine in `OMSFpDriver.dll` is `OMSAutoEnrollCmd` at RVA
`0x57cc`. It constructs exactly the command used by the Linux implementation:

```text
ef 01 ff ff ff ff 01 00 08 31 ID_hi ID_lo 06 00 00 checksum_hi checksum_lo
```

The response parser reads:

```text
frame offset 9   enrollment code/error
frame offset 10  parameter (1 through 6)
frame offset 11  accepted capture count for parameter 3
```

Parameters 1 and 4 update error byte 0; parameters 2 and 5 update error byte 1;
parameter 3 reports progress; parameter 6 is terminal. On error byte 0 equal to
`0x22`, the routine calls `OMSCancel` and finishes immediately. `OMSCancel` at
RVA `0x69bc` sends the confirmed payload `30`.

The handler at RVA `0x5cd4` retains the chosen template ID between captures.
For each WinBio sample it calls `OMSAutoEnrollCmd` again, so command `0x31` is
resent after capture counts 1 through 5. The Linux driver now follows this
cadence.

## Live result and Linux policy

On firmware `acm32_btl_ofilm_pc_000059`, an atomic pure-protocol test first
identified the touched finger as existing slot 0, then immediately enrolled the
same finger with the exact Windows command and resend cadence. The firmware
completed all six captures, returned zero in all three error fields, and created
slot 1; it never returned `0x22`. The test deleted slot 1 and verified that the
original index was restored. This rules out libfprint orchestration and finger
ambiguity as causes of the result.

The Windows package contains firmware/configuration for version `000057`, while
the tested device reports `000059`. The static `0x22` path may therefore be for
another firmware revision, a product-specific configuration, or defensive
compatibility code. No firmware replacement was attempted.

The Linux driver keeps native compatibility: if `0x22` is ever returned at
parameter 1 or 4, it sends `0x30` and reports
`FP_DEVICE_ERROR_DATA_DUPLICATE`. It does not advertise
`FP_DEVICE_FEATURE_DUPLICATES_CHECK`, because this firmware does not reliably
provide that capability.

Reliable duplicate prevention belongs to the enrollment coordinator. fprintd
already checks whether the device supports identify and calls identify before
enroll; a match is reported as `enroll-duplicate` and enrollment is never
started. Matching stays entirely inside the sensor and no image or template
material is extracted. A direct libfprint client that wants the same guarantee
must perform the same identify-before-enroll sequence.

## Confirmed live tests

- Pure protocol, existing finger: identify matched slot 0; direct enrollment
  nevertheless created slot 1 without `0x22`; automatic cleanup restored slot 0.
- fprintd-equivalent policy: identify of the existing finger matched slot 0, so
  enrollment was not started and the template count stayed at one.
- Different left thumb: identify returned no-match, six enrollment stages
  completed into slot 1, and the test deleted slot 1; count returned to one.
- The driver advertises feature mask `0x17e`; native duplicate checking is
  deliberately not advertised.
