#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "DeterministicAxiMemory.h"
#include "ElfImage.h"
#include "VCPU.h"

namespace {

struct Options {
  std::string elf;
  uint64_t seed = 1;
  uint64_t max_cycles = 1000000;
  uint64_t stop_after_retired = 0;
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
    if ((argument == "--elf" || argument == "--seed" ||
         argument == "--max-cycles" || argument == "--stop-after-retired") &&
        index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + argument);
    }
    if (argument == "--elf") {
      options.elf = argv[++index];
    } else if (argument == "--seed") {
      options.seed = parseUnsigned(argv[++index], "--seed");
    } else if (argument == "--max-cycles") {
      options.max_cycles = parseUnsigned(argv[++index], "--max-cycles");
    } else if (argument == "--stop-after-retired") {
      options.stop_after_retired = parseUnsigned(argv[++index], "--stop-after-retired");
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  if (options.elf.empty() || options.seed == 0 || options.stop_after_retired == 0) {
    throw std::invalid_argument("--elf, nonzero --seed, and nonzero --stop-after-retired are required");
  }
  return options;
}

zircon::sim::AxiMasterSignals captureMaster(const VCPU& dut) {
  return {
      .aw_valid = static_cast<bool>(dut.io_axi_awvalid),
      .aw_id = static_cast<uint8_t>(dut.io_axi_awid),
      .aw_addr = static_cast<uint32_t>(dut.io_axi_awaddr),
      .aw_len = static_cast<uint8_t>(dut.io_axi_awlen),
      .aw_size = static_cast<uint8_t>(dut.io_axi_awsize),
      .aw_burst = static_cast<uint8_t>(dut.io_axi_awburst),
      .w_valid = static_cast<bool>(dut.io_axi_wvalid),
      .w_data = static_cast<uint32_t>(dut.io_axi_wdata),
      .w_strb = static_cast<uint8_t>(dut.io_axi_wstrb),
      .w_last = static_cast<bool>(dut.io_axi_wlast),
      .b_ready = static_cast<bool>(dut.io_axi_bready),
      .ar_valid = static_cast<bool>(dut.io_axi_arvalid),
      .ar_id = static_cast<uint8_t>(dut.io_axi_arid),
      .ar_addr = static_cast<uint32_t>(dut.io_axi_araddr),
      .ar_len = static_cast<uint8_t>(dut.io_axi_arlen),
      .ar_size = static_cast<uint8_t>(dut.io_axi_arsize),
      .ar_burst = static_cast<uint8_t>(dut.io_axi_arburst),
      .r_ready = static_cast<bool>(dut.io_axi_rready),
  };
}

void driveSlave(VCPU& dut, const zircon::sim::AxiSlaveSignals& slave) {
  dut.io_axi_awready = slave.aw_ready;
  dut.io_axi_wready = slave.w_ready;
  dut.io_axi_bvalid = slave.b_valid;
  dut.io_axi_bid = slave.b_id;
  dut.io_axi_bresp = slave.b_resp;
  dut.io_axi_arready = slave.ar_ready;
  dut.io_axi_rvalid = slave.r_valid;
  dut.io_axi_rid = slave.r_id;
  dut.io_axi_rdata = slave.r_data;
  dut.io_axi_rresp = slave.r_resp;
  dut.io_axi_rlast = slave.r_last;

  // The old top-level exports commit debug Decoupled ports; they are sinks only.
  dut.io_dbg_cmt_robDeq_deq_0_ready = 1;
  dut.io_dbg_cmt_robDeq_deq_1_ready = 1;
  dut.io_dbg_cmt_robDeq_flush = 0;
  dut.io_dbg_cmt_bdbDeq_deq_ready = 1;
  dut.io_dbg_cmt_bdbDeq_flush = 0;
}

uint64_t acceptedRetirements(const VCPU& dut) {
  const bool lane0 = dut.io_dbg_cmt_robDeq_deq_0_valid &&
                     dut.io_dbg_cmt_robDeq_deq_0_ready;
  const bool lane1 = dut.io_dbg_cmt_robDeq_deq_1_valid &&
                     dut.io_dbg_cmt_robDeq_deq_1_ready;
  return static_cast<uint64_t>(lane0) + static_cast<uint64_t>(lane1);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parseOptions(argc, argv);
    const auto image = zircon::sim::ElfImage::load(options.elf);
    zircon::sim::DeterministicAxiMemory memory(image.memory(), options.seed, std::nullopt);
    Verilated::commandArgs(argc, argv);
    VCPU dut;

    driveSlave(dut, {});
    dut.reset = 1;
    for (int reset_cycle = 0; reset_cycle < 2; ++reset_cycle) {
      dut.clock = 0;
      dut.eval();
      dut.clock = 1;
      dut.eval();
    }
    dut.reset = 0;

    uint64_t retired = 0;
    for (uint64_t cycle = 0; cycle < options.max_cycles; ++cycle) {
      dut.clock = 0;
      dut.eval();
      const auto slave = memory.drive();
      driveSlave(dut, slave);
      dut.eval();
      const uint64_t remaining = options.stop_after_retired - retired;
      retired += std::min(acceptedRetirements(dut), remaining);
      if (retired >= options.stop_after_retired) {
        dut.final();
        std::cout << "{\"status\":\"retire-limit\",\"cycles\":" << cycle + 1
                  << ",\"seed\":" << options.seed << ",\"entry\":" << image.entry()
                  << ",\"retired\":" << retired << "}" << std::endl;
        return 0;
      }
      const auto master = captureMaster(dut);
      dut.clock = 1;
      dut.eval();
      memory.advance(master, slave);
    }

    dut.final();
    std::cerr << "baseline-ipc: retired " << retired << " events, expected "
              << options.stop_after_retired << std::endl;
    return 3;
  } catch (const std::exception& error) {
    std::cerr << "baseline-ipc: " << error.what() << std::endl;
    return 2;
  }
}
