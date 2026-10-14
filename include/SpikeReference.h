#ifndef ZIRCON_SIM_SPIKE_REFERENCE_H
#define ZIRCON_SIM_SPIKE_REFERENCE_H

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ElfImage.h"
#include "PlatformDevices.h"

namespace zircon::sim {

class CheckpointReader;
class CheckpointWriter;

struct SpikeVectorState {
    uint32_t vl = 0;
    uint32_t vtype = 0;
    uint32_t vstart = 0;
    uint8_t vxrm = 0;
    bool vxsat = false;
    uint8_t fflags = 0;
};

struct SpikeVectorRegister {
    uint8_t index = 0;
    std::array<uint32_t, 8> words{};
};

struct SpikeVectorCommit {
    SpikeVectorState before;
    SpikeVectorState after;
    std::array<uint32_t, 8> mask{};
    std::array<uint32_t, 8> selectionMask{};
    std::vector<SpikeVectorRegister> registers;
    std::vector<std::pair<uint32_t, uint8_t>> stores;
};

struct SpikeCommit {
    uint32_t pc = 0;
    uint32_t instruction = 0;
    uint32_t next_pc = 0;
    bool write_valid = false;
    bool is_fp = false;
    uint8_t rd = 0;
    uint32_t value = 0;
    std::optional<SpikeVectorCommit> vector;
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
    void vectorRegister(size_t index, const std::array<uint32_t, 8> &words);
    std::array<uint32_t, 8> vectorRegister(size_t index) const;
    SpikeVectorState vectorState() const;
    uint8_t memoryByte(uint32_t address) const;
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
