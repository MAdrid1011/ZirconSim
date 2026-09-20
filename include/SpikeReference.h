#ifndef ZIRCON_SIM_SPIKE_REFERENCE_H
#define ZIRCON_SIM_SPIKE_REFERENCE_H

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "ElfImage.h"
#include "PlatformDevices.h"

namespace zircon::sim {

class CheckpointReader;
class CheckpointWriter;

struct SpikeCommit {
    uint32_t pc = 0;
    uint32_t instruction = 0;
    uint32_t next_pc = 0;
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

struct SpikeTrap {
    uint32_t cause = 0;
    uint32_t epc = 0;
    uint32_t tval = 0;
    uint8_t target_privilege = 3;
};

class SpikeReference {
  public:
    SpikeReference(const SparseMemory &memory, uint32_t entry, bool linuxPlatform = false, std::string uartInput = {});
    ~SpikeReference();

    SpikeReference(const SpikeReference &) = delete;
    SpikeReference &operator=(const SpikeReference &) = delete;

    std::optional<SpikeCommit> next();
    void synchronize(const SpikeArchitecturalState &state);
    void setTime(uint64_t value);
    void appendUartInput(std::string_view input);
    void injectInterrupt(const SpikeTrap &trap);
    void injectPageFault(const SpikeTrap &trap);
    uint32_t integerRegister(size_t index) const;
    std::string diagnosticContext() const;
    void save(CheckpointWriter &writer) const;
    void restore(CheckpointReader &reader);

    static bool requiresSynchronization(uint32_t instruction);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zircon::sim

#endif
