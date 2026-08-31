#ifndef ZIRCON_SIM_DETERMINISTIC_RNG_H
#define ZIRCON_SIM_DETERMINISTIC_RNG_H

#include <cstdint>
#include <stdexcept>

namespace zircon::sim {

class DeterministicRng {
 public:
  explicit DeterministicRng(uint64_t seed) : state_(seed) {
    if (seed == 0) {
      throw std::invalid_argument("simulation seed must be non-zero");
    }
  }

  uint64_t next64() {
    uint64_t value = state_;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    state_ = value;
    return value * UINT64_C(2685821657736338717);
  }

  uint32_t next32() { return static_cast<uint32_t>(next64() >> 32); }

  bool chance(uint32_t numerator, uint32_t denominator) {
    if (denominator == 0 || numerator > denominator) {
      throw std::invalid_argument("invalid deterministic probability");
    }
    return (next32() % denominator) < numerator;
  }

 private:
  uint64_t state_;
};

}  // namespace zircon::sim

#endif
