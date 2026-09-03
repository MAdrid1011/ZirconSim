#include <cassert>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

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

  // M3 owns four physical demand IDs. Exercise all owners concurrently and
  // require cross-ID interleaving without disturbing per-owner beat order.
  zircon::sim::SparseMemory multi_memory;
  for (uint32_t index = 0; index < 4; ++index) {
    multi_memory.write32(0x80000100u + index * 0x100u, 0x10000000u + index * 2u);
    multi_memory.write32(0x80000104u + index * 0x100u, 0x10000001u + index * 2u);
  }
  zircon::sim::DeterministicAxiMemory multi_axi(multi_memory, 19);
  for (uint8_t id = 1; id <= 4; ++id) {
    zircon::sim::AxiMasterSignals request;
    request.ar_valid = true;
    request.ar_id = id;
    request.ar_addr = 0x80000100u + static_cast<uint32_t>(id - 1) * 0x100u;
    request.ar_len = 1;
    request.ar_size = 2;
    request.ar_burst = 1;
    bool accepted = false;
    for (int cycle = 0; cycle < 64 && !accepted; ++cycle) {
      const auto offered = multi_axi.drive();
      multi_axi.advance(request, offered);
      accepted = offered.ar_ready;
    }
    assert(accepted);
  }

  std::set<uint8_t> response_ids;
  std::vector<uint8_t> response_order;
  std::map<uint8_t, uint32_t> response_beats;
  std::map<uint8_t, uint32_t> response_last_data;
  uint32_t response_count = 0;
  for (int cycle = 0; cycle < 256 && response_count < 8; ++cycle) {
    const auto offered = multi_axi.drive();
    zircon::sim::AxiMasterSignals ready;
    ready.r_ready = true;
    if (offered.r_valid) {
      response_ids.insert(offered.r_id);
      response_order.push_back(offered.r_id);
      ++response_beats[offered.r_id];
      ++response_count;
      if (offered.r_last) response_last_data[offered.r_id] = offered.r_data;
      assert(offered.r_resp == 0);
    }
    multi_axi.advance(ready, offered);
  }
  assert(response_order.size() == 8);
  assert(std::set<uint8_t>(response_order.begin(), response_order.begin() + 4).size() >= 2);
  assert(response_ids == std::set<uint8_t>({1, 2, 3, 4}));
  for (uint8_t id = 1; id <= 4; ++id) {
    assert(response_beats[id] == 2);
    assert(response_last_data[id] == 0x10000001u + static_cast<uint32_t>(id - 1) * 2u);
  }

  zircon::sim::SparseMemory ordered_memory;
  ordered_memory.write32(0x80001000u, 0xaaaa0001u);
  ordered_memory.write32(0x80001004u, 0xbbbb0002u);
  zircon::sim::DeterministicAxiMemory ordered_axi(ordered_memory, 23);
  for (uint32_t address : {0x80001000u, 0x80001004u}) {
    zircon::sim::AxiMasterSignals request;
    request.ar_valid = true;
    request.ar_id = 3;
    request.ar_addr = address;
    request.ar_len = 0;
    request.ar_size = 2;
    request.ar_burst = 1;
    bool accepted = false;
    for (int cycle = 0; cycle < 64 && !accepted; ++cycle) {
      const auto offered = ordered_axi.drive();
      ordered_axi.advance(request, offered);
      accepted = offered.ar_ready;
    }
    assert(accepted);
  }
  std::vector<uint32_t> same_id_data;
  for (int cycle = 0; cycle < 128 && same_id_data.size() < 2; ++cycle) {
    const auto offered = ordered_axi.drive();
    zircon::sim::AxiMasterSignals ready;
    ready.r_ready = true;
    if (offered.r_valid) {
      assert(offered.r_id == 3);
      assert(offered.r_last);
      same_id_data.push_back(offered.r_data);
    }
    ordered_axi.advance(ready, offered);
  }
  assert(same_id_data == std::vector<uint32_t>({0xaaaa0001u, 0xbbbb0002u}));

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

  std::istringstream sail_memory_log(
      "mem[X,0x080000000] -> 0x00002083\n"
      "[0] [M]: 0x80000000 (0x00002083) lw x1, 0x0(x0)\n"
      "mem[R,0x080001000] -> 0x11223344\n"
      "x1 <- 0x11223344\n"
      "mem[X,0x080000004] -> 0x001000a3\n"
      "[1] [M]: 0x80000004 (0x001000a3) sb x1, 0x1(x0)\n"
      "mem[W,0x080001001] <- 0xAB\n"
      "mem[X,0x080000008] -> 0x0000202f\n"
      "[2] [M]: 0x80000008 (0x0000202f) amoadd.w x0, x0, (x0)\n"
      "mem[RW,0x080001000] -> 0x1122ab44\n"
      "mem[RW,0x080001000] <- 0xcafebabe\n");
  const auto sail_memory_records = zircon::sim::parseSailCommitLog(sail_memory_log, 3);
  assert(sail_memory_records[0].memory.address == 0x80001000u);
  assert(sail_memory_records[0].memory.read_mask == 0xf);
  assert(sail_memory_records[0].memory.read_data == 0x11223344u);
  assert(sail_memory_records[1].memory.address == 0x80001001u);
  assert(sail_memory_records[1].memory.write_mask == 0x2);
  assert(sail_memory_records[1].memory.write_data == 0x0000ab00u);
  assert(sail_memory_records[2].memory.read_mask == 0xf);
  assert(sail_memory_records[2].memory.write_mask == 0xf);
  assert(sail_memory_records[2].memory.write_data == 0xcafebabeu);

  std::istringstream orphan_sail_memory(
      "mem[W,0x080001000] <- 0x00000001\n");
  bool rejected_orphan_sail_memory = false;
  try {
    static_cast<void>(zircon::sim::parseSailCommitLog(orphan_sail_memory, 1));
  } catch (const std::runtime_error&) {
    rejected_orphan_sail_memory = true;
  }
  assert(rejected_orphan_sail_memory);

  std::istringstream invalid_sail_memory(
      "[0] [M]: 0x80000000 (0x00002083) lw x1, 0x0(x0)\n"
      "mem[R,0x080001000] <- 0x11223344\n");
  bool rejected_invalid_sail_memory = false;
  try {
    static_cast<void>(zircon::sim::parseSailCommitLog(invalid_sail_memory, 1));
  } catch (const std::runtime_error&) {
    rejected_invalid_sail_memory = true;
  }
  assert(rejected_invalid_sail_memory);

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
