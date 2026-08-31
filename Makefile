WORK_DIR := $(abspath .)
PARENT_DIR := $(abspath ..)
BUILD_DIR := $(WORK_DIR)/build
VERILOG_DIR := $(PARENT_DIR)/generated-trace
VERILOG_TOP := $(VERILOG_DIR)/ZirconCore.sv
VERILOG_SOURCES := $(wildcard $(VERILOG_DIR)/*.sv)
RTL_BINARY := $(BUILD_DIR)/rtl/VZirconCore
UNIT_BINARY := $(BUILD_DIR)/unit-tests
DIFF_BINARY := $(BUILD_DIR)/commit-trace-diff
TEST_ELF := $(PARENT_DIR)/RV-Software/picotest/build/pico-rv32imaf_zicsr_zifencei-ilp32f.elf
DIFF_PREFIX_ELF := $(BUILD_DIR)/rv32i-commit-prefix.elf
DIFF_PREFIX_TRACE := $(BUILD_DIR)/rv32i-commit-prefix.jsonl
DIFF_PREFIX_SPIKE_LOG := $(BUILD_DIR)/rv32i-commit-prefix.spike.log
DIFF_ALU_BRANCH_ELF := $(BUILD_DIR)/rv32i-alu-branch-prefix.elf
DIFF_ALU_BRANCH_TRACE := $(BUILD_DIR)/rv32i-alu-branch-prefix.jsonl
DIFF_ALU_BRANCH_SPIKE_LOG := $(BUILD_DIR)/rv32i-alu-branch-prefix.spike.log
SPIKE ?= spike

CXX ?= c++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Werror -I$(WORK_DIR)/include
VERILATOR_CXXFLAGS := -std=c++20 -O2 -Wall -Wextra \
	-Wno-sign-compare -Wno-unused-parameter -Wno-unused-variable \
	-Wno-unused-but-set-variable \
	-I$(WORK_DIR)/include

.PHONY: all unit software verilog rtl smoke diff diff-prefix diff-alu-branch clean

all: unit

unit: $(UNIT_BINARY) software
	$(UNIT_BINARY) $(TEST_ELF)

$(UNIT_BINARY): src/CommitTrace.cc src/DeterministicAxiMemory.cc src/ElfImage.cc tests/UnitTests.cc include/CommitTrace.h include/DeterministicAxiMemory.h include/ElfImage.h include/DeterministicRng.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) src/CommitTrace.cc src/DeterministicAxiMemory.cc src/ElfImage.cc tests/UnitTests.cc -o $@

$(DIFF_BINARY): src/CommitTrace.cc src/CommitTraceDiff.cc include/CommitTrace.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) src/CommitTrace.cc src/CommitTraceDiff.cc -o $@

$(BUILD_DIR)/%.elf: tests/%.S tests/rv32i-commit-prefix.ld
	@mkdir -p $(BUILD_DIR)
	clang --target=riscv32 -march=rv32i_zicsr_zifencei -mabi=ilp32 -nostdlib -fuse-ld=lld \
		-Wl,-T,$(WORK_DIR)/tests/rv32i-commit-prefix.ld -Wl,--build-id=none \
		-Wl,-e,_start -o $@ $<

software:
	$(MAKE) -C $(PARENT_DIR)/RV-Software/picotest image

verilog:
	$(MAKE) -C $(PARENT_DIR) trace-verilog

rtl: verilog
	@mkdir -p $(BUILD_DIR)/rtl
	verilator --cc --exe --build --trace -Wall -Wno-fatal -Wno-UNUSEDSIGNAL \
		--top-module ZirconCore --Mdir $(BUILD_DIR)/rtl \
		-CFLAGS "$(VERILATOR_CXXFLAGS)" \
		$(VERILOG_SOURCES) $(WORK_DIR)/src/main.cc $(WORK_DIR)/src/DeterministicAxiMemory.cc $(WORK_DIR)/src/ElfImage.cc

smoke: rtl software
	$(RTL_BINARY) --elf $(TEST_ELF) --retire-trace $(BUILD_DIR)/smoke-retire.jsonl --seed 1 --max-cycles 10 --allow-timeout

diff: diff-prefix diff-alu-branch

diff-prefix: rtl $(DIFF_BINARY) $(DIFF_PREFIX_ELF)
	$(RTL_BINARY) --elf $(DIFF_PREFIX_ELF) --retire-trace $(DIFF_PREFIX_TRACE) --seed 1 --max-cycles 1024 --expect-retired 17 --allow-timeout
	$(SPIKE) --isa=RV32I_Zicsr_Zifencei --priv=m --pc=0x80000000 --instructions=17 --log-commits --log=$(DIFF_PREFIX_SPIKE_LOG) $(DIFF_PREFIX_ELF)
	$(DIFF_BINARY) --zircon-trace $(DIFF_PREFIX_TRACE) --spike-log $(DIFF_PREFIX_SPIKE_LOG) --max-events 17

diff-alu-branch: rtl $(DIFF_BINARY) $(DIFF_ALU_BRANCH_ELF)
	$(RTL_BINARY) --elf $(DIFF_ALU_BRANCH_ELF) --retire-trace $(DIFF_ALU_BRANCH_TRACE) --seed 1 --max-cycles 1024 --expect-retired 32 --allow-timeout
	$(SPIKE) --isa=RV32I_Zicsr_Zifencei --priv=m --pc=0x80000000 --instructions=32 --log-commits --log=$(DIFF_ALU_BRANCH_SPIKE_LOG) $(DIFF_ALU_BRANCH_ELF)
	$(DIFF_BINARY) --zircon-trace $(DIFF_ALU_BRANCH_TRACE) --spike-log $(DIFF_ALU_BRANCH_SPIKE_LOG) --max-events 32

clean:
	rm -rf $(BUILD_DIR)
