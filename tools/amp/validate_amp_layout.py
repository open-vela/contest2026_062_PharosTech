#!/usr/bin/env python3
"""Validate the fixed KICKPI-K7 AMP memory and transport contract."""

from __future__ import annotations

import argparse
import re
import subprocess
import struct
from pathlib import Path


EXPECTED = {
    "vring": (0x47800000, 0x200000),
    "dma": (0x47A00000, 0x200000),
    "shmem": (0x47C00000, 0x400000),
    "openvela": (0x4A400000, 0x1000000),
}

FORBIDDEN = {
    "bl31": (0x40000000, 0x200000),
    "optee": (0x48400000, 0x1000000),
    "standalone_dma_heap": (0x49400000, 0x1000000),
}


def require(pattern: str, text: str, source: Path) -> re.Match[str]:
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    if match is None:
        raise ValueError(f"{source}: missing pattern: {pattern}")
    return match


def parse_dtsi(path: Path) -> dict[str, tuple[int, int]]:
    text = path.read_text(encoding="utf-8")
    labels = {
        "vring": "rpmsg_reserved",
        "dma": "rpmsg_dma_reserved",
        "shmem": "amp_shmem_reserved",
        "openvela": "openvela_reserved",
    }
    result: dict[str, tuple[int, int]] = {}
    for name, label in labels.items():
        match = require(
            rf"{label}:.*?\{{.*?reg\s*=\s*<0x0\s+(0x[0-9a-fA-F]+)\s+"
            rf"0x0\s+(0x[0-9a-fA-F]+)>;",
            text,
            path,
        )
        result[name] = (int(match.group(1), 16), int(match.group(2), 16))

    require(r"mboxes\s*=\s*<&mailbox0\s+0\s+&mailbox3\s+0>;", text, path)
    require(r"rockchip,link-id\s*=\s*<0x03>;", text, path)
    require(r"GIC_AMP_IRQ_CFG_ROUTE\(174,", text, path)

    # The shared region must stay no-map: that is what keeps it out of the
    # linear mapping and the page allocator.  Dropping it would let the kernel
    # hand the pages to something else while the control domain is still
    # writing audio into them.  The region is referenced by a separate node
    # rather than carrying a compatible itself, because the kernel only
    # instantiates reserved-memory children on its own allow-list.
    require(r'amp_shmem_reserved:\s*amp-shmem@47c00000\s*\{[^}]*'
            r'\bno-map\s*;', text, path)
    require(r'compatible\s*=\s*"nyabula,amp-shmem";[^}]*'
            r'memory-region\s*=\s*<&amp_shmem_reserved>;', text, path)
    for cpu in range(4):
        require(rf'/delete-node/\s*&cpu_l{cpu}\s*;', text, path)
        require(rf'&cpu_b{cpu}\s*\{{\s*status\s*=\s*"okay";', text, path)
    return result


def parse_defconfig(path: Path) -> tuple[int, int]:
    text = path.read_text(encoding="utf-8")
    start = int(require(r"^CONFIG_RAM_START=(0x[0-9a-fA-F]+)$", text, path).group(1), 16)
    size = int(require(r"^CONFIG_RAM_SIZE=([0-9]+)$", text, path).group(1), 10)
    require(r"^CONFIG_OPENAMP_CACHE=y$", text, path)
    require(r"^CONFIG_SMP=y$", text, path)
    # savedefconfig omits SMP_NCPUS=4 because four is the Kconfig default.
    # The expanded config gate below always requires the resulting value.
    ncpus = re.search(r"^CONFIG_SMP_NCPUS=(.*)$", text, re.MULTILINE)
    if ncpus is not None and ncpus.group(1) != "4":
        raise ValueError(f"{path}: AMP requires four NuttX CPUs")
    return start, size


def parse_rptun(path: Path) -> tuple[int, int, int, int]:
    text = path.read_text(encoding="utf-8")
    values = []
    for macro in (
        "RK3576_RPMSG_VRING0_DA",
        "RK3576_RPMSG_VRING_SIZE",
        "RK3576_RPMSG_BUFFER_DA",
        "RK3576_RPMSG_BUFFER_LEN",
    ):
        match = require(rf"^#define\s+{macro}\s+(0x[0-9a-fA-F]+)", text, path)
        values.append(int(match.group(1), 16))

    require(
        r"#define\s+RK3576_RPMSG_TX_MBOX\s+0.*?"
        r"#define\s+RK3576_RPMSG_RX_MBOX\s+3.*?"
        r"notifyid\s*==\s*0.*?RK3576_RPMSG_TX_MBOX.*?"
        r"notifyid\s*==\s*1.*?RK3576_RPMSG_RX_MBOX",
        text,
        path,
    )
    return tuple(values)  # type: ignore[return-value]


def overlaps(left: tuple[int, int], right: tuple[int, int]) -> bool:
    return left[0] < right[0] + right[1] and right[0] < left[0] + left[1]


def validate_config(path: Path) -> None:
    """Check expanded Kconfig output, not just the requested defconfig."""
    values = dict(re.findall(r"^(CONFIG_\w+)=(.*)$", path.read_text(), re.MULTILINE))
    expected = {
        "CONFIG_SMP": "y",
        "CONFIG_SMP_NCPUS": "4",
        "CONFIG_NCPUS": "4",
        "CONFIG_ARCH_HAVE_MULTICPU": "y",
        "CONFIG_ARCH_HAVE_IRQTRIGGER": "y",
        "CONFIG_MM_REGIONS": "1",
        "CONFIG_ARM64_GICV2_PREINITIALIZED": "y",
        "CONFIG_ARM64_GICV2_STATIC_SPI": "y",
        "CONFIG_UART0_SERIAL_CONSOLE": "y",
    }
    for key, value in expected.items():
        if values.get(key) != value:
            raise ValueError(f"{path}: {key} must be {value}, got {values.get(key)!r}")
    for key in ("CONFIG_UP", "CONFIG_RK3576_DMA_ALLOC", "CONFIG_NO_SERIAL_CONSOLE"):
        if values.get(key) == "y":
            raise ValueError(f"{path}: {key} is incompatible with the AMP profile")
    for key, expected_value in (("CONFIG_RAM_START", EXPECTED["openvela"][0]),
                                ("CONFIG_RAM_SIZE", EXPECTED["openvela"][1]),
                                ("CONFIG_SMP_DEFAULT_CPUSET", 0xF)):
        if int(values.get(key, "-1"), 0) != expected_value:
            raise ValueError(f"{path}: unexpected {key}")


