#include <cassert>
#include <iomanip>
#include <iostream>
#include <sstream>
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

  zircon::sim::SparseMemory backing_memory;
  zircon::sim::TestExitMonitor backing_monitor(*tohost);
  backing_memory.write32(*tohost, 0, 0xf);
  assert(!backing_monitor.observeBackingMemory(backing_memory).has_value());
  backing_memory.write32(*tohost, 1, 0xf);
  assert(backing_monitor.observeBackingMemory(backing_memory).value() == 0);

  zircon::sim::SparseMemory memory;
  memory.write32(0x80000000u, 0x00500093u);
  zircon::sim::DeterministicAxiMemory axi(memory, 7);
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
      "{\"order\":0,\"pc\":2147483648,\"instruction\":5243027,\"privilege\":3,\"gprWrite\":true,\"gprAddress\":1,\"gprData\":5,\"fprWrite\":false,\"csrWrite\":false,\"csrAddress\":0,\"csrData\":0,\"memoryAddress\":0,\"memoryReadMask\":0,\"memoryWriteMask\":0,\"memoryReadData\":0,\"memoryWriteData\":0,\"trap\":false,\"interrupt\":false}\n"
      "{\"order\":1,\"pc\":2147483652,\"instruction\":99,\"privilege\":3,\"gprWrite\":false,\"gprAddress\":8,\"gprData\":123,\"fprWrite\":false,\"csrWrite\":false,\"csrAddress\":832,\"csrData\":14,\"memoryAddress\":0,\"memoryReadMask\":0,\"memoryWriteMask\":0,\"memoryReadData\":0,\"memoryWriteData\":0,\"trap\":false,\"interrupt\":false}\n");
  std::istringstream spike_log(
      "core   0: 3 0x80000000 (0x00500093) x1  0x00000005\n"
      "core   0: 3 0x80000004 (0x00000063)\n");
  const auto zircon_records = zircon::sim::parseZirconTrace(zircon_trace, 2);
  const auto spike_records = zircon::sim::parseSpikeCommitLog(spike_log, 2);
  zircon::sim::compareCommitPrefixes(zircon_records, spike_records);

  std::istringstream sail_log(
      "[0] [M]: 0x80000000 (0x00500093) addi x1, x0, 0x5\n"
      "x1 <- 0x00000005\n"
      "CSR mip (0x344) <- 0x00000080\n"
      "[1] [M]: 0x80000004 (0x34009173) csrrw x2, mscratch, x1\n"
      "x2 <- 0x00000000\n"
      "CSR mscratch (0x340) <- 0x00000005\n");
  const auto sail_records = zircon::sim::parseSailCommitLog(sail_log, 2);
  assert(sail_records.size() == 2);
  assert(sail_records[0].privilege == 3);
  assert(sail_records[0].gpr_write && sail_records[0].gpr_address == 1);
  assert(!sail_records[0].csr_write);
  assert(sail_records[1].gpr_write && sail_records[1].gpr_address == 2);
  assert(sail_records[1].csr_write && sail_records[1].csr_address == 0x340);
  assert(sail_records[1].csr_data == 5);

  std::istringstream zircon_memory_trace(
      "{\"order\":0,\"pc\":2147483648,\"instruction\":8323,\"privilege\":3,\"gprWrite\":true,\"gprAddress\":1,\"gprData\":287454020,\"fprWrite\":false,\"csrWrite\":false,\"csrAddress\":0,\"csrData\":0,\"memoryAddress\":2147487744,\"memoryReadMask\":15,\"memoryWriteMask\":0,\"memoryReadData\":287454020,\"memoryWriteData\":0,\"trap\":false,\"interrupt\":false}\n"
      "{\"order\":1,\"pc\":2147483652,\"instruction\":1048739,\"privilege\":3,\"gprWrite\":false,\"gprAddress\":0,\"gprData\":0,\"fprWrite\":false,\"csrWrite\":false,\"csrAddress\":0,\"csrData\":0,\"memoryAddress\":2147487745,\"memoryReadMask\":0,\"memoryWriteMask\":2,\"memoryReadData\":0,\"memoryWriteData\":43776,\"trap\":false,\"interrupt\":false}\n"
      "{\"order\":2,\"pc\":2147483656,\"instruction\":8239,\"privilege\":3,\"gprWrite\":false,\"gprAddress\":0,\"gprData\":0,\"fprWrite\":false,\"csrWrite\":false,\"csrAddress\":0,\"csrData\":0,\"memoryAddress\":2147487744,\"memoryReadMask\":15,\"memoryWriteMask\":15,\"memoryReadData\":287484740,\"memoryWriteData\":3405691582,\"trap\":false,\"interrupt\":false}\n");
  std::istringstream spike_memory_log(
      "core   0: 3 0x80000000 (0x00002083) x1  0x11223344 mem 0x80001000\n"
      "core   0: 3 0x80000004 (0x001000a3) mem 0x80001001 0xab\n"
      "core   0: 3 0x80000008 (0x0000202f) mem 0x80001000 mem 0x80001000 0xcafebabe\n");
  const auto zircon_memory_records = zircon::sim::parseZirconTrace(zircon_memory_trace, 3);
  const auto spike_memory_records = zircon::sim::parseSpikeCommitLog(spike_memory_log, 3);
  zircon::sim::SparseMemory initial_memory;
  initial_memory.write32(0x80001000u, 0x11223344u);
  zircon::sim::SparseMemory memory_snapshot = initial_memory;
  memory_snapshot.write32(0x80001001u, 0x0000ab00u, 0x2);
  memory_snapshot.write32(0x80001000u, 0xcafebabeu);
  bool rejected_memory_prefix = false;
  try {
    zircon::sim::compareCommitPrefixes(zircon_memory_records, spike_memory_records);
  } catch (const std::runtime_error&) {
    rejected_memory_prefix = true;
  }
  assert(rejected_memory_prefix);
  bool rejected_stale_backing = false;
  try {
    zircon::sim::compareCommittedMemory(zircon_memory_records, spike_memory_records,
                                        initial_memory, initial_memory);
  } catch (const std::runtime_error&) {
    rejected_stale_backing = true;
  }
  assert(rejected_stale_backing);
  zircon::sim::compareCommittedMemory(zircon_memory_records, spike_memory_records,
                                      initial_memory, memory_snapshot);

  std::stringstream snapshot;
  for (const auto& [address, value] : memory_snapshot.snapshot()) {
    snapshot << std::hex << std::setfill('0') << std::setw(8) << address << " "
             << std::setw(2) << static_cast<unsigned>(value) << "\n";
  }
  const auto parsed_backing = zircon::sim::parseBackingMemorySnapshot(snapshot);
  assert(parsed_backing.read32(0x80001000u) == 0xcafebabeu);

  std::cout << "unit-tests: deterministic RNG, ELF32, AXI, symbols, tohost backing memory, and commit trace parsers passed" << std::endl;
  return 0;
}
