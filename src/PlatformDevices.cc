#include "PlatformDevices.h"

#include "Checkpoint.h"

#include <iostream>
#include <limits>

namespace zircon::sim {
namespace {

bool contains(uint64_t address, size_t size, uint32_t base, uint32_t length) {
    return size != 0 && address >= base && address + size <= uint64_t{base} + length &&
           address + size <= uint64_t{1} << 32;
}

} // namespace

PlatformDevices::PlatformDevices(bool emitUart, std::string uartInput) : emitUart_(emitUart) {
    appendUartInput(uartInput);
}

bool PlatformDevices::isRam(uint64_t address, size_t size) const { return contains(address, size, kRamBase, kRamSize); }

bool PlatformDevices::isDevice(uint64_t address, size_t size) const {
    return contains(address, size, kClintBase, kClintSize) || contains(address, size, kUartBase, kUartSize);
}

bool PlatformDevices::readByte(uint32_t address, uint8_t &value) {
    if (address >= kClintBase && address < kClintBase + kClintSize) {
        const uint32_t offset = address - kClintBase;
        if (offset >= 0x4000u && offset < 0x4008u) {
            value = static_cast<uint8_t>(mtimecmp_ >> ((offset - 0x4000u) * 8));
            return true;
        }
        if (offset >= 0xbff8u && offset < 0xc000u) {
            value = static_cast<uint8_t>(mtime_ >> ((offset - 0xbff8u) * 8));
            return true;
        }
        return false;
    }
    if (address < kUartBase || address >= kUartBase + kUartSize) {
        return false;
    }

    switch (address - kUartBase) {
    case 0:
        if ((uartLcr_ & 0x80u) != 0) {
            value = uartDll_;
        } else if (uartInput_.empty()) {
            value = 0;
        } else {
            value = uartInput_.front();
            uartInput_.pop_front();
        }
        return true;
    case 1:
        value = (uartLcr_ & 0x80u) != 0 ? uartDlm_ : uartIer_;
        return true;
    case 2:
        value = 0x01u;
        return true;
    case 3:
        value = uartLcr_;
        return true;
    case 4:
        value = uartMcr_;
        return true;
    case 5:
        value = static_cast<uint8_t>(0x60u | (uartInput_.empty() ? 0u : 1u));
        return true;
    default:
        value = 0;
        return true;
    }
}

bool PlatformDevices::writeByte(uint32_t address, uint8_t value) {
    if (address >= kClintBase && address < kClintBase + kClintSize) {
        const uint32_t offset = address - kClintBase;
        if (offset >= 0x4000u && offset < 0x4008u) {
            const unsigned shift = (offset - 0x4000u) * 8;
            mtimecmp_ = (mtimecmp_ & ~(uint64_t{0xff} << shift)) | (uint64_t{value} << shift);
            return true;
        }
        if (offset >= 0xbff8u && offset < 0xc000u) {
            const unsigned shift = (offset - 0xbff8u) * 8;
            mtime_ = (mtime_ & ~(uint64_t{0xff} << shift)) | (uint64_t{value} << shift);
            return true;
        }
        return false;
    }
    if (address < kUartBase || address >= kUartBase + kUartSize) {
        return false;
    }

    switch (address - kUartBase) {
    case 0:
        if ((uartLcr_ & 0x80u) != 0) {
            uartDll_ = value;
        } else {
            uartOutput_.push_back(static_cast<char>(value));
            if (emitUart_) {
                std::cout.put(static_cast<char>(value));
                std::cout.flush();
            }
        }
        return true;
    case 1:
        if ((uartLcr_ & 0x80u) != 0) {
            uartDlm_ = value;
        } else {
            uartIer_ = value;
        }
        return true;
    case 2:
        return true;
    case 3:
        uartLcr_ = value;
        return true;
    case 4:
        uartMcr_ = value;
        return true;
    default:
        return true;
    }
}

bool PlatformDevices::read(uint32_t address, size_t size, uint8_t *bytes) {
    if (bytes == nullptr || !isDevice(address, size)) {
        return false;
    }
    for (size_t index = 0; index < size; ++index) {
        if (!readByte(address + static_cast<uint32_t>(index), bytes[index])) {
            return false;
        }
    }
    return true;
}

bool PlatformDevices::write(uint32_t address, size_t size, const uint8_t *bytes) {
    if (bytes == nullptr || !isDevice(address, size)) {
        return false;
    }
    for (size_t index = 0; index < size; ++index) {
        if (!writeByte(address + static_cast<uint32_t>(index), bytes[index])) {
            return false;
        }
    }
    return true;
}

bool PlatformDevices::readBus(uint32_t address, size_t size, uint32_t &data) {
    if (size == 0 || size > sizeof(uint32_t) || (address & (size - 1)) != 0 || !isDevice(address, size)) {
        return false;
    }
    data = 0;
    for (size_t index = 0; index < size; ++index) {
        uint8_t value = 0;
        if (!readByte(address + static_cast<uint32_t>(index), value)) {
            return false;
        }
        const unsigned lane = (address + static_cast<uint32_t>(index)) & 3u;
        data |= uint32_t{value} << (lane * 8);
    }
    return true;
}

bool PlatformDevices::writeBus(uint32_t address, uint32_t data, uint8_t strobe) {
    const uint32_t alignedAddress = address & ~uint32_t{3};
    bool wrote = false;
    for (unsigned lane = 0; lane < 4; ++lane) {
        if ((strobe & (1u << lane)) == 0) {
            continue;
        }
        wrote = true;
        if (!writeByte(alignedAddress + lane, static_cast<uint8_t>(data >> (lane * 8)))) {
            return false;
        }
    }
    return wrote;
}

bool PlatformDevices::timerInterrupt() const { return mtime_ >= mtimecmp_; }

uint64_t PlatformDevices::time() const { return mtime_; }

void PlatformDevices::setTime(uint64_t value) { mtime_ = value; }

void PlatformDevices::tick() {
    static_assert(kCpuFrequencyHz % kTimebaseFrequencyHz == 0);
    if (++timerSubcycle_ == kTimerDivider) {
        timerSubcycle_ = 0;
        if (mtime_ != std::numeric_limits<uint64_t>::max()) {
            ++mtime_;
        }
    }
}

void PlatformDevices::appendUartInput(std::string_view input) {
    for (const char character : input) {
        uartInput_.push_back(static_cast<uint8_t>(character));
    }
}

bool PlatformDevices::outputContains(std::string_view marker) const {
    return !marker.empty() && uartOutput_.find(marker) != std::string::npos;
}

const std::string &PlatformDevices::uartOutput() const { return uartOutput_; }

void PlatformDevices::save(CheckpointWriter &writer) const {
    writer.write(mtime_);
    writer.write(mtimecmp_);
    writer.write(timerSubcycle_);
    writer.write(uartIer_);
    writer.write(uartLcr_);
    writer.write(uartMcr_);
    writer.write(uartDll_);
    writer.write(uartDlm_);
    writer.write<uint64_t>(uartInput_.size());
    for (const uint8_t value : uartInput_) {
        writer.write(value);
    }
    writer.writeString(uartOutput_);
}

void PlatformDevices::restore(CheckpointReader &reader) {
    mtime_ = reader.read<uint64_t>();
    mtimecmp_ = reader.read<uint64_t>();
    timerSubcycle_ = reader.read<uint32_t>();
    if (timerSubcycle_ >= kTimerDivider) {
        throw std::runtime_error("checkpoint timer subcycle is invalid");
    }
    uartIer_ = reader.read<uint8_t>();
    uartLcr_ = reader.read<uint8_t>();
    uartMcr_ = reader.read<uint8_t>();
    uartDll_ = reader.read<uint8_t>();
    uartDlm_ = reader.read<uint8_t>();
    const uint64_t inputSize = reader.readCount(1 << 20, "UART input");
    uartInput_.clear();
    for (uint64_t index = 0; index < inputSize; ++index) {
        uartInput_.push_back(reader.read<uint8_t>());
    }
    uartOutput_ = reader.readString(64 << 20);
}

} // namespace zircon::sim
