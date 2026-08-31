#ifndef ZIRCON_SIM_DETERMINISTIC_AXI_MEMORY_H
#define ZIRCON_SIM_DETERMINISTIC_AXI_MEMORY_H

#include <cstdint>
#include <optional>

#include "DeterministicRng.h"
#include "ElfImage.h"

namespace zircon::sim {

struct AxiMasterSignals {
  bool aw_valid = false;
  uint8_t aw_id = 0;
  uint32_t aw_addr = 0;
  uint8_t aw_len = 0;
  uint8_t aw_size = 0;
  uint8_t aw_burst = 0;
  bool w_valid = false;
  uint32_t w_data = 0;
  uint8_t w_strb = 0;
  bool w_last = false;
  bool b_ready = false;
  bool ar_valid = false;
  uint8_t ar_id = 0;
  uint32_t ar_addr = 0;
  uint8_t ar_len = 0;
  uint8_t ar_size = 0;
  uint8_t ar_burst = 0;
  bool r_ready = false;
};

struct AxiSlaveSignals {
  bool aw_ready = false;
  bool w_ready = false;
  bool b_valid = false;
  uint8_t b_id = 0;
  uint8_t b_resp = 0;
  bool ar_ready = false;
  bool r_valid = false;
  uint8_t r_id = 0;
  uint32_t r_data = 0;
  uint8_t r_resp = 0;
  bool r_last = false;
};

/** Deterministic AXI4 slave used by the Verilator harness.
 *
 * One read and one write burst may be outstanding. The model uses the explicit
 * non-zero seed only to decide legal backpressure and response latency; once a
 * valid response is asserted it remains stable until handshake.
 */
class DeterministicAxiMemory {
 public:
  DeterministicAxiMemory(SparseMemory memory, uint64_t seed,
                         std::optional<uint32_t> tohost_address);

  AxiSlaveSignals drive();
  void advance(const AxiMasterSignals& master, const AxiSlaveSignals& slave);

  const SparseMemory& memory() const { return memory_; }
  std::optional<int> exitStatus() const { return exit_status_; }

 private:
  enum class ReadState { Idle, Respond };
  enum class WriteState { Address, Data, Response };

  SparseMemory memory_;
  DeterministicRng rng_;
  std::optional<TestExitMonitor> exit_monitor_;
  std::optional<int> exit_status_;

  ReadState read_state_ = ReadState::Idle;
  uint8_t read_id_ = 0;
  uint32_t read_address_ = 0;
  uint32_t read_remaining_ = 0;
  uint32_t read_stride_ = 4;
  uint32_t read_delay_ = 0;
  uint8_t read_response_ = 0;

  WriteState write_state_ = WriteState::Address;
  uint8_t write_id_ = 0;
  uint32_t write_address_ = 0;
  uint32_t write_remaining_ = 0;
  uint32_t write_stride_ = 4;
  uint32_t write_delay_ = 0;
  uint8_t write_response_ = 0;

  bool acceptThisCycle();
  static uint32_t transferStride(uint8_t size);
  static bool validIncrementingBurst(uint32_t address, uint8_t length,
                                     uint8_t size, uint8_t burst);
};

}  // namespace zircon::sim

#endif
