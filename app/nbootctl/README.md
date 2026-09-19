# nbootctl

`nbootctl` is the openvela-side client for the KICKPI-K7 N-Boot
contract. It reads the handoff published immediately before N-Boot enters
NuttX, or stores a one-shot request in bootctrl before a system reset.

This utility targets the current K7 layout: partition 3 is bootctrl,
partitions 4/5 are NuttX A/B, and partitions 6/7 are AMP A/B. N-Boot occupies
the fixed 4 MiB slot starting at sector 16384. Arbitrary layouts are not
supported. AMP operations do not coordinate with a running Linux owner.

```text
nbootctl status
nbootctl verify nuttx a
nbootctl set-active nuttx b
nbootctl mark-successful nuttx b
nbootctl stage nuttx /tmp/nuttx.bin
nbootctl clone nuttx a b
nbootctl update-nboot /tmp/nboot.img
nbootctl reboot console
nbootctl reboot fastboot
nbootctl reboot nuttx-a
nbootctl reboot nuttx-b
```

`status` validates the handoff magic and version and reads the header twice so
a partially updated handoff is rejected. Slot requests affect one boot only;
they do not change the persistent active slot. Reboot requests are read back
before the system reset. Requests use the first four padding bytes (offset
236) of the existing CRC-protected bootctrl record. N-Boot consumes and
clears a request through the redundant-copy update before acting on it. This
requires the N-Boot version supporting persistent one-shot requests; PMU1
OS_REG12 did not survive the tested loader reset chain reliably.

`verify`, `set-active`, `mark-successful`, `stage`, and `clone` operate on
either the `nuttx` or `amp` domain. `stage` writes the inactive slot, verifies
its SHA-256 from media, records the new metadata, and activates it. `clone`
copies one verified slot to the other without activating it. Boot-control
mutations update the older redundant copy first, verify it, and then update the
other copy. `update-nboot` replaces the N-Boot FIT on the current medium and
verifies it from media; reboot is left to the caller.

`nbootctl_bootctrl.c` is also a small library. `nbootctl_handoff_read()` and
`nbootctl_bootctrl_snapshot()` return the handoff and both domains as a struct
and print nothing, so other code (Nyabula Core's `update.status`) can report
slot state without parsing this tool's output. `tries_remaining` is not part of
that struct: N-Boot selects by priority alone. The file must be linked once per
image: a build that enables this command reuses its object, and only a build
without it compiles the source into the caller.
