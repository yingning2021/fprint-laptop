# Safe Windows capture plan

Do not attach the physical sensor to a Windows instance that has installed the official package with its default registry settings.

The eventual capture environment should use USBPcap/Wireshark and an isolated Windows VM. Before USB passthrough:

1. Install/copy the analysis tools while the sensor is not passed through.
2. Stage the driver without starting the OMS service.
3. Set `HKLM\\System\\CurrentControlSet\\Control\\OMS\\EnableUpdate` to `0` offline or before first device attachment.
4. Disable `OMSFpServer` auto-start as a second guard.
5. Snapshot the VM.
6. Start USB capture, then pass through only `33a7:2388`.
7. Capture initialization and a firmware-version query first.
8. Diff later enrollment/verify captures against the known initialization sequence.

The Omarchy Windows VM uses `dockurr/windows` under Docker. Its generated Compose file exposes KVM but does not add this USB device by default; a narrowly scoped `/dev/bus/usb/<bus>/<device>` mapping would be required. USB device numbers can change after reconnect, so the mapping must be revalidated immediately before launch.

