#include <verilated.h>
#include <verilated_vcd_c.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include "DeterministicAxiMemory.h"
#include "ElfImage.h"
#include "VZirconCore.h"

namespace {

struct Options {
  std::string elf;
  std::string retire_trace;
  uint64_t seed = 1;
  uint64_t max_cycles = 1000000;
  uint64_t expect_retired = 0;
  bool allow_timeout = false;
  bool wave = false;
};

struct RetireEvent {
  bool valid = false;
  uint64_t order = 0;
  uint32_t pc = 0;
  uint32_t instruction = 0;
  uint8_t privilege = 0;
  bool gpr_write = false;
  uint8_t gpr_address = 0;
  uint32_t gpr_data = 0;
  bool fpr_write = false;
  uint8_t fpr_address = 0;
  uint32_t fpr_data = 0;
  bool csr_write = false;
  uint16_t csr_address = 0;
  uint32_t csr_data = 0;
  uint32_t memory_address = 0;
  uint8_t memory_read_mask = 0;
  uint8_t memory_write_mask = 0;
  uint32_t memory_read_data = 0;
  uint32_t memory_write_data = 0;
  bool trap = false;
  bool interrupt = false;
  uint32_t cause = 0;
  uint32_t trap_value = 0;
  uint8_t fflags = 0;
};

uint64_t parseUnsigned(const char* text, const char* option) {
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 0);
  if (text[0] == '\0' || end == nullptr || *end != '\0') {
    throw std::invalid_argument(std::string("invalid value for ") + option);
  }
  return value;
}

Options parseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if ((argument == "--elf" || argument == "--retire-trace" ||
         argument == "--seed" || argument == "--max-cycles" ||
         argument == "--expect-retired") &&
        index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + argument);
    }
    if (argument == "--elf") {
      options.elf = argv[++index];
    } else if (argument == "--retire-trace") {
      options.retire_trace = argv[++index];
    } else if (argument == "--seed") {
      options.seed = parseUnsigned(argv[++index], "--seed");
    } else if (argument == "--max-cycles") {
      options.max_cycles = parseUnsigned(argv[++index], "--max-cycles");
    } else if (argument == "--expect-retired") {
      options.expect_retired = parseUnsigned(argv[++index], "--expect-retired");
    } else if (argument == "--allow-timeout") {
      options.allow_timeout = true;
    } else if (argument == "--wave") {
      options.wave = true;
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  if (options.elf.empty() || options.retire_trace.empty()) {
    throw std::invalid_argument("--elf and --retire-trace are required");
  }
  return options;
}

zircon::sim::AxiMasterSignals captureMaster(const VZirconCore& dut) {
  return {
      .aw_valid = static_cast<bool>(dut.io_axi_aw_valid),
      .aw_id = static_cast<uint8_t>(dut.io_axi_aw_bits_id),
      .aw_addr = static_cast<uint32_t>(dut.io_axi_aw_bits_addr),
      .aw_len = static_cast<uint8_t>(dut.io_axi_aw_bits_len),
      .aw_size = static_cast<uint8_t>(dut.io_axi_aw_bits_size),
      .aw_burst = static_cast<uint8_t>(dut.io_axi_aw_bits_burst),
      .w_valid = static_cast<bool>(dut.io_axi_w_valid),
      .w_data = static_cast<uint32_t>(dut.io_axi_w_bits_data),
      .w_strb = static_cast<uint8_t>(dut.io_axi_w_bits_strb),
      .w_last = static_cast<bool>(dut.io_axi_w_bits_last),
      .b_ready = static_cast<bool>(dut.io_axi_b_ready),
      .ar_valid = static_cast<bool>(dut.io_axi_ar_valid),
      .ar_id = static_cast<uint8_t>(dut.io_axi_ar_bits_id),
      .ar_addr = static_cast<uint32_t>(dut.io_axi_ar_bits_addr),
      .ar_len = static_cast<uint8_t>(dut.io_axi_ar_bits_len),
      .ar_size = static_cast<uint8_t>(dut.io_axi_ar_bits_size),
      .ar_burst = static_cast<uint8_t>(dut.io_axi_ar_bits_burst),
      .r_ready = static_cast<bool>(dut.io_axi_r_ready),
  };
}

