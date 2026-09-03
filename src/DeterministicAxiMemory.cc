#include "DeterministicAxiMemory.h"

#include <stdexcept>
#include <utility>

namespace zircon::sim {
namespace {

constexpr uint8_t kAxiOkay = 0;
constexpr uint8_t kAxiDecodeError = 3;
constexpr uint8_t kAxiBurstIncrementing = 1;

}  // namespace

DeterministicAxiMemory::DeterministicAxiMemory(SparseMemory memory, uint64_t seed)
    : memory_(std::move(memory)), rng_(seed) {}

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
  for (size_t index = 0; index < kMaxReadOwners; ++index) {
    if (!read_owners_[index].valid) {
      slave.ar_ready = acceptThisCycle();
      break;
    }
  }

  // Keep the selected beat stable while the core backpressures R. Once the
  // offer is consumed, choose another ready owner using the seeded RNG.
  if (read_offer_ < 0 || !read_owners_[static_cast<size_t>(read_offer_)].valid ||
      read_owners_[static_cast<size_t>(read_offer_)].delay != 0) {
    read_offer_ = -1;
    for (size_t index = 0; index < kMaxReadOwners; ++index) {
      if (read_owners_[index].valid && read_owners_[index].delay == 0) {
        if (read_offer_ < 0 || acceptThisCycle()) {
          read_offer_ = static_cast<int>(index);
        }
      }
    }
  }
  if (read_offer_ >= 0) {
    const auto& owner = read_owners_[static_cast<size_t>(read_offer_)];
    slave.r_valid = true;
    slave.r_id = owner.id;
    slave.r_data = owner.response == kAxiOkay ? memory_.read32(owner.address) : 0;
    slave.r_resp = owner.response;
    slave.r_last = owner.remaining == 1;
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
  int accepted_read = -1;
  if (master.ar_valid && slave.ar_ready) {
    for (size_t index = 0; index < kMaxReadOwners; ++index) {
      if (!read_owners_[index].valid) {
        accepted_read = static_cast<int>(index);
        break;
      }
    }
    if (accepted_read < 0) {
      throw std::logic_error("AXI read accepted without a free owner");
    }
    auto& owner = read_owners_[static_cast<size_t>(accepted_read)];
    owner.valid = true;
    owner.id = master.ar_id;
    owner.address = master.ar_addr;
    owner.remaining = static_cast<uint32_t>(master.ar_len) + 1u;
    owner.stride = transferStride(master.ar_size);
    owner.response = validIncrementingBurst(master.ar_addr, master.ar_len,
                                            master.ar_size, master.ar_burst)
                         ? kAxiOkay
                         : kAxiDecodeError;
    owner.delay = rng_.next32() % 4u;
    owner.sequence = next_read_sequence_++;
  }

  for (auto& owner : read_owners_) {
    if (owner.valid && owner.delay != 0 &&
        static_cast<int>(&owner - read_owners_.data()) != accepted_read) {
      --owner.delay;
    }
  }

  if (slave.r_valid && master.r_ready) {
    if (read_offer_ < 0 || !read_owners_[static_cast<size_t>(read_offer_)].valid) {
      throw std::logic_error("AXI read response accepted without a live owner");
    }
    auto& owner = read_owners_[static_cast<size_t>(read_offer_)];
    if (owner.remaining == 0) {
      throw std::logic_error("AXI read underflow");
    }
    --owner.remaining;
    if (owner.remaining == 0) {
      owner.valid = false;
      read_offer_ = -1;
    } else {
      owner.address += owner.stride;
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