def validate_dtb(path: Path) -> None:
    """Use libfdt's tool to inspect compiled CPU nodes, including omitted status."""
    def get(*args: str) -> str:
        return subprocess.check_output(
            ["fdtget", *args], text=True, stderr=subprocess.PIPE
        ).strip()

    address_cells = int(get("-t", "u", str(path), "/cpus", "#address-cells"))
    if address_cells not in (1, 2):
        raise ValueError("unsupported CPU address cells")
    cpus = {}
    for node in get("-l", str(path), "/cpus").splitlines():
        node_path = f"/cpus/{node}"
        properties = get("-p", str(path), node_path).splitlines()
        if "device_type" not in properties:
            if node == "cpu" or node.startswith("cpu@"):
                raise ValueError(f"{node_path}: CPU device_type is required")
            continue
        if get("-t", "s", str(path), node_path, "device_type") != "cpu":
            if node == "cpu" or node.startswith("cpu@"):
                raise ValueError(f"{node_path}: invalid CPU device_type")
            continue
        if (get("-t", "s", str(path), node_path, "enable-method") != "psci" or
                "arm,cortex-a72" not in get("-t", "s", str(path), node_path, "compatible").split()):
            raise ValueError(f"{node_path}: expected an A72 with PSCI")
        cells = get("-t", "x", str(path), node_path, "reg").split()
        if len(cells) != address_cells:
            raise ValueError(f"{node_path}: invalid CPU reg")
        affinity = 0
        for cell in cells:
            affinity = (affinity << 32) | int(cell, 16)
        if affinity in cpus:
            raise ValueError(f"duplicate CPU affinity {affinity:#x}")
        cpus[affinity] = (get("-t", "s", str(path), node_path, "status")
                          if "status" in properties else "okay")
    expected = {0x100 + cpu: "okay" for cpu in range(4)}
    cpus = {cpu: "okay" if status == "ok" else status
            for cpu, status in cpus.items()}
    if cpus != expected:
        raise ValueError(f"{path}: CPU topology mismatch: {cpus!r}")


def validate_image(path: Path, limit: int, linux: bool = False) -> None:
    with path.open("rb") as image:
        header = image.read(64)
    if len(header) != 64 or header[56:60] != b"ARM\x64":
        raise ValueError(f"{path}: invalid ARM64 Image header")
    offset, runtime_size, flags = struct.unpack_from("<QQQ", header, 8)
    if runtime_size < path.stat().st_size or runtime_size > limit or flags & 1:
        raise ValueError(f"{path}: invalid runtime image extent or byte order")
    if linux and (offset > 0x42000000 or (0x42000000 - offset) & 0x1FFFFF):
        raise ValueError(f"{path}: Linux text offset does not match the load address")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, help="expanded NuttX .config")
    parser.add_argument("--dtb", type=Path, help="compiled Linux DTB (requires fdtget)")
    parser.add_argument("--linux-image", type=Path, help="Linux ARM64 Image")
    parser.add_argument("--nuttx-image", type=Path, help="NuttX ARM64 raw image")
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="team repository root",
    )
    args = parser.parse_args()
    repo = args.repo.resolve()

    dtsi_path = repo / "tools/amp/linux/rk3576-kickpi-k7-amp.dtsi"
    defconfig_path = repo / "boards/rk3576/kickpi-k7/configs/amp/defconfig"
    rptun_path = repo / "chips/rk3576/rk3576_rptun.c"

    layout = parse_dtsi(dtsi_path)
    if layout != EXPECTED:
        raise ValueError(f"DTS layout mismatch: {layout!r}")

    if parse_defconfig(defconfig_path) != EXPECTED["openvela"]:
        raise ValueError("openvela defconfig does not match its DTS carveout")

    vring, vring_size, dma, dma_size = parse_rptun(rptun_path)
    if vring != EXPECTED["vring"][0] or vring_size != 0x8000:
        raise ValueError("rptun vring geometry does not match the DTS contract")
    if (dma, dma_size) != EXPECTED["dma"]:
        raise ValueError("rptun buffer pool does not match the DTS contract")

    names = list(EXPECTED)
    for index, name in enumerate(names):
        for other in names[index + 1 :]:
            if overlaps(EXPECTED[name], EXPECTED[other]):
                raise ValueError(f"overlap: {name} and {other}")

    for name, region in EXPECTED.items():
        for reserved_name, reserved in FORBIDDEN.items():
            if overlaps(region, reserved):
                raise ValueError(f"overlap: {name} and {reserved_name}")

    if args.config:
        validate_config(args.config)
    if args.dtb:
        validate_dtb(args.dtb)
    if args.linux_image:
        validate_image(args.linux_image, 0x05000000, linux=True)
    if args.nuttx_image:
        validate_image(args.nuttx_image, EXPECTED["openvela"][1])
    print("AMP static layout OK (not a boot/readiness validation)")
    for name, (start, size) in EXPECTED.items():
        print(f"  {name:9s} 0x{start:08x}-0x{start + size:08x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
