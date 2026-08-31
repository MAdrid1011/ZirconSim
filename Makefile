WORK_DIR := $(abspath .)
PARENT_DIR := $(abspath ..)
BUILD_DIR := $(WORK_DIR)/build
VERILOG_DIR := $(PARENT_DIR)/generated
VERILOG_TOP := $(VERILOG_DIR)/ZirconCore.sv
RTL_BINARY := $(BUILD_DIR)/rtl/VZirconCore
UNIT_BINARY := $(BUILD_DIR)/unit-tests
TEST_ELF := $(PARENT_DIR)/RV-Software/picotest/build/pico-rv32imaf_zicsr_zifencei-ilp32f.elf

CXX ?= c++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Werror -I$(WORK_DIR)/include
VERILATOR_CXXFLAGS := -std=c++20 -O2 -Wall -Wextra \
	-Wno-sign-compare -Wno-unused-parameter -Wno-unused-variable \
	-Wno-unused-but-set-variable \
	-I$(WORK_DIR)/include

.PHONY: all unit software verilog rtl smoke clean

all: unit

unit: $(UNIT_BINARY) software
	$(UNIT_BINARY) $(TEST_ELF)

$(UNIT_BINARY): src/ElfImage.cc tests/UnitTests.cc include/ElfImage.h include/DeterministicRng.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) src/ElfImage.cc tests/UnitTests.cc -o $@

software:
	$(MAKE) -C $(PARENT_DIR)/RV-Software/picotest image

verilog:
	$(MAKE) -C $(PARENT_DIR) verilog

rtl: verilog
	@mkdir -p $(BUILD_DIR)/rtl
	verilator --cc --exe --build --trace -Wall -Wno-fatal -Wno-UNUSEDSIGNAL \
		--top-module ZirconCore --Mdir $(BUILD_DIR)/rtl \
		-CFLAGS "$(VERILATOR_CXXFLAGS)" \
		$(VERILOG_TOP) $(WORK_DIR)/src/main.cc $(WORK_DIR)/src/ElfImage.cc

smoke: rtl software
	$(RTL_BINARY) --elf $(TEST_ELF) --seed 1 --max-cycles 10 --allow-timeout

clean:
	rm -rf $(BUILD_DIR)
