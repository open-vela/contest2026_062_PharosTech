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

The handoff also says which bootctrl domain the running image belongs to
(header bits 3:2: 0 = a NuttX slot, 1 = the control domain of an AMP slot) and
`status` prints it as `domain=`. Under AMP `slot=` is the AMP slot and no NuttX
slot is running. An AMP image that N-Boot started from RAM reports
`slot=none reason=ram`: it has a medium, so bootctrl can be found, and no slot
to protect. The first valid handoff is kept in memory for the life of the
image, so later readers do not depend on registers the other AMP domain can
reach. This needs an N-Boot that publishes the handoff from `bootamp`; with an
older one the AMP control domain still reports `no valid N-Boot handoff`.

`verify`, `set-active`, `mark-successful`, `stage`, and `clone` operate on
either the `nuttx` or `amp` domain. `stage` never writes the slot the running
image came from: it writes the other slot of that domain, and for the domain
nothing runs from, the slot that is not active. It verifies
its SHA-256 from media, records the new metadata, and activates it. `clone`
copies one verified slot to the other without activating it. Boot-control
mutations update the older redundant copy first, verify it, and then update the
other copy. `update-nboot` replaces the N-Boot FIT on the current medium and
verifies it from media; reboot is left to the caller.

## Raw writes

```text
nbootctl digest /data/tmp/ota.bin
nbootctl verify-part /data/tmp/ota.bin SHA256
nbootctl write-part trust /data/tmp/trust.img SHA256
nbootctl write-raw 64 704 /data/tmp/miniloader.bin SHA256
nbootctl write-gpt /data/tmp/gpt.bin SHA256
nbootctl check-raw 64 704 SHA256
nbootctl format config
```

`write-part` takes one of `uboot`, `trust`, `bootctrl`, `nuttx_a`, `nuttx_b`,
`amp_a`, `amp_b`, `config`, `data` and writes through that partition's own
device node, so the block layer bounds the write. `write-raw` exists for the
MiniLoader at sector 64, which no partition covers; its sector count is a
mandatory bound. `write-gpt` writes at most 34 sectors at LBA 0 and does not
rewrite the backup table.

Every write checks two digests: the file against the SHA-256 given on the
command line before the medium is opened, and the medium read back against
the file afterwards. The read-back hashes the payload bytes only, not the zero
padding of the last sector. There is no form without a digest.

These are raw writes. They do not update bootctrl, so a slot written with
`write-part` keeps the recorded size and digest of the image it replaced and
N-Boot will refuse it and clear its priority: use `stage` for a slot that is
meant to boot. They do not look at mount points, do not protect the slot that
is running, and `write-part uboot` / `update-nboot` are not power-fail safe
(one region, updated in place). A caller that exposes them to a user is
responsible for those refusals; Nyabula Core's `update.apply` is one.

`format` creates a FAT filesystem on a named partition with mkfatfs and is only
built with `CONFIG_FSUTILS_MKFATFS`. The same file holds the strong definition
of `kickpi_k7_storage_format_hook()`, which the board's storage driver calls
for a `config` partition whose first sector is all zeroes.

The medium is reached with `open_blockdriver()`, as the bootctrl code does,
not with `open()` on the device node: that would need the block-to-character
proxy (`CONFIG_BCH`).

## Library use

`nbootctl_bootctrl.c` is also a small library. `nbootctl_handoff_read()` and
`nbootctl_bootctrl_snapshot()` return the handoff and both domains as a struct
and print nothing, so other code (Nyabula Core's `update.status`) can report
slot state without parsing this tool's output. `tries_remaining` is not part of
that struct: N-Boot selects by priority alone. `nbootctl_part.c` is used the
same way for raw partition writes. Both files must be linked once per image: a
build that enables this command reuses their objects, and only a build without
it compiles the sources into the caller.
