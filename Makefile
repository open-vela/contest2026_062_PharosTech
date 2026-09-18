# SPDX-License-Identifier: Apache-2.0
#
# Nyabula KICKPI-K7 product build — single entry point.
#
# Run from this repository root inside an openvela workspace
# (i.e. ../build.sh exists). Every target only calls the scripts that
# already live under tools/; this file is the dependency graph.
#
#   make help              list targets
#   make nuttx             main-domain firmware (configs/product)
#   make amp               AMP FIT: Linux + initramfs + nyampd + A53 openvela
#   make nboot             N-Boot A/B images from the pinned release
#   make emmc              RKDevTool eMMC flash package (final deliverable)
#   make sd                whole-disk SD image
#   make product           emmc + sd + SHA256SUMS
#
# Layer-by-layer overrides live in product/product.mk.

include product/product.mk

.DEFAULT_GOAL := help
.PHONY: help all setup product nuttx amp-nuttx linux nyampd initramfs amp nboot rkbin \
        data web fonts sd emmc flash-ota flash-amp check clean distclean

help: ## Show this help
	@grep -hE '^[a-zA-Z_-]+:.*##' $(MAKEFILE_LIST) | awk 'BEGIN{FS=":.*##"}{printf "  %-12s %s\n",$$1,$$2}'

all: product

# ---------------------------------------------------------------- main domain
fonts: ## Generate Eye Engine glyph tables (Noto fallback)
	$(PY) app/nyabula/tools/generate_fonts.py --download-fallback

nuttx: fonts $(OUT) ## Build main-domain openvela (configs/product)
	cd $(VELA_ROOT) && ./build.sh $(REPO_NAME)/configs/$(PRODUCT_CONFIG) -j$(JOBS)
	cp $(VELA_ROOT)/nuttx/nuttx.bin $(OUT)/nuttx.bin
	cp $(VELA_ROOT)/nuttx/nuttx     $(OUT)/nuttx.elf
	cp $(VELA_ROOT)/nuttx/.config   $(OUT)/nuttx.config
	@ls -l $(OUT)/nuttx.bin

# ---------------------------------------------------------------- AMP domain
amp-nuttx: $(OUT) ## Build A53 openvela for the AMP domain (configs/amp)
	cd $(VELA_ROOT) && ./build.sh $(REPO_NAME)/boards/rk3576/kickpi-k7/configs/amp -j$(JOBS)
	cp $(VELA_ROOT)/nuttx/nuttx.bin $(OUT)/amp-nuttx.bin
	cp $(VELA_ROOT)/nuttx/.config   $(OUT)/amp-nuttx.config

linux: $(OUT) ## Build Linux Image + K7 AMP DTB (needs LINUX_SRC)
	@test -d "$(LINUX_SRC)" || { echo "LINUX_SRC=$(LINUX_SRC) not found" >&2; exit 1; }
	JOBS=$(JOBS) CROSS_COMPILE=$(LINUX_CROSS) tools/amp/linux/build_kernel.sh $(LINUX_SRC) $(OUT)/linux

nyampd: $(OUT) ## Cross-build the Linux compute daemon (aarch64)
	JOBS=$(JOBS) CC=$(LINUX_CROSS)gcc CXX=$(LINUX_CROSS)g++ tools/amp/nyampd/build_arm64.sh $(OUT)/nyampd

initramfs: nyampd ## Minimal busybox initramfs with nyampd
	@test -f "$(BUSYBOX)" || { echo "BUSYBOX=$(BUSYBOX) missing; run tools/amp/linux/fetch_busybox_static.sh" >&2; exit 1; }
	tools/amp/linux/build_minimal_initramfs.sh $(BUSYBOX) $(OUT)/amp-initramfs.cpio.gz $(OUT)/nyampd/nyampd

