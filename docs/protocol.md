# OMS USB protocol notes

Status: core operations confirmed against firmware `acm32_btl_ofilm_pc_000059`.
All multi-byte constants below are shown as bytes on the USB wire.

## Transport

- Device: `33a7:2388`
- Configuration/interface/alternate setting: `1/0/0`
- OUT endpoint: `0x01` bulk
- IN endpoint: `0x81` bulk
- Transfer size used by the Windows driver: 64 bytes
- Commands are zero-padded to 64 bytes before transmission.

Each logical payload is wrapped by a small transport protocol. A write starts with
`55 53 42 43 00 40 00 00`; a requested read starts with
`55 53 42 43 80 40 00 00`. The final three bytes are the little-endian payload
length, here 64 bytes.

The transaction observed in the driver is:

1. OUT: 8-byte `USBC` write header
2. OUT: 64-byte command payload
3. IN: write acknowledgement (the device returned a 4-byte short packet during
   live probing; the Windows driver provides a 64-byte maximum buffer)
4. wait about 50 ms
5. OUT: 8-byte `USBC` read header
6. IN: 64-byte response payload
7. IN: read acknowledgement (maximum 64 bytes; short packets are valid)

The device sometimes returns an all-zero packet when a response is not ready.
The host must issue another read transaction after a short delay instead of
treating that packet as the command result. Both the standalone transport and
the libfprint driver retry this condition.

Before secure-session setup, commands are sent as plaintext. Once its secure flag is enabled, the driver shifts the payload four bytes and prepends an encrypted-frame header; that path is deliberately not implemented yet.

## Confirmed pre-session commands

### Handshake

Request prefix (12 bytes):

```text
ef 01 ff ff ff ff 01 00 03 35 00 39
```

Expected response prefix (12 bytes):

```text
ef 01 ff ff ff ff 07 00 03 00 00 0a
```

### Read firmware version

Request prefix (13 bytes):

```text
ef 01 ff ff ff ff 01 00 04 af 10 00 c4
```

The response begins with:

```text
ef 01 ff ff ff ff 07
```

The Windows driver scans response offsets 10 through 37 for `acm32`, then copies a 26-byte version field.

## Inner frame format

```text
ef 01 | ff ff ff ff | direction | length_be16 | payload | checksum_be16
```

- `direction` is `01` for a host request and `07` for a device response.
- `length` is `payload length + 2` (the two checksum bytes).
- `checksum` is the 16-bit sum of every byte from `direction` through the end
  of the payload.
- Multiple complete frames may be packed into one transport response; trailing
  zero padding is not part of a frame.

### Identify existing templates

Start-identify frame (17 bytes):

```text
ef 01 ff ff ff ff 01 00 08 32 05 ff ff 00 00 02 3e
```

The response payload exposes `code`, `parameter`, and a big-endian template ID.
`parameter == 05` is the completion condition used by the Windows driver. An
error/status frame uses `parameter == f5`, followed by an error-count byte and
error detail bytes. The first response is read with a 64-byte budget; continued
finger waiting is polled with 32-byte reads. The probe sends the confirmed
cancel command on timeout.

The Windows driver's `OMSAutoIdentify` path constructs this same 17-byte
identify frame. It pauses host reads while the device is in D3 and continues
reading the pending identify response after resume. There is no separate
auto-identify wire command in this path. The libfprint driver mirrors that
behavior by keeping command `0x32` active across its suspend/resume callbacks;
libfprint enables USB remote wake while that operation is suspended.

### Other confirmed commands

Get chip serial number (13-byte prefix):

```text
ef 01 ff ff ff ff 01 00 04 34 00 00 39
```

Get valid template count (12-byte prefix):

```text
ef 01 ff ff ff ff 01 00 03 1d 00 21
```

The count is returned in response bytes 10 and 11.

Read the 32-byte template index table (13-byte prefix):

```text
ef 01 ff ff ff ff 01 00 04 1f 00 00 24
```

