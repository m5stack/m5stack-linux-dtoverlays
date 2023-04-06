DTC ?= dtc
CPP ?= cpp
DESTDIR ?=

DTCVERSION ?= $(shell $(DTC) --version | grep ^Version | sed 's/^.* //g')

MAKEFLAGS += -rR --no-print-directory

ALL_PLATFORMES := $(patsubst overlays/%,%,$(wildcard overlays/*))

PHONY += all
all: help # $(foreach i,$(ALL_PLATFORMES),all_$(i))
	@echo ""
	@echo "nothing to do, please use target all_<platform>"

PHONY += clean
clean: # $(foreach i,$(ALL_PLATFORMES),clean_$(i))
	@echo "nothing to do, please use target clean_<platform>"

PHONY += install
install: # $(foreach i,$(ALL_PLATFORMES),install_$(i))
	@echo "nothing to do, please use target install_<platform>"


install_%:
	make -C overlays/$* install_bin
	make -C modules/aw882xx start_aw882xx_drv_ko


help:
	@echo "Targets:"
	@echo ""
	@echo "  all_<PLATFORM>:            Build all device tree binaries for <PLATFORM>"
	@echo "  clean_<PLATFORM>:          Clean all generated files for <PLATFORM>"
	@echo "  install_<PLATFORM>:        Install all generated files for <PLATFORM> (sudo)"
	@echo ""
	@echo "  overlays/<PLATFORM>/<DTS>.dtbo   Build a single device tree binary"
	@echo ""
	@echo "Architectures: $(ALL_PLATFORMES)"
	@echo ""
	@echo "Obsolete Targets: (no longer supported)"
	@echo "  all:                   Build all device tree binaries for all architectures"
	@echo "  clean:                 Clean all generated files"
	@echo "  install:               Install all generated files (sudo)"