amp: amp-nuttx linux initramfs ## Assemble the AMP FIT (amp.itb)
	tools/amp/nboot/build_amp_fit.sh \
	    $(OUT)/linux/arch/arm64/boot/Image \
	    $(OUT)/linux/arch/arm64/boot/dts/rockchip/$(AMP_DTB) \
	    $(OUT)/amp-initramfs.cpio.gz \
	    $(OUT)/amp-nuttx.bin $(OUT)/amp-nuttx.config \
	    $(OUT)/amp.itb

# ---------------------------------------------------------------- boot chain
rkbin: ## Fetch pinned Rockchip boot blobs
	@test -d "$(RKBIN)/bin" || tools/k7_pack/fetch_rkbin.sh $(RKBIN)

nboot: nuttx rkbin ## Build N-Boot A/B images (uses pinned boards/.../nboot release)
	tools/k7_pack/build_nboot_ab.sh $(OUT)/nuttx.bin $(NBOOT_DIR) $(RKBIN) $(OUT)/nboot emmc

# ---------------------------------------------------------------- data partition
web: ## Build the browser control panel
	cd app/nyabula_web && $(PNPM) install --frozen-lockfile && $(PNPM) build

data: $(OUT) ## Initial /data image (config template + web dist + build.json)
	MODELS_DIR=$(MODELS_DIR) tools/k7_pack/build_data_img.sh product/data $(WEB_DIST) $(VER) $(OUT)/data.img $(DATA_MIB)

# ---------------------------------------------------------------- disk images
sd: nuttx rkbin ## Whole-disk SD image
	tools/k7_pack/build_sd.sh $(OUT)/nuttx.bin $(NBOOT_DIR) $(RKBIN) $(OUT)/sd
	@ls -l $(OUT)/sd/*.img

emmc: nuttx rkbin ## RKDevTool eMMC package (Loader + parameter + partition images)
	DATA_IMG=$(OUT)/data.img AMP_ITB=$(OUT)/amp.itb tools/k7_pack/build_emmc.sh $(OUT)/nuttx.bin $(NBOOT_DIR) $(RKBIN) $(OUT)/emmc
	cd $(OUT)/emmc/$(EMMC_PKG) && sha256sum package-file README.txt Image/* > SHA256SUMS

product: emmc sd ## Final aggregate: emmc package + sd image, zipped with version
	cd $(OUT) && rm -f $(PRODUCT_ZIP) && zip -qr $(PRODUCT_ZIP) emmc/$(EMMC_PKG) sd/*.img nuttx.elf nuttx.config
	cd $(OUT) && sha256sum $(PRODUCT_ZIP) | tee $(PRODUCT_ZIP).sha256

# ---------------------------------------------------------------- board ops
flash-ota: ## Hot-update main firmware over serial (PORT=COMx)
	$(PY) tools/k7_ota/k7_ota.py --port $(PORT) --img $(OUT)/nboot/nuttx-fit.img

flash-amp: ## Stage amp.itb into the amp_b slot via fastboot and activate
	fastboot stage $(OUT)/amp.itb && fastboot oem board:flash:amp_b && fastboot oem board:activate:amp_b

# ---------------------------------------------------------------- gates
setup: ## Wire this repo into the openvela workspace (manifest <linkfile> equivalents)
	cd $(VELA_ROOT) && $(REPO_DIR)/tools/setup_workspace.sh $(REPO_DIR)

check: ## Host-side gates (layout, bootctrl, protocol, format)
	$(PY) tools/amp/validate_amp_layout.py
	$(PY) -m unittest discover -s tools/amp -p 'test_*.py'
	$(PY) tools/k7_abpack/test_bootctrl.py
	bash tools/format.sh --check

$(OUT):
	mkdir -p $(OUT)

clean: ## Remove product outputs
	rm -rf $(OUT)

distclean: clean ## Also distclean the openvela tree
	cd $(VELA_ROOT) && ./build.sh $(REPO_NAME)/configs/$(PRODUCT_CONFIG) distclean
