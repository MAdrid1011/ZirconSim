#include "DeterministicAxiMemory.h"

#include <stdexcept>
#include <utility>

namespace zircon::sim {
namespace {

constexpr uint8_t kAxiOkay = 0;
constexpr uint8_t kAxiDecodeError = 3;
constexpr uint8_t kAxiBurstIncrementing = 1;

}  // namespace

DeterministicAxiMemory::DeterministicAxiMemory(
    SparseMemory memory, uint64_t seed, std::optional<uint32_t> tohost_address)
    : memory_(std::move(memory)), rng_(seed) {
  if (tohost_address.has_value()) {
    exit_monitor_.emplace(*tohost_address);
  }
}

bool DeterministicAxiMemory::acceptThisCycle() {
  // The probability is fixed and the sequence is fully determined by --seed.
  return rng_.chance(9, 10);
}

uint32_t DeterministicAxiMemory::transferStride(uint8_t size) {
  if (size > 2) {
    return 0;
  }
  return 1u << size;
}

bool DeterministicAxiMemory::validIncrementingBurst(
    uint32_t address, uint8_t length, uint8_t size, uint8_t burst) {
  const uint32_t stride = transferStride(size);
  if (stride == 0 || burst != kAxiBurstIncrementing ||
      (address & (stride - 1u)) != 0) {
    return false;
  }
  const uint64_t bytes = static_cast<uint64_t>(length + 1u) * stride;
  return (address & ~uint32_t{0xfff}) ==
         ((address + static_cast<uint32_t>(bytes - 1u)) & ~uint32_t{0xfff});
}

AxiSlaveSignals DeterministicAxiMemory::drive() {
  AxiSlaveSignals slave;
  if (read_state_ == ReadState::Idle) {
    slave.ar_ready = acceptThisCycle();
  } else if (read_delay_ == 0) {
    slave.r_valid = true;
    slave.r_id = read_id_;
    slave.r_data = read_response_ == kAxiOkay ? memory_.read32(read_address_) : 0;
    slave.r_resp = read_response_;
    slave.r_last = read_remaining_ == 1;
  }

  switch (write_state_) {
    case WriteState::Address:
      slave.aw_ready = acceptThisCycle();
      break;
    case WriteState::Data:
      slave.w_ready = acceptThisCycle();
      break;
    case WriteState::Response:
      if (write_delay_ == 0) {
        slave.b_valid = true;
        slave.b_id = write_id_;
        slave.b_resp = write_response_;
      }
      break;
  }
  return slave;
}

void DeterministicAxiMemory::advance(const AxiMasterSignals& master,
                                     const AxiSlaveSignals& slave) {
  if (read_state_ == ReadState::Idle) {
    if (master.ar_valid && slave.ar_ready) {
      read_state_ = ReadState::Respond;
      read_id_ = master.ar_id;
      read_address_ = master.ar_addr;
      read_remaining_ = static_cast<uint32_t>(master.ar_len) + 1u;
      read_stride_ = transferStride(master.ar_size);
      read_response_ = validIncrementingBurst(master.ar_addr, master.ar_len,
                                               master.ar_size, master.ar_burst)
                           ? kAxiOkay
                           : kAxiDecodeError;
      read_delay_ = rng_.next32() % 4u;
    }
  } else if (read_delay_ != 0) {
    --read_delay_;
  } else if (slave.r_valid && master.r_ready) {
    if (read_remaining_ == 0) {
      throw std::logic_error("AXI read underflow");
    }
    --read_remaining_;
    if (read_remaining_ == 0) {
      read_state_ = ReadState::Idle;
    } else {
      read_address_ += read_stride_;
    }
  }

  switch (write_state_) {
    case WriteState::Address:
      if (master.aw_valid && slave.aw_ready) {
        write_state_ = WriteState::Data;
        write_id_ = master.aw_id;
        write_address_ = master.aw_addr;
        write_remaining_ = static_cast<uint32_t>(master.aw_len) + 1u;
        write_stride_ = transferStride(master.aw_size);
        write_response_ = validIncrementingBurst(master.aw_addr, master.aw_len,
                                                  master.aw_size, master.aw_burst)
                              ? kAxiOkay
                              : kAxiDecodeError;
      }
      break;
    case WriteState::Data:
      if (master.w_valid && slave.w_ready) {
        const bool last = write_remaining_ == 1;
        if (master.w_last != last) {
          write_response_ = kAxiDecodeError;
        }
        if (write_response_ == kAxiOkay) {
          memory_.write32(write_address_, master.w_data, master.w_strb);
          if (exit_monitor_.has_value() && !exit_status_.has_value()) {
            exit_status_ = exit_monitor_->observeWrite(write_address_, master.w_data,
                                                        master.w_strb);
          }
        }
        if (write_remaining_ == 0) {
          throw std::logic_error("AXI write underflow");
        }
        --write_remaining_;
        if (write_remaining_ == 0) {
          write_state_ = WriteState::Response;
          write_delay_ = rng_.next32() % 4u;
        } else {
          write_address_ += write_stride_;
        }
      }
      break;
    case WriteState::Response:
      if (write_delay_ != 0) {
        --write_delay_;
      } else if (slave.b_valid && master.b_ready) {
        write_state_ = WriteState::Address;
      }
      break;
  }
}

}  // namespace zircon::sim
