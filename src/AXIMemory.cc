#include "AXIMemory.h"

#include "Checkpoint.h"

#include <iostream>

namespace {
constexpr uint32_t kUartAddress = 0xa00003f8u;
}

AXIMemory::AXIMemory(zircon::sim::SparseMemory &memory, std::optional<uint32_t> tohost, uint64_t seed,
                     zircon::sim::PlatformDevices *platform)
    : memory_(memory), rng_(seed), platform_(platform) {
    if (tohost.has_value()) {
        exit_.emplace(*tohost);
    }
}

bool AXIMemory::randomReady() { return (rng_.next32() & 3u) != 0; }

bool AXIMemory::randomValid() { return (rng_.next32() & 3u) != 0; }

void AXIMemory::drive(VZirconCore &cpu) {
    if (readActive_ && !readDataValid_ && (platform_ != nullptr || randomValid())) {
        readDataValid_ = true;
        readResponse_ = 0;
        const uint32_t address = static_cast<uint32_t>(readAddress_);
        if (platform_ != nullptr && platform_->isDevice(readAddress_, readSize_)) {
            if (readLength_ != 0 || !platform_->readBus(address, readSize_, readData_)) {
                readData_ = 0;
                readResponse_ = 2;
            }
        } else if (platform_ != nullptr && !platform_->isRam(readAddress_, readSize_)) {
            readData_ = 0;
            readResponse_ = 3;
        } else {
            readData_ = memory_.read32(address);
        }
    }

    cpu.io_axi_ar_ready = !readActive_ && (platform_ != nullptr || randomReady());
    cpu.io_axi_r_valid = readDataValid_;
    cpu.io_axi_r_bits_id = readId_;
    cpu.io_axi_r_bits_data = readData_;
    cpu.io_axi_r_bits_resp = readResponse_;
    cpu.io_axi_r_bits_last = readActive_ && readBeat_ == readLength_;

    cpu.io_axi_aw_ready = !writeActive_ && !writeResponseValid_ && (platform_ != nullptr || randomReady());
    cpu.io_axi_w_ready = writeActive_ && (platform_ != nullptr || randomReady());
    cpu.io_axi_b_valid = writeResponseValid_;
    cpu.io_axi_b_bits_id = writeId_;
    cpu.io_axi_b_bits_resp = writeResponse_;
}

std::optional<int> AXIMemory::update(VZirconCore &cpu) {
    if (cpu.io_axi_ar_valid && cpu.io_axi_ar_ready) {
        readActive_ = true;
        readAddress_ = cpu.io_axi_ar_bits_addr;
        readLength_ = cpu.io_axi_ar_bits_len;
        readSize_ = uint64_t{1} << cpu.io_axi_ar_bits_size;
        readId_ = cpu.io_axi_ar_bits_id;
        readBeat_ = 0;
    }

    if (cpu.io_axi_r_valid && cpu.io_axi_r_ready) {
        readDataValid_ = false;
        if (readBeat_ == readLength_) {
            readActive_ = false;
        } else {
            ++readBeat_;
            readAddress_ += readSize_;
        }
    }

    if (cpu.io_axi_aw_valid && cpu.io_axi_aw_ready) {
        writeActive_ = true;
        writeAddress_ = cpu.io_axi_aw_bits_addr;
        writeLength_ = cpu.io_axi_aw_bits_len;
        writeSize_ = uint64_t{1} << cpu.io_axi_aw_bits_size;
        writeId_ = cpu.io_axi_aw_bits_id;
        writeBeat_ = 0;
        writeResponse_ = 0;
    }

    if (cpu.io_axi_w_valid && cpu.io_axi_w_ready) {
        const uint32_t address = static_cast<uint32_t>(writeAddress_);
        const uint32_t data = cpu.io_axi_w_bits_data;
        const uint8_t strobe = cpu.io_axi_w_bits_strb;
        if (platform_ != nullptr && platform_->isDevice(address & ~uint32_t{3}, sizeof(uint32_t))) {
            if (writeLength_ != 0 || !platform_->writeBus(address, data, strobe)) {
                writeResponse_ = 2;
            }
        } else if (platform_ != nullptr && !platform_->isRam(address & ~uint32_t{3}, sizeof(uint32_t))) {
            writeResponse_ = 3;
        } else {
            memory_.write32(address, data, strobe);
        }
        if (platform_ == nullptr && address == kUartAddress && (strobe & 1u) != 0) {
            std::cout.put(static_cast<char>(data & 0xffu));
            std::cout.flush();
        }
        const auto result = exit_.has_value() ? exit_->observeWrite(address, data, strobe) : std::nullopt;

        if (cpu.io_axi_w_bits_last || writeBeat_ == writeLength_) {
            writeActive_ = false;
            writeResponseValid_ = true;
        } else {
            ++writeBeat_;
            writeAddress_ += writeSize_;
        }

        if (result.has_value()) {
            return result;
        }
    }

    if (cpu.io_axi_b_valid && cpu.io_axi_b_ready) {
        writeResponseValid_ = false;
    }
    return std::nullopt;
}

