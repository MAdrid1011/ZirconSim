WORK_DIR := $(abspath .)
PARENT_DIR := $(abspath ..)
BUILD_DIR := $(WORK_DIR)/build
RETIRE_VERILOG_DIR := $(PARENT_DIR)/generated-trace
RETIRE_VERILOG_TOP := $(RETIRE_VERILOG_DIR)/ZirconCore.sv
RETIRE_VERILOG_SOURCES = $(wildcard $(RETIRE_VERILOG_DIR)/*.sv)
RTL_BINARY := $(BUILD_DIR)/rtl/VZirconCore
TRACE_RTL_BINARY := $(BUILD_DIR)/trace-rtl/VZirconCore
UNIT_BINARY := $(BUILD_DIR)/unit-tests
DIFF_BINARY := $(BUILD_DIR)/commit-trace-diff
BASELINE_IPC_BINARY := $(BUILD_DIR)/baseline-ipc-runner
TEST_ELF := $(PARENT_DIR)/RV-Software/picotest/build/pico-rv32imaf_zicsr_zifencei-ilp32f.elf
DIFF_PREFIX_ELF := $(BUILD_DIR)/rv32i-commit-prefix.elf
DIFF_PREFIX_TRACE := $(BUILD_DIR)/rv32i-commit-prefix.jsonl
DIFF_PREFIX_SPIKE_LOG := $(BUILD_DIR)/rv32i-commit-prefix.spike.log
DIFF_ALU_BRANCH_ELF := $(BUILD_DIR)/rv32i-alu-branch-prefix.elf
DIFF_ALU_BRANCH_TRACE := $(BUILD_DIR)/rv32i-alu-branch-prefix.jsonl
DIFF_ALU_BRANCH_SPIKE_LOG := $(BUILD_DIR)/rv32i-alu-branch-prefix.spike.log
DIFF_RV32M_ELF := $(BUILD_DIR)/rv32m-commit-prefix.elf
DIFF_RV32M_TRACE := $(BUILD_DIR)/rv32m-commit-prefix.jsonl
DIFF_RV32M_SPIKE_LOG := $(BUILD_DIR)/rv32m-commit-prefix.spike.log
DIFF_RV32M_SAIL_LOG := $(BUILD_DIR)/rv32m-commit-prefix.sail.log
DIFF_PREFIX_SAIL_MEMORY_LOG := $(BUILD_DIR)/rv32i-commit-prefix.sail-memory.log
DIFF_ALU_BRANCH_SAIL_MEMORY_LOG := $(BUILD_DIR)/rv32i-alu-branch-prefix.sail-memory.log
DIFF_RV32M_SAIL_MEMORY_LOG := $(BUILD_DIR)/rv32m-commit-prefix.sail-memory.log
RV32A_TOHOST_SAIL_MEMORY_LOG := $(BUILD_DIR)/rv32a-tohost.sail-memory.log
RV32A_TOHOST_ELF := $(BUILD_DIR)/rv32a-tohost.elf
RV32A_TOHOST_TRACE := $(BUILD_DIR)/rv32a-tohost.jsonl
DIFF_PREFIX_BACKING := $(BUILD_DIR)/rv32i-commit-prefix.backing-memory
DIFF_ALU_BRANCH_BACKING := $(BUILD_DIR)/rv32i-alu-branch-prefix.backing-memory
DIFF_RV32M_BACKING := $(BUILD_DIR)/rv32m-commit-prefix.backing-memory
RV32A_TOHOST_BACKING := $(BUILD_DIR)/rv32a-tohost.backing-memory
RV32A_TOHOST_SPIKE_LOG := $(BUILD_DIR)/rv32a-tohost.spike.log
THROUGHPUT_ELF := $(BUILD_DIR)/sim-throughput.elf
THROUGHPUT_TRACE := $(BUILD_DIR)/sim-throughput.jsonl
THROUGHPUT_CYCLES ?= 100000
SPIKE ?= spike
SAIL ?= sail_riscv_sim
BASELINE_2024 ?=
BASELINE_2024_CORE_SHA := 65a3dd381f4c83a5844858a927dafdbc8263c35e
BASELINE_2024_SOFTWARE_SHA := 5f81f2ad378f537182e4cf1a0fcb45159509a2ec
BASELINE_2024_SIM_SHA := b1694da4a92046edeead50c9b2a1c086a13e6511
VERILATOR_INCLUDE ?= /usr/local/share/verilator/include

CXX ?= c++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Werror -I$(WORK_DIR)/include
BASELINE_CXXFLAGS := $(CXXFLAGS) -Wno-error=unused-parameter -Wno-error=sign-compare \
	-Wno-unused-parameter -Wno-sign-compare
VERILATOR_CXXFLAGS := -std=c++20 -O2 -Wall -Wextra \
	-Wno-sign-compare -Wno-unused-parameter -Wno-unused-variable \
	-Wno-unused-but-set-variable \
	-I$(WORK_DIR)/include
TRACE_VERILATOR_CXXFLAGS := $(VERILATOR_CXXFLAGS) -DZIRCON_SIM_TRACE=1
RTL_SOURCES := $(WORK_DIR)/src/main.cc $(WORK_DIR)/src/DeterministicAxiMemory.cc \
	$(WORK_DIR)/src/ElfImage.cc
CORE_SCALA_SOURCES := $(shell find $(PARENT_DIR)/src/main/scala -type f -name '*.scala')
CORE_BUILD_INPUTS := $(CORE_SCALA_SOURCES) $(PARENT_DIR)/build.sbt \
	$(wildcard $(PARENT_DIR)/project/*.sbt) $(wildcard $(PARENT_DIR)/project/*.scala)

.PHONY: all unit software verilog trace-verilog rtl trace-rtl smoke trace-smoke tohost tohost-rv32i-prefix tohost-rv32i-alu-branch tohost-rv32m tohost-rv32a throughput diff diff-prefix diff-alu-branch diff-rv32m diff-sail-rv32m diff-memory-spike diff-memory-rv32i-prefix diff-memory-rv32i-alu-branch diff-memory-rv32m diff-memory-rv32a diff-memory-sail diff-memory-sail-rv32i-prefix diff-memory-sail-rv32i-alu-branch diff-memory-sail-rv32m diff-memory-sail-rv32a micro-ipc-rv32m check-baseline-2024 baseline-ipc-rv32m clean

all: unit

unit: $(UNIT_BINARY) software
	$(UNIT_BINARY) $(TEST_ELF)

$(UNIT_BINARY): src/CommitTrace.cc src/DeterministicAxiMemory.cc src/ElfImage.cc tests/UnitTests.cc include/CommitTrace.h include/DeterministicAxiMemory.h include/ElfImage.h include/DeterministicRng.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) src/CommitTrace.cc src/DeterministicAxiMemory.cc src/ElfImage.cc tests/UnitTests.cc -o $@

$(DIFF_BINARY): src/CommitTrace.cc src/CommitTraceDiff.cc src/ElfImage.cc include/CommitTrace.h include/ElfImage.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) src/CommitTrace.cc src/CommitTraceDiff.cc src/ElfImage.cc -o $@

$(BASELINE_IPC_BINARY): src/BaselineIpcRunner.cc src/DeterministicAxiMemory.cc src/ElfImage.cc include/DeterministicAxiMemory.h include/ElfImage.h
	@test -n "$(BASELINE_2024)" || (echo "BASELINE_2024 must name a clean Zircon-2024 checkout"; exit 2)
	@test -f "$(BASELINE_2024)/ZirconSim/build/VCPU__ALL.a" || (echo "build the baseline VCPU first"; exit 2)
	$(CXX) $(BASELINE_CXXFLAGS) -I$(BASELINE_2024)/ZirconSim/build -I$(VERILATOR_INCLUDE) \
		src/BaselineIpcRunner.cc src/DeterministicAxiMemory.cc src/ElfImage.cc \
		$(BASELINE_2024)/ZirconSim/build/VCPU__ALL.a \
		$(VERILATOR_INCLUDE)/verilated.cpp $(VERILATOR_INCLUDE)/verilated_threads.cpp \
		$(VERILATOR_INCLUDE)/verilated_vcd_c.cpp -pthread -latomic -o $@

$(BUILD_DIR)/%.elf: tests/%.S tests/rv32i-commit-prefix.ld
	@mkdir -p $(BUILD_DIR)
	clang --target=riscv32 -march=rv32i_zicsr_zifencei -mabi=ilp32 -nostdlib -fuse-ld=lld \
		-Wl,-T,$(WORK_DIR)/tests/rv32i-commit-prefix.ld -Wl,--build-id=none \
		-Wl,-e,_start -o $@ $<

$(DIFF_RV32M_ELF): tests/rv32m-commit-prefix.S tests/rv32i-commit-prefix.ld
	@mkdir -p $(BUILD_DIR)
	clang --target=riscv32 -march=rv32im_zicsr_zifencei -mabi=ilp32 -nostdlib -fuse-ld=lld \
		-Wl,-T,$(WORK_DIR)/tests/rv32i-commit-prefix.ld -Wl,--build-id=none \
		-Wl,-e,_start -o $@ $<

$(RV32A_TOHOST_ELF): tests/rv32a-tohost.S tests/rv32i-commit-prefix.ld
	@mkdir -p $(BUILD_DIR)
	clang --target=riscv32 -march=rv32ima_zicsr_zifencei -mabi=ilp32 -nostdlib -fuse-ld=lld \
		-Wl,-T,$(WORK_DIR)/tests/rv32i-commit-prefix.ld -Wl,--build-id=none \
		-Wl,-e,_start -o $@ $<

software:
	$(MAKE) -C $(PARENT_DIR)/RV-Software/picotest image

verilog:
	$(MAKE) -C $(PARENT_DIR) trace-verilog


trace-verilog:
	$(MAKE) -C $(PARENT_DIR) trace-verilog

$(RETIRE_VERILOG_TOP): $(CORE_BUILD_INPUTS)
	$(MAKE) -C $(PARENT_DIR) trace-verilog

$(RTL_BINARY): $(RETIRE_VERILOG_TOP) $(RTL_SOURCES)
	@mkdir -p $(BUILD_DIR)/rtl
	verilator --cc --exe --build -Wall -Wno-fatal -Wno-UNUSEDSIGNAL \
		--top-module ZirconCore --Mdir $(BUILD_DIR)/rtl \
		-CFLAGS "$(VERILATOR_CXXFLAGS)" \
		$(RETIRE_VERILOG_SOURCES) $(RTL_SOURCES)

$(TRACE_RTL_BINARY): $(RETIRE_VERILOG_TOP) $(RTL_SOURCES)
	@mkdir -p $(BUILD_DIR)/trace-rtl
	verilator --cc --exe --build --trace -Wall -Wno-fatal -Wno-UNUSEDSIGNAL \
		--top-module ZirconCore --Mdir $(BUILD_DIR)/trace-rtl \
		-CFLAGS "$(TRACE_VERILATOR_CXXFLAGS)" \
		$(RETIRE_VERILOG_SOURCES) $(RTL_SOURCES)

rtl: $(RTL_BINARY)

trace-rtl: $(TRACE_RTL_BINARY)

smoke: rtl software
	$(RTL_BINARY) --elf $(TEST_ELF) --retire-trace $(BUILD_DIR)/smoke-retire.jsonl --seed 1 --max-cycles 10 --allow-timeout

trace-smoke: trace-rtl software
	$(TRACE_RTL_BINARY) --elf $(TEST_ELF) --retire-trace $(BUILD_DIR)/trace-smoke-retire.jsonl --seed 1 --max-cycles 10 --allow-timeout --wave

tohost: tohost-rv32i-prefix tohost-rv32i-alu-branch tohost-rv32m tohost-rv32a

tohost-rv32i-prefix: rtl $(DIFF_PREFIX_ELF)
	$(RTL_BINARY) --elf $(DIFF_PREFIX_ELF) --retire-trace $(DIFF_PREFIX_TRACE) --backing-memory $(DIFF_PREFIX_BACKING) --seed 1 --max-cycles 1024

tohost-rv32i-alu-branch: rtl $(DIFF_ALU_BRANCH_ELF)
	$(RTL_BINARY) --elf $(DIFF_ALU_BRANCH_ELF) --retire-trace $(DIFF_ALU_BRANCH_TRACE) --backing-memory $(DIFF_ALU_BRANCH_BACKING) --seed 1 --max-cycles 1024

tohost-rv32m: rtl $(DIFF_RV32M_ELF)
	$(RTL_BINARY) --elf $(DIFF_RV32M_ELF) --retire-trace $(BUILD_DIR)/rv32m-tohost.jsonl --backing-memory $(DIFF_RV32M_BACKING) --seed 1 --max-cycles 2048

tohost-rv32a: rtl $(RV32A_TOHOST_ELF)
	$(RTL_BINARY) --elf $(RV32A_TOHOST_ELF) --retire-trace $(RV32A_TOHOST_TRACE) --backing-memory $(RV32A_TOHOST_BACKING) --seed 1 --max-cycles 2048

throughput: rtl $(THROUGHPUT_ELF)
	$(RTL_BINARY) --elf $(THROUGHPUT_ELF) --retire-trace $(THROUGHPUT_TRACE) --seed 1 --max-cycles $(THROUGHPUT_CYCLES) --allow-timeout

diff: diff-prefix diff-alu-branch diff-rv32m

diff-prefix: tohost-rv32i-prefix $(DIFF_BINARY)
	$(SPIKE) --isa=RV32I_Zicsr_Zifencei --priv=m --pc=0x80000000 --instructions=17 --log-commits --log=$(DIFF_PREFIX_SPIKE_LOG) $(DIFF_PREFIX_ELF)
	$(DIFF_BINARY) --zircon-trace $(DIFF_PREFIX_TRACE) --spike-log $(DIFF_PREFIX_SPIKE_LOG) --max-events 17

diff-alu-branch: tohost-rv32i-alu-branch $(DIFF_BINARY)
	$(SPIKE) --isa=RV32I_Zicsr_Zifencei --priv=m --pc=0x80000000 --instructions=32 --log-commits --log=$(DIFF_ALU_BRANCH_SPIKE_LOG) $(DIFF_ALU_BRANCH_ELF)
	$(DIFF_BINARY) --zircon-trace $(DIFF_ALU_BRANCH_TRACE) --spike-log $(DIFF_ALU_BRANCH_SPIKE_LOG) --max-events 32

diff-rv32m: tohost-rv32m $(DIFF_BINARY)
	cp $(BUILD_DIR)/rv32m-tohost.jsonl $(DIFF_RV32M_TRACE)
	$(SPIKE) --isa=RV32IM_Zicsr_Zifencei --priv=m --pc=0x80000000 --instructions=17 --log-commits --log=$(DIFF_RV32M_SPIKE_LOG) $(DIFF_RV32M_ELF)
	$(DIFF_BINARY) --zircon-trace $(DIFF_RV32M_TRACE) --spike-log $(DIFF_RV32M_SPIKE_LOG) --max-events 17

diff-sail-rv32m: tohost-rv32m $(DIFF_BINARY)
	cp $(BUILD_DIR)/rv32m-tohost.jsonl $(DIFF_RV32M_TRACE)
	$(SAIL) --rv32 --inst-limit 17 --trace-output $(DIFF_RV32M_SAIL_LOG) --trace-instr --trace-gpr --trace-csr $(DIFF_RV32M_ELF)
	$(DIFF_BINARY) --zircon-trace $(DIFF_RV32M_TRACE) --sail-log $(DIFF_RV32M_SAIL_LOG) --max-events 17

diff-memory-spike: diff-memory-rv32i-prefix diff-memory-rv32i-alu-branch diff-memory-rv32m diff-memory-rv32a

diff-memory-rv32i-prefix: tohost-rv32i-prefix $(DIFF_BINARY)
	$(SPIKE) --isa=RV32I_Zicsr_Zifencei --priv=m --pc=0x80000000 --instructions=19 --log-commits --log=$(DIFF_PREFIX_SPIKE_LOG) $(DIFF_PREFIX_ELF)
	$(DIFF_BINARY) --zircon-trace $(DIFF_PREFIX_TRACE) --spike-log $(DIFF_PREFIX_SPIKE_LOG) --memory-elf $(DIFF_PREFIX_ELF) --backing-memory $(DIFF_PREFIX_BACKING) --max-events 19

diff-memory-rv32i-alu-branch: tohost-rv32i-alu-branch $(DIFF_BINARY)
	$(SPIKE) --isa=RV32I_Zicsr_Zifencei --priv=m --pc=0x80000000 --instructions=34 --log-commits --log=$(DIFF_ALU_BRANCH_SPIKE_LOG) $(DIFF_ALU_BRANCH_ELF)
	$(DIFF_BINARY) --zircon-trace $(DIFF_ALU_BRANCH_TRACE) --spike-log $(DIFF_ALU_BRANCH_SPIKE_LOG) --memory-elf $(DIFF_ALU_BRANCH_ELF) --backing-memory $(DIFF_ALU_BRANCH_BACKING) --max-events 34

diff-memory-rv32m: tohost-rv32m $(DIFF_BINARY)
	cp $(BUILD_DIR)/rv32m-tohost.jsonl $(DIFF_RV32M_TRACE)
	$(SPIKE) --isa=RV32IM_Zicsr_Zifencei --priv=m --pc=0x80000000 --instructions=19 --log-commits --log=$(DIFF_RV32M_SPIKE_LOG) $(DIFF_RV32M_ELF)
	$(DIFF_BINARY) --zircon-trace $(DIFF_RV32M_TRACE) --spike-log $(DIFF_RV32M_SPIKE_LOG) --memory-elf $(DIFF_RV32M_ELF) --backing-memory $(DIFF_RV32M_BACKING) --max-events 19

diff-memory-rv32a: tohost-rv32a $(DIFF_BINARY)
	$(SPIKE) --isa=RV32IMA_Zicsr_Zifencei --priv=m --pc=0x80000000 --instructions=12 --log-commits --log=$(RV32A_TOHOST_SPIKE_LOG) $(RV32A_TOHOST_ELF)
	$(DIFF_BINARY) --zircon-trace $(RV32A_TOHOST_TRACE) --spike-log $(RV32A_TOHOST_SPIKE_LOG) --memory-elf $(RV32A_TOHOST_ELF) --backing-memory $(RV32A_TOHOST_BACKING) --max-events 12

diff-memory-sail: diff-memory-sail-rv32i-prefix diff-memory-sail-rv32i-alu-branch diff-memory-sail-rv32m diff-memory-sail-rv32a

diff-memory-sail-rv32i-prefix: tohost-rv32i-prefix $(DIFF_BINARY)
	$(SAIL) --rv32 --inst-limit 19 --trace-output $(DIFF_PREFIX_SAIL_MEMORY_LOG) --trace-instr --trace-gpr --trace-csr --trace-mem $(DIFF_PREFIX_ELF)
	$(DIFF_BINARY) --zircon-trace $(DIFF_PREFIX_TRACE) --sail-log $(DIFF_PREFIX_SAIL_MEMORY_LOG) --memory-elf $(DIFF_PREFIX_ELF) --backing-memory $(DIFF_PREFIX_BACKING) --max-events 19

diff-memory-sail-rv32i-alu-branch: tohost-rv32i-alu-branch $(DIFF_BINARY)
	$(SAIL) --rv32 --inst-limit 34 --trace-output $(DIFF_ALU_BRANCH_SAIL_MEMORY_LOG) --trace-instr --trace-gpr --trace-csr --trace-mem $(DIFF_ALU_BRANCH_ELF)
	$(DIFF_BINARY) --zircon-trace $(DIFF_ALU_BRANCH_TRACE) --sail-log $(DIFF_ALU_BRANCH_SAIL_MEMORY_LOG) --memory-elf $(DIFF_ALU_BRANCH_ELF) --backing-memory $(DIFF_ALU_BRANCH_BACKING) --max-events 34

diff-memory-sail-rv32m: tohost-rv32m $(DIFF_BINARY)
	cp $(BUILD_DIR)/rv32m-tohost.jsonl $(DIFF_RV32M_TRACE)
	$(SAIL) --rv32 --inst-limit 19 --trace-output $(DIFF_RV32M_SAIL_MEMORY_LOG) --trace-instr --trace-gpr --trace-csr --trace-mem $(DIFF_RV32M_ELF)
	$(DIFF_BINARY) --zircon-trace $(DIFF_RV32M_TRACE) --sail-log $(DIFF_RV32M_SAIL_MEMORY_LOG) --memory-elf $(DIFF_RV32M_ELF) --backing-memory $(DIFF_RV32M_BACKING) --max-events 19

diff-memory-sail-rv32a: tohost-rv32a $(DIFF_BINARY)
	$(SAIL) --rv32 --inst-limit 12 --trace-output $(RV32A_TOHOST_SAIL_MEMORY_LOG) --trace-instr --trace-gpr --trace-csr --trace-mem $(RV32A_TOHOST_ELF)
	$(DIFF_BINARY) --zircon-trace $(RV32A_TOHOST_TRACE) --sail-log $(RV32A_TOHOST_SAIL_MEMORY_LOG) --memory-elf $(RV32A_TOHOST_ELF) --backing-memory $(RV32A_TOHOST_BACKING) --max-events 12

micro-ipc-rv32m: rtl $(DIFF_RV32M_ELF)
	$(RTL_BINARY) --elf $(DIFF_RV32M_ELF) --retire-trace $(BUILD_DIR)/rv32m-ipc-prefix.jsonl --seed 1 --max-cycles 2048 --expect-retired 17 --stop-after-retired 17

check-baseline-2024:
	@test -n "$(BASELINE_2024)" || (echo "BASELINE_2024 must name a clean Zircon-2024 checkout"; exit 2)
	@test "$$(git -C $(BASELINE_2024) rev-parse HEAD)" = "$(BASELINE_2024_CORE_SHA)" || (echo "BASELINE_2024 core SHA does not match the immutable baseline"; exit 2)
	@test "$$(git -C $(BASELINE_2024)/RV-Software rev-parse HEAD)" = "$(BASELINE_2024_SOFTWARE_SHA)" || (echo "BASELINE_2024 RV-Software SHA does not match the immutable baseline"; exit 2)
	@test "$$(git -C $(BASELINE_2024)/ZirconSim rev-parse HEAD)" = "$(BASELINE_2024_SIM_SHA)" || (echo "BASELINE_2024 ZirconSim SHA does not match the immutable baseline"; exit 2)
	@test -z "$$(git -C $(BASELINE_2024) status --porcelain)" || (echo "BASELINE_2024 must be clean"; exit 2)

baseline-ipc-rv32m: check-baseline-2024 $(DIFF_RV32M_ELF)
	$(MAKE) -C $(BASELINE_2024)/ZirconSim
	$(MAKE) $(BASELINE_IPC_BINARY) BASELINE_2024=$(BASELINE_2024)
	$(BASELINE_IPC_BINARY) --elf $(DIFF_RV32M_ELF) --seed 1 --max-cycles 2048 --stop-after-retired 17

clean:
	rm -rf $(BUILD_DIR)
