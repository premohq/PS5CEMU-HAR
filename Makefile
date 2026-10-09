# SPDX-License-Identifier: GPL-3.0-or-later
# PS5CEMU-HAR build. `make help` lists the targets. Missing dependencies are fetched at their pinned
# revisions (tools/deps.json); nothing that already exists is changed. The build runs on Linux with
# clang-18, lld-18 and the LLVM 18 tools, cmake, ninja, git, python3 and make.

SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
.DEFAULT_GOAL := help
MAKEFLAGS += --no-print-directory

# The release's version, set here only: port/CMakeLists.txt and tools/package.sh (param.json) read it
VERSION := 3.5.0
APP := build/app/PPSA99360
JOBS ?= $(shell nproc)
export JOBS

.PHONY: help deps deps-status radv azahar melonds build check package release clean distclean

help: ## List the targets
	@echo 'PS5CEMU-HAR build - make [target] [VARIABLE=value]'
	@echo
	@awk 'BEGIN { FS = ":.*## " } /^[a-z][a-z-]*:.*## / { printf "  %-12s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo
	@echo 'Variables:'
	@echo '  JOBS=$(JOBS)                parallel compile jobs'
	@echo '  RADV_PATCHES=0006           the driver patches (patches/mesa) RADV is built with: their numbers,'
	@echo '                              all, or empty for none (0006: 120 Hz output after the new launcher)'
	@echo '  RADV_ARCHIVE, RADV_SDK      a RADV built elsewhere, and the SDK fork it was built with'

deps: ## Fetch the pinned inputs and build the libraries Cemu needs for the PS5
	python3 -B tools/deps.py fetch
	bash tools/build-deps.sh

deps-status: ## Show every dependency and whether it matches its pin
	@python3 -B tools/deps.py status

radv: deps ## Build RADV, the Vulkan driver, with PS5_Vulkan's recipe and patches/mesa (RADV_PATCHES)
	bash tools/build-radv.sh

azahar: deps ## Build Azahar's core (the 3DS side) and its PS5 frontend: build/azahar
	bash tools/build-azahar.sh

melonds: deps ## Build melonDS's core (the DS side's) and its PS5 frontend: build/melonds
	bash tools/build-melonds.sh

build: deps azahar melonds $(if $(RADV_ARCHIVE),,radv) ## Build PS5CEMU-HAR (Cemu, Azahar and melonDS) and link it with RADV: build/cemu/ps5cemu.elf
	bash tools/build-cemu.sh

check: deps azahar melonds ## Build and package everything with a stand-in for RADV, to check the build (not an app)
	PS5CEMU_LINK_CHECK=1 bash tools/build-cemu.sh
	bash tools/package.sh --check

package: build ## The app folder: build/app/PPSA99360
	bash tools/package.sh

release: package ## dist/PS5CEMU-HAR-vVERSION.zip (the PPSA99360 folder), SHA256SUMS, and the ELF with its symbols
	@mkdir -p dist
	rm -f dist/PS5CEMU-HAR-v$(VERSION).zip
	cd build/app && python3 -m zipfile -c ../../dist/PS5CEMU-HAR-v$(VERSION).zip PPSA99360
	cd dist && sha256sum PS5CEMU-HAR-v$(VERSION).zip > SHA256SUMS
	@# the unstripped ELF of the eboot, kept to look up this release's crash reports
	@# (tools/symbolize-crash.py dist/ps5cemu-vVERSION.elf boot.log)
	cp build/cemu/ps5cemu.elf dist/ps5cemu-v$(VERSION).elf
	@cat dist/SHA256SUMS

clean: ## Remove build outputs (build/, dist/); dependencies stay
	rm -rf build dist

distclean: clean ## Also remove the fetched dependencies in .deps
	python3 -B tools/deps.py clean