bool AXIMemory::timerInterrupt() const { return platform_ != nullptr && platform_->timerInterrupt(); }

uint64_t AXIMemory::platformTime() const { return platform_ == nullptr ? 0 : platform_->time(); }

void AXIMemory::tick() {
    if (platform_ != nullptr) {
        platform_->tick();
    }
}

void AXIMemory::appendUartInput(std::string_view input) {
    if (platform_ != nullptr) {
        platform_->appendUartInput(input);
    }
}

bool AXIMemory::outputContains(std::string_view marker) const {
    return platform_ != nullptr && platform_->outputContains(marker);
}

void AXIMemory::save(zircon::sim::CheckpointWriter &writer) const {
    writer.write(exit_.has_value());
    writer.write(exit_.has_value() ? exit_->value() : 0u);
    writer.write(rng_.state());
    writer.write(readActive_);
    writer.write(readDataValid_);
    writer.write(readData_);
    writer.write(readAddress_);
    writer.write(readSize_);
    writer.write(readLength_);
    writer.write(readBeat_);
    writer.write(readId_);
    writer.write(readResponse_);
    writer.write(writeActive_);
    writer.write(writeResponseValid_);
    writer.write(writeAddress_);
    writer.write(writeSize_);
    writer.write(writeLength_);
    writer.write(writeBeat_);
    writer.write(writeId_);
    writer.write(writeResponse_);
}

void AXIMemory::restore(zircon::sim::CheckpointReader &reader) {
    const bool hasExit = reader.read<bool>();
    const uint32_t exitValue = reader.read<uint32_t>();
    if (hasExit != exit_.has_value()) {
        throw std::runtime_error("checkpoint tohost configuration does not match the image");
    }
    if (exit_.has_value()) {
        exit_->setValue(exitValue);
    }
    rng_.setState(reader.read<uint64_t>());
    readActive_ = reader.read<bool>();
    readDataValid_ = reader.read<bool>();
    readData_ = reader.read<uint32_t>();
    readAddress_ = reader.read<uint64_t>();
    readSize_ = reader.read<uint64_t>();
    readLength_ = reader.read<uint8_t>();
    readBeat_ = reader.read<uint8_t>();
    readId_ = reader.read<uint8_t>();
    readResponse_ = reader.read<uint8_t>();
    writeActive_ = reader.read<bool>();
    writeResponseValid_ = reader.read<bool>();
    writeAddress_ = reader.read<uint64_t>();
    writeSize_ = reader.read<uint64_t>();
    writeLength_ = reader.read<uint8_t>();
    writeBeat_ = reader.read<uint8_t>();
    writeId_ = reader.read<uint8_t>();
    writeResponse_ = reader.read<uint8_t>();
}
