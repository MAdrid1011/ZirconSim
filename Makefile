PARENT_DIR := $(abspath ..)
BUILD_DIR ?= $(PARENT_DIR)/build/cmake
TEST_ELF ?= $(PARENT_DIR)/RV-Software/picotest/build/pico-rv32imaf_zicsr_zifencei-ilp32f.elf
CMAKE_ARGS ?=

.PHONY: all configure unit verilog rtl smoke clean

all: rtl

configure:
	cmake -S $(PARENT_DIR) -B $(BUILD_DIR) $(CMAKE_ARGS)

unit: configure
	cmake --build $(BUILD_DIR) --target zircon-sim-unit --parallel
	$(BUILD_DIR)/bin/zircon-sim-unit $(TEST_ELF)

verilog: configure
	cmake --build $(BUILD_DIR) --target zircon-rtl --parallel

rtl: configure
	cmake --build $(BUILD_DIR) --target zircon-sim --parallel

smoke: rtl
	$(BUILD_DIR)/bin/zircon-sim --elf $(TEST_ELF) --seed 1 --max-cycles 10 --allow-timeout

clean:
	cmake -E rm -rf $(BUILD_DIR)
