#ifndef ZIRCON_SIM_COMMIT_TRACE_H
#define ZIRCON_SIM_COMMIT_TRACE_H

#include <cstddef>
#include <cstdint>
#include <istream>
#include <vector>

namespace zircon::sim {

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
};

std::vector<CommitRecord> parseZirconTrace(std::istream& input,
                                           size_t max_records);
std::vector<CommitRecord> parseSpikeCommitLog(std::istream& input,
                                              size_t max_records);
std::vector<CommitRecord> parseSailCommitLog(std::istream& input,
                                             size_t max_records);
void compareCommitPrefixes(const std::vector<CommitRecord>& zircon,
                           const std::vector<CommitRecord>& reference);

}  // namespace zircon::sim

#endif