The driver copies 32 bytes beginning at response offset 10. This is a bitset,
not 32 byte-sized slots: bit `N % 8` in byte `N / 8` represents template ID
`N`, for 256 possible IDs. On the test machine, `01 00 ...` means only ID 0 is
occupied. The Windows engine checks only bits 0 through 29 when deciding that
storage is full, so the Linux driver also allocates new templates only in the
30-slot range. It still lists and clears all 256 addressable IDs so unexpected
out-of-range records remain recoverable.

### Enroll

Start enrollment:

```text
31 | template_id_be16 | 06 00 00
```

For template ID 1 the complete inner request is:

```text
ef 01 ff ff ff ff 01 00 08 31 00 01 06 00 00 00 41
```

The template ID is big-endian. Sending `01 00` for ID 1 actually requests ID
256 and produces initial status `0b 00 00`; the correct `00 01` request is
accepted with `00 00 00`.

The Windows driver stops device idle, waits 500 ms, reads the index bitset,
chooses the first clear bit, and sends this command. Enrollment events use
parameters 1 through 6. Parameter 3 contains the accepted capture count;
parameter 6 completes enrollment. After every accepted capture below count 6,
the Windows driver sends the same `0x31` command again. Six captures were
accepted in live testing.

For parameters 1 and 4, the first payload byte is the primary enrollment error.
The official driver treats `0x22` as a duplicate template, immediately sends
cancel command `0x30`, and exposes the result through WinBio duplicate checking.
Firmware `000059` did not produce `0x22` reliably in repeated same-finger live
tests, even when a pure-protocol identify immediately proved that the same
finger matched an occupied slot and enrollment used the Windows resend cadence.
The libfprint driver therefore retains native `0x22` handling but does not
advertise native duplicate-check support. fprintd provides deterministic
duplicate prevention by using the driver's on-chip identify operation before
starting enrollment. Direct libfprint clients must use the same orchestration.
No fingerprint images or templates leave the sensor.

### Delete one template

```text
0c | template_id_be16 | 00 01
```

Deletion of a newly created ID 1 was confirmed through both the standalone
protocol and the libfprint API.

### Clear storage

libfprint clear-storage support reads the 32-byte index bitset and sends the
confirmed single-template delete command for every occupied ID. Live testing
started with IDs 0 and 1 occupied, completed successfully, then survived a
close/reopen cycle with template count 0 and all 256 index bits clear.

Sleep (12-byte prefix):

```text
ef 01 ff ff ff ff 01 00 03 33 00 37
```

The cancel payload is `30`. Firmware-log clear, firmware boot mode, firmware
erase, and firmware update commands are intentionally absent from executable
code.

## Windows update behavior

`OMSFpServer.exe` implements the update path. Static control-flow analysis shows:

1. Read `HKLM\\System\\CurrentControlSet\\Control\\OMS\\EnableUpdate`.
2. Return without updating if the value is zero.
3. Read `config.ini`, `FW_VERSION`, and `ACM32FP.bin`.
4. Query the current version with a WBDI control-unit operation.
5. Treat the firmware as current only when the returned version string contains the configured version string.
6. Otherwise enter its bootloader/download/retry path.

The official INF sets `EnableUpdate=1`, so a normal Windows installation is not capture-safe until this registry default is overridden before the service or device starts.

## System suspend observations

With an identify operation active, the libfprint suspend/resume callbacks keep
command `0x32` pending and the match result is delivered immediately after
resume. In a real `s2idle` test, a light touch did not wake the machine within a
10-second observation window. Pressing the combined power/fingerprint button
did wake it; the fingerprint USB controller and ACPI power-button wake sources
both recorded new events, and Omarchy unlocked about 160 ms after resume. This
matches a OneKey-style power-button wake followed by delivery of the pending
on-chip match, rather than proven touch-only USB remote wake.

## Still unknown

- Meanings of device-specific error codes other than duplicate code `0x22`
- Secure-session key derivation and block cipher details; the confirmed normal
  list/enroll/identify/delete paths do not use it
- Whether a different firmware or platform can assert touch-only USB remote
  wake without pressing the integrated power button
- Firmware update protocol (deliberately out of scope)
