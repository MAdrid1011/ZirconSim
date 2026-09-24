#ifndef AXIMEMORY_HH
#define AXIMEMORY_HH

#include <cstdint>
#include <array>
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
    struct ReadTransaction {
        bool active = false;
        uint64_t address = 0;
        uint64_t size = 4;
        uint8_t length = 0;
        uint8_t beat = 0;
    };

    bool randomReady();
    bool randomValid();

    zircon::sim::SparseMemory &memory_;
    std::optional<zircon::sim::TestExitMonitor> exit_;
    zircon::sim::DeterministicRng rng_;
    zircon::sim::PlatformDevices *platform_;

    std::array<ReadTransaction, 2> reads_{};
    bool readDataValid_ = false;
    uint64_t readData_ = 0;
    uint8_t readSelectedId_ = 0;
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
