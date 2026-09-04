# Related public work

Search date: 2026-08-22.

No public implementation was found for `33a7:2388`, nor for the distinctive
symbols `OMSHandshake`, `OMSBulkWritePacket`, or firmware identifier
`acm32_btl_ofilm`. The same USB ID appears in several Windows driver indexes,
but those pages only redistribute or catalogue the vendor driver.

Useful prior art for the method and eventual architecture:

- `uunicorn/synaWudfBioUsb-sandbox`: a standalone Win64 WinBio imitation plus a
  patched Wine USB/WDF environment. It loads a vendor UMDF2 biometric driver on
  Linux and drives its IOCTLs. This is the best fallback if static analysis
  cannot recover an OMS state transition; it may avoid a full Windows VM.
- `OMGrant/ft9201-libfprint` and its `PORTING.md`: a detailed account of
  recovering USB behavior from a Windows UMDF driver, validating with usbmon,
  and integrating into libfprint. Its Windows matcher-loader is not needed for
  OMS because OMS is Match-on-Chip, but its reverse-engineering workflow is
  directly applicable.
- `HansHoogerwerf/synaptics-00e7-fpdrv`: a from-scratch userspace implementation
  for a secure Match-on-Chip sensor. It separates a bring-up CLI from the
  fprintd/libfprint-facing module, persists host pairing data, and re-handshakes
  after suspend.
- `archeYR/libfprint-CS9711`: a working community libfprint fork for another
  previously unsupported fingerprint reader. It is not protocol-compatible
  with OMS, but is useful as an example of libfprint state-machine integration
  and Arch packaging.
- Experimental ELAN Match-on-Chip work documents an important shared-storage
  rule: begin with list/identify/adopt operations and do not implement device
  template deletion or erase until ownership semantics are known.

The repeated successful pattern is:

1. Build a small libusb bring-up tool with read-only commands.
2. Record and replay deterministic protocol transactions.
3. Recover pairing/encryption separately from enroll/identify commands.
4. Keep destructive template and firmware operations out of the early tool.
5. Once identify/enroll are reliable, move the protocol core into an
   asynchronous `FpDevice` driver and retain the CLI for diagnostics.

## Links

- https://github.com/uunicorn/synaWudfBioUsb-sandbox
- https://github.com/OMGrant/ft9201-libfprint/blob/master/PORTING.md
- https://github.com/HansHoogerwerf/synaptics-00e7-fpdrv
- https://github.com/archeYR/libfprint-CS9711
- https://github.com/jedbillyb/linux-fingerprint-drivers

