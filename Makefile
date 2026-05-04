# esp32-s3-touch-openclaw
#
# Override the PlatformIO binary or serial port from the command line, e.g.
#   make flash PORT=/dev/ttyACM0
#   make monitor PIO=~/.platformio/penv/bin/pio

PIO  ?= pio
PORT ?=
BAUD ?= 115200

FIRMWARE_DIR := firmware

PIO_RUN := $(PIO) run -d $(FIRMWARE_DIR)
PIO_MONITOR := $(PIO) device monitor -d $(FIRMWARE_DIR)
UPLOAD_FLAGS :=
MONITOR_FLAGS := -b $(BAUD)
ifneq ($(PORT),)
UPLOAD_FLAGS  += --upload-port $(PORT)
MONITOR_FLAGS += -p $(PORT)
endif

.PHONY: help build flash monitor flash-monitor clean

help:
	@echo "Firmware targets:"
	@echo "  make build           Compile firmware"
	@echo "  make flash           Upload firmware to the device"
	@echo "  make monitor         Open the serial monitor"
	@echo "  make flash-monitor   Flash, then open the serial monitor"
	@echo "  make clean           Remove firmware build artifacts"
	@echo ""
	@echo "Variables (override on command line):"
	@echo "  PIO   PlatformIO binary (default: pio)"
	@echo "  PORT  Serial port for flash/monitor (auto-detect if unset)"
	@echo "  BAUD  Monitor baud rate (default: 115200)"

build:
	$(PIO_RUN)

flash:
	$(PIO_RUN) -t upload $(UPLOAD_FLAGS)

monitor:
	$(PIO_MONITOR) $(MONITOR_FLAGS)

flash-monitor:
	$(PIO_RUN) -t upload -t monitor $(UPLOAD_FLAGS)

clean:
	$(PIO_RUN) -t clean
