#include <cassert>
#include <iostream>
#include <stdexcept>

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

  std::cout << "unit-tests: deterministic RNG, ELF32, AXI, symbols, and tohost passed" << std::endl;
  return 0;
}
