#ifndef AXIMEMORY_HH
#define AXIMEMORY_HH

#include <cstdint>
#include <optional>

#include "DeterministicRng.h"
#include "ElfImage.h"
#include "VZirconCore.h"

class AXIMemory {
  public:
    AXIMemory(zircon::sim::SparseMemory& memory, uint32_t tohost, uint64_t seed);

    void drive(VZirconCore& cpu);
    std::optional<int> update(VZirconCore& cpu);

  private:
    bool randomReady();
    bool randomValid();

    zircon::sim::SparseMemory& memory_;
    zircon::sim::TestExitMonitor exit_;
    zircon::sim::DeterministicRng rng_;

    bool readActive_ = false;
    bool readDataValid_ = false;
    uint32_t readData_ = 0;
    uint64_t readAddress_ = 0;
    uint64_t readSize_ = 4;
    uint8_t readLength_ = 0;
    uint8_t readBeat_ = 0;
    uint8_t readId_ = 0;

    bool writeActive_ = false;
    bool writeResponseValid_ = false;
    uint64_t writeAddress_ = 0;
    uint64_t writeSize_ = 4;
    uint8_t writeLength_ = 0;
    uint8_t writeBeat_ = 0;
    uint8_t writeId_ = 0;

};

#endif
