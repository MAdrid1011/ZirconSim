#ifndef ZIRCON_SIM_COMMIT_TRACE_H
#define ZIRCON_SIM_COMMIT_TRACE_H

#include <cstddef>
#include <cstdint>
#include <istream>
#include <vector>

#include "ElfImage.h"

namespace zircon::sim {

struct CommitMemoryRecord {
  uint32_t address = 0;
  uint8_t read_mask = 0;
  uint8_t write_mask = 0;
  uint32_t read_data = 0;
  uint32_t write_data = 0;

  bool active() const { return read_mask != 0 || write_mask != 0; }
};

struct CommitRecord {
  uint64_t order = 0;
  uint8_t privilege = 0;
  uint32_t pc = 0;
  uint32_t instruction = 0;
  bool gpr_write = false;
  uint8_t gpr_address = 0;
  uint32_t gpr_data = 0;
  bool csr_write = false;
  uint16_t csr_address = 0;
  uint32_t csr_data = 0;
  CommitMemoryRecord memory;
};

std::vector<CommitRecord> parseZirconTrace(std::istream& input,
                                           size_t max_records);
std::vector<CommitRecord> parseSpikeCommitLog(std::istream& input,
                                              size_t max_records);
std::vector<CommitRecord> parseSailCommitLog(std::istream& input,
                                             size_t max_records);
void compareCommitPrefixes(const std::vector<CommitRecord>& zircon,
                           const std::vector<CommitRecord>& reference);
SparseMemory parseBackingMemorySnapshot(std::istream& input);
void compareCommittedMemory(const std::vector<CommitRecord>& zircon,
                            const std::vector<CommitRecord>& reference,
                            SparseMemory initial_memory,
                            const SparseMemory& backing_memory);

}  // namespace zircon::sim

#endif
