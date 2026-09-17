#ifndef ZIRCON_SIM_SPIKE_REFERENCE_H
#define ZIRCON_SIM_SPIKE_REFERENCE_H

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace zircon::sim {

struct SpikeCommit {
    uint32_t pc = 0;
    uint32_t instruction = 0;
    bool write_valid = false;
    bool is_fp = false;
    uint8_t rd = 0;
    uint32_t value = 0;
};

std::optional<SpikeCommit> parseSpikeCommitLine(const std::string &line);

class SpikeReference {
  public:
    SpikeReference(const std::string &spike, const std::string &elf, uint32_t entry);
    ~SpikeReference();

    SpikeReference(const SpikeReference &) = delete;
    SpikeReference &operator=(const SpikeReference &) = delete;

    std::optional<SpikeCommit> next();

  private:
    FILE *stream_ = nullptr;
    uint32_t entry_ = 0;
    bool reached_entry_ = false;
    std::string last_line_;
};

} // namespace zircon::sim

#endif
