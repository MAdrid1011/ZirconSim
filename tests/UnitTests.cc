#include <cassert>
#include <iostream>
#include <stdexcept>

#include "DeterministicRng.h"
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

  std::cout << "unit-tests: deterministic RNG, ELF32, symbols, and tohost passed" << std::endl;
  return 0;
}
