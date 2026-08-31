#include <verilated.h>
#include <verilated_vcd_c.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "DeterministicRng.h"
#include "ElfImage.h"
#include "VZirconCore.h"

namespace {

struct Options {
  std::string elf;
  uint64_t seed = 1;
  uint64_t max_cycles = 1000000;
  bool allow_timeout = false;
  bool wave = false;
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
    if ((argument == "--elf" || argument == "--seed" || argument == "--max-cycles") &&
        index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + argument);
    }
    if (argument == "--elf") {
      options.elf = argv[++index];
    } else if (argument == "--seed") {
      options.seed = parseUnsigned(argv[++index], "--seed");
    } else if (argument == "--max-cycles") {
      options.max_cycles = parseUnsigned(argv[++index], "--max-cycles");
    } else if (argument == "--allow-timeout") {
      options.allow_timeout = true;
    } else if (argument == "--wave") {
      options.wave = true;
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  if (options.elf.empty()) {
    throw std::invalid_argument("--elf is required");
  }
  return options;
}

void driveSlaveDefaults(VZirconCore& dut) {
  dut.io_axi_aw_ready = 1;
  dut.io_axi_w_ready = 1;
  dut.io_axi_b_valid = 0;
  dut.io_axi_b_bits_id = 0;
  dut.io_axi_b_bits_resp = 0;
  dut.io_axi_ar_ready = 1;
  dut.io_axi_r_valid = 0;
  dut.io_axi_r_bits_id = 0;
  dut.io_axi_r_bits_data = 0;
  dut.io_axi_r_bits_resp = 0;
  dut.io_axi_r_bits_last = 0;
  dut.io_interrupts_meip = 0;
  dut.io_interrupts_msip = 0;
  dut.io_interrupts_mtip = 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parseOptions(argc, argv);
    auto image = zircon::sim::ElfImage::load(options.elf);
    const auto tohost = image.symbol("tohost");
    if (!tohost.has_value()) {
      throw std::runtime_error("ELF does not define tohost");
    }
    zircon::sim::DeterministicRng rng(options.seed);
    (void)rng;

    Verilated::commandArgs(argc, argv);
    VZirconCore dut;
    VerilatedVcdC trace;
    uint64_t simulation_time = 0;
    if (options.wave) {
      Verilated::traceEverOn(true);
      dut.trace(&trace, 8);
      trace.open("build/zircon.vcd");
    }

    driveSlaveDefaults(dut);
    dut.reset = 1;
    for (int reset_cycle = 0; reset_cycle < 2; ++reset_cycle) {
      dut.clock = 0;
      dut.eval();
      if (options.wave) trace.dump(simulation_time++);
      dut.clock = 1;
      dut.eval();
      if (options.wave) trace.dump(simulation_time++);
    }
    dut.reset = 0;

    for (uint64_t cycle = 0; cycle < options.max_cycles; ++cycle) {
      driveSlaveDefaults(dut);
      dut.clock = 0;
      dut.eval();
      if (options.wave) trace.dump(simulation_time++);
      dut.clock = 1;
      dut.eval();
      if (options.wave) trace.dump(simulation_time++);
    }

    dut.final();
    if (options.wave) trace.close();
    std::cout << "{\"status\":\"timeout\",\"cycles\":" << options.max_cycles
              << ",\"seed\":" << options.seed << ",\"entry\":" << image.entry()
              << ",\"tohost\":" << *tohost << "}" << std::endl;
    return options.allow_timeout ? 0 : 124;
  } catch (const std::exception& error) {
    std::cerr << "zircon-sim: " << error.what() << std::endl;
    return 2;
  }
}
