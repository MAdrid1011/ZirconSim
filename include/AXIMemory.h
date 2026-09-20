#ifndef AXIMEMORY_HH
#define AXIMEMORY_HH

#include <cstdint>
#include <optional>

#include "DeterministicRng.h"
#include "ElfImage.h"
#include "PlatformDevices.h"
#include "VZirconCore.h"

namespace zircon::sim {
class CheckpointReader;
class CheckpointWriter;
} // namespace zircon::sim

class AXIMemory {
  public:
    AXIMemory(zircon::sim::SparseMemory &memory, std::optional<uint32_t> tohost, uint64_t seed,
              zircon::sim::PlatformDevices *platform = nullptr);

    void drive(VZirconCore &cpu);
    std::optional<int> update(VZirconCore &cpu);
    bool timerInterrupt() const;
    uint64_t platformTime() const;
    void tick();
    void appendUartInput(std::string_view input);
    bool outputContains(std::string_view marker) const;
    void save(zircon::sim::CheckpointWriter &writer) const;
    void restore(zircon::sim::CheckpointReader &reader);

  private:
    bool randomReady();
    bool randomValid();

    zircon::sim::SparseMemory &memory_;
    std::optional<zircon::sim::TestExitMonitor> exit_;
    zircon::sim::DeterministicRng rng_;
    zircon::sim::PlatformDevices *platform_;

    bool readActive_ = false;
    bool readDataValid_ = false;
    uint32_t readData_ = 0;
    uint64_t readAddress_ = 0;
    uint64_t readSize_ = 4;
    uint8_t readLength_ = 0;
    uint8_t readBeat_ = 0;
    uint8_t readId_ = 0;
    uint8_t readResponse_ = 0;

    bool writeActive_ = false;
    bool writeResponseValid_ = false;
    uint64_t writeAddress_ = 0;
    uint64_t writeSize_ = 4;
    uint8_t writeLength_ = 0;
    uint8_t writeBeat_ = 0;
    uint8_t writeId_ = 0;
    uint8_t writeResponse_ = 0;
};

#endif
