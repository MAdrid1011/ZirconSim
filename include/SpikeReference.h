#ifndef ZIRCON_SIM_SPIKE_REFERENCE_H
#define ZIRCON_SIM_SPIKE_REFERENCE_H

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "ElfImage.h"

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

struct SpikeArchitecturalState {
    std::array<uint32_t, 32> integer{};
    std::array<uint32_t, 32> floating{};
};

class SpikeReference {
  public:
    SpikeReference(const SparseMemory &memory, uint32_t entry);
    ~SpikeReference();

    SpikeReference(const SpikeReference &) = delete;
    SpikeReference &operator=(const SpikeReference &) = delete;

    std::optional<SpikeCommit> next();
    void synchronize(const SpikeArchitecturalState &state);

    static bool requiresSynchronization(uint32_t instruction);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zircon::sim

#endif
