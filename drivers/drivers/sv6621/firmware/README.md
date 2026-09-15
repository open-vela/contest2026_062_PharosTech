# SeekWave SV6621 (SWT6621-S) firmware inputs

This repository does not distribute SeekWave firmware. The Apache-2.0
license of the driver does not grant a license to the firmware.

## Building with the onboard radio

Obtain firmware authorized by SeekWave or an authorized supplier for your
hardware and intended use. Place these files in this directory before
building an image with CONFIG_KICKPI_K7_WIFI enabled:

| File | Purpose |
| --- | --- |
| SWT6621S_IRAM_SDIO.bin | Instruction RAM image |
| SWT6621S_DRAM_SDIO.bin | Data RAM image |
| SWT6621S_NV_SDIO_ALONE.bin | SDIO operating-mode configuration |
| SWT6621S_SEEKWAVE_R00001.bin | Board RF calibration data |

Bluetooth additionally requires the following file when
CONFIG_KICKPI_K7_BLUETOOTH is enabled:

| File | Purpose |
| --- | --- |
| sv6160lite.nvbin | Bluetooth NV configuration |

NV and RF calibration data must match the target hardware. Do not substitute
unrelated board data. The board embeds these inputs using .incbin; a local
full build without the required inputs fails unless placeholders are created
explicitly for build-only validation as described below.

Permission to obtain a file does not necessarily permit redistribution.
Ensure your authorization covers storage, CI use, and any distribution of
images containing the firmware. Do not commit these files, generated objects,
or firmware-containing images to the public repository.

## Public CI placeholders

Public CI creates exact-size, zero-filled files with the required names before
building. They contain no SeekWave code or board data and exist only to verify
that the complete SV6621 integration compiles with both Make and CMake. The
files remain ignored and are never committed.

At runtime the KICKPI-K7 integration recognizes the complete all-zero WiFi
set before touching the radio hardware. It prints a warning, leaves WiFi
disabled, and continues system startup. A partially zero or otherwise
corrupt firmware set is not treated as a CI placeholder and follows the
normal error path. When Bluetooth is enabled, an all-zero Bluetooth NV
image is recognized separately: Bluetooth is skipped while an otherwise
healthy WiFi bring-up continues.

Public CI may upload the resulting build artifacts. Such artifacts contain
only the zero-filled placeholders: they can exercise unrelated board features
but cannot initialize the onboard SV6621 radio. Replace the authorized WiFi
inputs (and the Bluetooth NV file when Bluetooth is enabled) and rebuild to
enable the radio.

The existing full configuration and both build systems remain available.
From the OpenVela workspace root:

```sh
./build.sh contest2026_062_PharosTech/configs/dev -j4
./build.sh contest2026_062_PharosTech/configs/dev --cmake
```