void driveSlave(VZirconCore& dut, const zircon::sim::AxiSlaveSignals& slave) {
  dut.io_axi_aw_ready = slave.aw_ready;
  dut.io_axi_w_ready = slave.w_ready;
  dut.io_axi_b_valid = slave.b_valid;
  dut.io_axi_b_bits_id = slave.b_id;
  dut.io_axi_b_bits_resp = slave.b_resp;
  dut.io_axi_ar_ready = slave.ar_ready;
  dut.io_axi_r_valid = slave.r_valid;
  dut.io_axi_r_bits_id = slave.r_id;
  dut.io_axi_r_bits_data = slave.r_data;
  dut.io_axi_r_bits_resp = slave.r_resp;
  dut.io_axi_r_bits_last = slave.r_last;
  dut.io_interrupts_meip = 0;
  dut.io_interrupts_msip = 0;
  dut.io_interrupts_mtip = 0;
}

void emitTrace(std::ostream& stream, const RetireEvent& event, uint64_t& expected_order) {
  if (!event.valid) return;
  if (event.order != expected_order) {
    throw std::runtime_error("non-monotonic or duplicate retire order");
  }
  ++expected_order;
  stream << std::boolalpha;
  stream << "{\"order\":" << event.order << ",\"pc\":" << event.pc
         << ",\"instruction\":" << event.instruction
         << ",\"privilege\":" << static_cast<unsigned>(event.privilege)
         << ",\"gprWrite\":" << event.gpr_write
         << ",\"gprAddress\":" << static_cast<unsigned>(event.gpr_address)
         << ",\"gprData\":" << event.gpr_data
         << ",\"fprWrite\":" << event.fpr_write
         << ",\"fprAddress\":" << static_cast<unsigned>(event.fpr_address)
         << ",\"fprData\":" << event.fpr_data
         << ",\"csrWrite\":" << event.csr_write
         << ",\"csrAddress\":" << event.csr_address
         << ",\"csrData\":" << event.csr_data
         << ",\"memoryAddress\":" << event.memory_address
         << ",\"memoryReadMask\":" << static_cast<unsigned>(event.memory_read_mask)
         << ",\"memoryWriteMask\":" << static_cast<unsigned>(event.memory_write_mask)
         << ",\"memoryReadData\":" << event.memory_read_data
         << ",\"memoryWriteData\":" << event.memory_write_data
         << ",\"trap\":" << event.trap
         << ",\"interrupt\":" << event.interrupt
         << ",\"cause\":" << event.cause
         << ",\"trapValue\":" << event.trap_value
         << ",\"fflags\":" << static_cast<unsigned>(event.fflags) << "}\n";
}

RetireEvent lane0(const VZirconCore& dut) {
  return {
      .valid = static_cast<bool>(dut.io_trace_0_valid), .order = dut.io_trace_0_order,
      .pc = dut.io_trace_0_pc, .instruction = dut.io_trace_0_instruction,
      .privilege = static_cast<uint8_t>(dut.io_trace_0_privilege),
      .gpr_write = static_cast<bool>(dut.io_trace_0_gprWrite),
      .gpr_address = static_cast<uint8_t>(dut.io_trace_0_gprAddress),
      .gpr_data = dut.io_trace_0_gprData, .fpr_write = static_cast<bool>(dut.io_trace_0_fprWrite),
      .fpr_address = static_cast<uint8_t>(dut.io_trace_0_fprAddress),
      .fpr_data = dut.io_trace_0_fprData, .csr_write = static_cast<bool>(dut.io_trace_0_csrWrite),
      .csr_address = static_cast<uint16_t>(dut.io_trace_0_csrAddress),
      .csr_data = dut.io_trace_0_csrData, .memory_address = dut.io_trace_0_memoryAddress,
      .memory_read_mask = static_cast<uint8_t>(dut.io_trace_0_memoryReadMask),
      .memory_write_mask = static_cast<uint8_t>(dut.io_trace_0_memoryWriteMask),
      .memory_read_data = dut.io_trace_0_memoryReadData,
      .memory_write_data = dut.io_trace_0_memoryWriteData,
      .trap = static_cast<bool>(dut.io_trace_0_trap),
      .interrupt = static_cast<bool>(dut.io_trace_0_interrupt), .cause = dut.io_trace_0_cause,
      .trap_value = dut.io_trace_0_trapValue,
      .fflags = static_cast<uint8_t>(dut.io_trace_0_fflags),
  };
}

