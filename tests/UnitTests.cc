#include <cassert>
#include <sstream>
#include <iostream>
#include <stdexcept>

#include "CommitTrace.h"
#include "DeterministicRng.h"
#include "DeterministicAxiMemory.h"
#include "ElfImage.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: unit-tests <rv32-elf>" << std::endl;
    return 2;
  }

  zircon::sim::DeterministicRng first(1);
  zircon::sim::DeterministicRng second(1);
  for (int sample = 0; sample < 1024; ++sample) {
    assert(first.next64() == second.next64());
  }
  bool rejected_zero_seed = false;
  try {
    zircon::sim::DeterministicRng invalid(0);
  } catch (const std::invalid_argument&) {
    rejected_zero_seed = true;
  }
  assert(rejected_zero_seed);

  auto image = zircon::sim::ElfImage::load(argv[1]);
  assert(image.isElf());
  assert(image.entry() == 0x80000000u);
  assert(image.memory().read32(image.entry()) != 0u);
  const auto tohost = image.symbol("tohost");
  const auto fromhost = image.symbol("fromhost");
  assert(tohost.has_value());
  assert(fromhost.has_value());
  assert(*fromhost == *tohost + 4u);

  zircon::sim::TestExitMonitor monitor(*tohost);
  assert(!monitor.observeWrite(*tohost, 0, 0xf).has_value());
  assert(monitor.observeWrite(*tohost, 1, 0xf).value() == 0);

  zircon::sim::TestExitMonitor failure_monitor(*tohost);
  assert(failure_monitor.observeWrite(*tohost, 7, 0xf).value() == 3);

  zircon::sim::SparseMemory memory;
  memory.write32(0x80000000u, 0x00500093u);
  zircon::sim::DeterministicAxiMemory axi(memory, 7, std::nullopt);
  zircon::sim::AxiMasterSignals ar;
  ar.ar_valid = true;
  ar.ar_id = 2;
  ar.ar_addr = 0x80000000u;
  ar.ar_len = 0;
  ar.ar_size = 2;
  ar.ar_burst = 1;
  zircon::sim::AxiSlaveSignals slave;
  for (int cycle = 0; cycle < 32; ++cycle) {
    slave = axi.drive();
    axi.advance(ar, slave);
    if (slave.ar_ready) {
      ar.ar_valid = false;
      break;
    }
  }
  assert(!ar.ar_valid);

  bool observed_read = false;
  for (int cycle = 0; cycle < 32; ++cycle) {
    slave = axi.drive();
    zircon::sim::AxiMasterSignals r;
    r.r_ready = true;
    axi.advance(r, slave);
    if (slave.r_valid) {
      assert(slave.r_id == 2);
      assert(slave.r_data == 0x00500093u);
      assert(slave.r_resp == 0);
      assert(slave.r_last);
      observed_read = true;
      break;
    }
  }
  assert(observed_read);

  std::istringstream zircon_trace(
      "{\"order\":0,\"pc\":2147483648,\"instruction\":5243027,\"privilege\":3,\"gprWrite\":true,\"gprAddress\":1,\"gprData\":5,\"fprWrite\":false,\"csrWrite\":false,\"csrAddress\":0,\"csrData\":0,\"memoryReadMask\":0,\"memoryWriteMask\":0,\"trap\":false,\"interrupt\":false}\n"
      "{\"order\":1,\"pc\":2147483652,\"instruction\":99,\"privilege\":3,\"gprWrite\":false,\"gprAddress\":8,\"gprData\":123,\"fprWrite\":false,\"csrWrite\":false,\"csrAddress\":832,\"csrData\":14,\"memoryReadMask\":0,\"memoryWriteMask\":0,\"trap\":false,\"interrupt\":false}\n");
  std::istringstream spike_log(
      "core   0: 3 0x80000000 (0x00500093) x1  0x00000005\n"
      "core   0: 3 0x80000004 (0x00000063)\n");
  const auto zircon_records = zircon::sim::parseZirconTrace(zircon_trace, 2);
  const auto spike_records = zircon::sim::parseSpikeCommitLog(spike_log, 2);
  zircon::sim::compareCommitPrefixes(zircon_records, spike_records);

  std::cout << "unit-tests: deterministic RNG, ELF32, AXI, symbols, tohost, and commit trace passed" << std::endl;
  return 0;
}
