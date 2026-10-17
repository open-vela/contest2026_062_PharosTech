#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Turn the minimal AMP Linux device tree into the one the product needs.

The minimal AMP profile hands openvela a serial port and a mailbox.  The
product image drives most of the board, so the Linux side has to let go of
it:

  * memory  - two more no-map regions: openvela's DMA heap (0x49400000,
              16 MiB) and its second heap region (0x70000000, 256 MiB).
              N-Boot only checks the four regions of the base contract and
              accepts extra ones.
  * nodes   - every controller openvela drives is disabled here.  Leaving
              one "okay" is not harmless even with no driver built: the OF
              core still applies its assigned-clocks (the eMMC card clock
              was re-rated behind openvela's back and the card stopped
              answering about ten seconds later), and built-in drivers
              (thermal, watchdog) bind and reset the block.
  * domains - the power domains holding those controllers are marked
              rockchip,always-on.  With no Linux consumer a domain is one
              queued work away from off; touching SAI1 in a powered-down
              PD_AUDIO is a bus error on the openvela side.
  * clocks  - the rockchip-amp node holds the clocks that Linux would
              otherwise gate.  clk_ignore_unused does not cover the
              vendor's own 28 s "unprotect" work, which drops PWM2 (the
              LCD backlight).

It works on a compiled tree (dtb -> dts -> edit -> dtb) so it needs dtc and
the kernel's clock id header, not a kernel build.

usage: amp_product_dtb.py BASE.dtb OUT.dtb --cru-header rockchip,rk3576-cru.h
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RESERVED = (
    ("openvela-dma@49400000", 0x49400000, 0x01000000),
    ("openvela-heap@70000000", 0x70000000, 0x10000000),
)

# Controllers the openvela product image owns.
DISABLED = (
    "mmc@2a330000",       # eMMC (SDHCI)
    "mmc@2a310000",       # SD slot, shares PD_SDGMAC with the Wi-Fi SDIO
    "sai@2a610000",       # SAI1 -> ES8388
    "tsadc@2ae70000",
    "watchdog@2ace0000",
    "adc@2ae00000",       # SARADC
)

# RK3576_PD_NVM, PD_SDGMAC, PD_USB, PD_AUDIO
ALWAYS_ON_DOMAINS = (5, 6, 7, 10)

AMP_CLOCKS = (
    "PCLK_MAILBOX0",
    "CCLK_SRC_EMMC", "HCLK_EMMC", "ACLK_EMMC", "BCLK_EMMC", "TCLK_EMMC",
    "ACLK_NVM_ROOT", "HCLK_NVM_ROOT",
    "CLK_PWM2", "PCLK_PWM2",
)


def node_span(text, header):
    """Return (start, index of the closing brace) of the node at header."""
    start = text.index(header)
    depth = 0
    index = text.index("{", start)
    while True:
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return start, index
        index += 1


def set_status(text, header, value):
    start, end = node_span(text, header)
    lines = text[start:end].split("\n")
    target = None
    indent = None
    for number, line in enumerate(lines[1:], 1):
        match = re.match(r'^(\s*)status = "[a-z]+";', line)
        if match and (indent is None or len(match.group(1)) < indent):
            indent = len(match.group(1))
            target = number
    if target is None:
        lines.insert(1, '\t\tstatus = "%s";' % value)
    else:
        lines[target] = re.sub(r'"[a-z]+"', '"%s"' % value, lines[target])
    return text[:start] + "\n".join(lines) + text[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("base")
    parser.add_argument("output")
    parser.add_argument("--cru-header", required=True)
    args = parser.parse_args()

    ids = dict(re.findall(r"#define\s+(\w+)\s+(\d+)",
                          Path(args.cru_header).read_text()))
    missing = [name for name in AMP_CLOCKS if name not in ids]
    if missing:
        sys.exit("clock ids not found in %s: %s" % (args.cru_header, missing))

    text = subprocess.check_output(
        ["dtc", "-I", "dtb", "-O", "dts", args.base],
        stderr=subprocess.DEVNULL, text=True)

    # Reserved memory, after the openvela carve-out the base tree must have.
    _, end = node_span(text, "openvela@4a400000 {")
    end = text.index(";", end) + 1
    extra = "".join(
        "\n\n\t\t%s {\n\t\t\treg = <0x00 0x%x 0x00 0x%x>;\n\t\t\tno-map;\n\t\t};"
        % region for region in RESERVED if region[0] not in text)
    text = text[:end] + extra + text[end:]

    for header in DISABLED:
        if header + " {" not in text:
            sys.exit("node %s is not in the base tree" % header)
        text = set_status(text, header + " {", "disabled")

    for domain in ALWAYS_ON_DOMAINS:
        header = "power-domain@%x {" % domain
        if header not in text:
            header = "power-domain@%d {" % domain
        start, end = node_span(text, header)
        body = text[start:end]
        if "rockchip,always-on;" not in body:
            brace = body.index("{") + 1
            body = body[:brace] + "\n\t\t\t\trockchip,always-on;" + body[brace:]
            text = text[:start] + body + text[end:]

    start, end = node_span(text, "rockchip-amp {")
    body = text[start:end]
    cru = re.search(r"clocks = <(0x[0-9a-f]+) ", body).group(1)
    power = re.search(r"power-domains = <(0x[0-9a-f]+) 0x05>", text).group(1)
    clocks = " ".join("%s 0x%x" % (cru, int(ids[name])) for name in AMP_CLOCKS)
    domains = " ".join("%s 0x%02x" % (power, d) for d in ALWAYS_ON_DOMAINS)
    body = re.sub(r"\n\s*power-domains = <[^>]*>;", "", body)
    body = re.sub(r"clocks = <[^>]*>;",
                  "clocks = <%s>;\n\t\tpower-domains = <%s>;" % (clocks, domains),
                  body, count=1)
    text = text[:start] + body + text[end:]

    with tempfile.NamedTemporaryFile("w", suffix=".dts", delete=False) as handle:
        handle.write(text)
    subprocess.check_call(["dtc", "-I", "dts", "-O", "dtb", "-o", args.output,
                           handle.name], stderr=subprocess.DEVNULL)
    Path(handle.name).unlink()
    print("wrote %s" % args.output)


if __name__ == "__main__":
    main()
