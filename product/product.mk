# Nyabula product build variables. Override on the command line or env:
#   make emmc OUT=/tmp/o JOBS=12 LINUX_SRC=/root/openvela/media-amp-20260912/kernel-source

REPO_NAME      ?= contest2026_062_PharosTech
VELA_ROOT      ?= $(abspath ..)
REPO_DIR       ?= $(abspath .)
PRODUCT_CONFIG ?= product
JOBS           ?= 4
PY             ?= python3
PNPM           ?= pnpm

VER            ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)-$(shell date +%Y%m%d)
OUT            ?= $(abspath out/product-$(VER))

# Boot chain inputs
RKBIN          ?= $(abspath tools/k7_pack/rkbin)
NBOOT_DIR      ?= $(abspath boards/rk3576/kickpi-k7/nboot)
EMMC_PKG       ?= nyabula-k7-emmc
PRODUCT_ZIP    ?= nyabula-k7-$(VER).zip

# AMP / Linux inputs (only needed for `make amp`)
LINUX_SRC      ?= /root/openvela/media-amp-20260912/kernel-source
LINUX_CROSS    ?= aarch64-linux-gnu-
AMP_DTB        ?= rk3576-kickpi-k7-nyabula-amp.dtb
BUSYBOX        ?= $(abspath out/busybox-static)

# Data partition inputs
WEB_DIST       ?= app/nyabula_web/apps/nyabula/dist

# Serial port for flash-ota
PORT           ?= COM12