RetireEvent lane1(const VZirconCore& dut) {
  return {
      .valid = static_cast<bool>(dut.io_trace_1_valid), .order = dut.io_trace_1_order,
      .pc = dut.io_trace_1_pc, .instruction = dut.io_trace_1_instruction,
      .privilege = static_cast<uint8_t>(dut.io_trace_1_privilege),
      .gpr_write = static_cast<bool>(dut.io_trace_1_gprWrite),
      .gpr_address = static_cast<uint8_t>(dut.io_trace_1_gprAddress),
      .gpr_data = dut.io_trace_1_gprData, .fpr_write = static_cast<bool>(dut.io_trace_1_fprWrite),
      .fpr_address = static_cast<uint8_t>(dut.io_trace_1_fprAddress),
      .fpr_data = dut.io_trace_1_fprData, .csr_write = static_cast<bool>(dut.io_trace_1_csrWrite),
      .csr_address = static_cast<uint16_t>(dut.io_trace_1_csrAddress),
      .csr_data = dut.io_trace_1_csrData, .memory_address = dut.io_trace_1_memoryAddress,
      .memory_read_mask = static_cast<uint8_t>(dut.io_trace_1_memoryReadMask),
      .memory_write_mask = static_cast<uint8_t>(dut.io_trace_1_memoryWriteMask),
      .memory_read_data = dut.io_trace_1_memoryReadData,
      .memory_write_data = dut.io_trace_1_memoryWriteData,
      .trap = static_cast<bool>(dut.io_trace_1_trap),
      .interrupt = static_cast<bool>(dut.io_trace_1_interrupt), .cause = dut.io_trace_1_cause,
      .trap_value = dut.io_trace_1_trapValue,
      .fflags = static_cast<uint8_t>(dut.io_trace_1_fflags),
  };
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parseOptions(argc, argv);
    auto image = zircon::sim::ElfImage::load(options.elf);
    const auto tohost = image.symbol("tohost");
    if (!tohost.has_value()) throw std::runtime_error("ELF does not define tohost");
    std::ofstream trace_stream(options.retire_trace);
    if (!trace_stream) throw std::runtime_error("cannot create retire trace: " + options.retire_trace);

    zircon::sim::DeterministicAxiMemory memory(image.memory(), options.seed, tohost);
    Verilated::commandArgs(argc, argv);
    VZirconCore dut;
    VerilatedVcdC wave;
    uint64_t simulation_time = 0;
    uint64_t expected_order = 0;
    if (options.wave) {
      Verilated::traceEverOn(true);
      dut.trace(&wave, 8);
      wave.open("build/zircon.vcd");
    }

    driveSlave(dut, {});
    dut.reset = 1;
    for (int reset_cycle = 0; reset_cycle < 2; ++reset_cycle) {
      dut.clock = 0;
      dut.eval();
      if (options.wave) wave.dump(simulation_time++);
      dut.clock = 1;
      dut.eval();
      if (options.wave) wave.dump(simulation_time++);
    }
    dut.reset = 0;

    for (uint64_t cycle = 0; cycle < options.max_cycles; ++cycle) {
      dut.clock = 0;
      dut.eval();
      const auto slave = memory.drive();
      driveSlave(dut, slave);
      dut.eval();
      if (options.wave) wave.dump(simulation_time++);
      emitTrace(trace_stream, lane0(dut), expected_order);
      emitTrace(trace_stream, lane1(dut), expected_order);
      const auto master = captureMaster(dut);

      dut.clock = 1;
      dut.eval();
      if (options.wave) wave.dump(simulation_time++);
      memory.advance(master, slave);
      if (memory.exitStatus().has_value()) {
        dut.final();
        if (options.wave) wave.close();
        const int status = *memory.exitStatus();
        std::cout << "{\"status\":\"tohost\",\"cycles\":" << cycle + 1
                  << ",\"seed\":" << options.seed << ",\"entry\":" << image.entry()
                  << ",\"tohost\":" << *tohost << ",\"exit\":" << status
                  << ",\"retired\":" << expected_order << "}" << std::endl;
        return status == 0 ? 0 : 1;
      }
    }

    dut.final();
    if (options.wave) wave.close();
    if (expected_order < options.expect_retired) {
      std::cerr << "zircon-sim: retired " << expected_order << " events, expected at least "
                << options.expect_retired << std::endl;
      return 3;
    }
    std::cout << "{\"status\":\"timeout\",\"cycles\":" << options.max_cycles
              << ",\"seed\":" << options.seed << ",\"entry\":" << image.entry()
              << ",\"tohost\":" << *tohost << ",\"retired\":" << expected_order
              << "}" << std::endl;
    return options.allow_timeout ? 0 : 124;
  } catch (const std::exception& error) {
    std::cerr << "zircon-sim: " << error.what() << std::endl;
    return 2;
  }
}
