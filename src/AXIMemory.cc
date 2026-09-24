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

bool AXIMemory::randomReady() { return (rng_.next32() & 15u) != 0; }

bool AXIMemory::randomValid() { return (rng_.next32() & 15u) != 0; }

void AXIMemory::drive(VZirconCore &cpu) {
    const bool demandRead = reads_[0].active;
    const bool prefetchRead = reads_[1].active;
    if ((demandRead || prefetchRead) && !readDataValid_ && (reads_[demandRead ? 0 : 1].beat != 0 || randomValid())) {
        readSelectedId_ = demandRead ? 0 : 1;
        const auto &read = reads_[readSelectedId_];
        readDataValid_ = true;
        readResponse_ = 0;
        const uint32_t address = static_cast<uint32_t>(read.address);
        if (platform_ != nullptr && platform_->isDevice(read.address, read.size)) {
            uint32_t deviceData = 0;
            if (read.length != 0 || !platform_->readBus(address, read.size, deviceData)) {
                readData_ = 0;
                readResponse_ = 2;
            } else {
                readData_ = static_cast<uint64_t>(deviceData) << ((address & 4u) * 8);
            }
        } else if (platform_ != nullptr && !platform_->isRam(read.address, read.size)) {
            readData_ = 0;
            readResponse_ = 3;
        } else {
            readData_ = memory_.read64(address & ~uint32_t{7});
        }
    }

    const uint8_t requestReadId = cpu.io_axi_ar_bits_id;
    cpu.io_axi_ar_ready = requestReadId < reads_.size() && !reads_[requestReadId].active && randomReady();
    cpu.io_axi_r_valid = readDataValid_;
    cpu.io_axi_r_bits_id = readSelectedId_;
    cpu.io_axi_r_bits_data = readData_;
    cpu.io_axi_r_bits_resp = readResponse_;
    cpu.io_axi_r_bits_last = readDataValid_ && reads_[readSelectedId_].beat == reads_[readSelectedId_].length;

    cpu.io_axi_aw_ready = !writeActive_ && !writeResponseValid_ && randomReady();
    cpu.io_axi_w_ready = writeActive_ && (writeBeat_ != 0 || randomReady());
    cpu.io_axi_b_valid = writeResponseValid_;
    cpu.io_axi_b_bits_id = writeId_;
    cpu.io_axi_b_bits_resp = writeResponse_;
}

std::optional<int> AXIMemory::update(VZirconCore &cpu) {
    if (cpu.io_axi_ar_valid && cpu.io_axi_ar_ready) {
        auto &read = reads_[cpu.io_axi_ar_bits_id];
        read.active = true;
        read.address = cpu.io_axi_ar_bits_addr;
        read.length = cpu.io_axi_ar_bits_len;
        read.size = uint64_t{1} << cpu.io_axi_ar_bits_size;
        read.beat = 0;
    }

    if (cpu.io_axi_r_valid && cpu.io_axi_r_ready) {
        auto &read = reads_[readSelectedId_];
        readDataValid_ = false;
        if (read.beat == read.length) {
            read.active = false;
        } else {
            ++read.beat;
            read.address += read.size;
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
        const uint64_t data = cpu.io_axi_w_bits_data;
        const uint8_t strobe = cpu.io_axi_w_bits_strb;
        const unsigned laneShift = (address & 4u) * 8;
        const uint32_t narrowData = static_cast<uint32_t>(data >> laneShift);
        const uint8_t narrowStrobe = static_cast<uint8_t>((strobe >> (laneShift / 8)) & 0xfu);
        if (platform_ != nullptr && platform_->isDevice(address, writeSize_)) {
            if (writeLength_ != 0 || !platform_->writeBus(address, narrowData, narrowStrobe)) {
                writeResponse_ = 2;
            }
        } else if (platform_ != nullptr && !platform_->isRam(address, writeSize_)) {
            writeResponse_ = 3;
        } else {
            memory_.write64(address & ~uint32_t{7}, data, strobe);
        }
        if (platform_ == nullptr && address == kUartAddress && (narrowStrobe & 1u) != 0) {
            std::cout.put(static_cast<char>(narrowData & 0xffu));
            std::cout.flush();
        }
        const auto result = exit_.has_value() ? exit_->observeWrite(address, narrowData, narrowStrobe) : std::nullopt;

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
    writer.write(readDataValid_);
    writer.write(readData_);
    writer.write(readSelectedId_);
    writer.write(readResponse_);
    for (const auto &read : reads_) {
        writer.write(read.active);
        writer.write(read.address);
        writer.write(read.size);
        writer.write(read.length);
        writer.write(read.beat);
    }
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
    readDataValid_ = reader.read<bool>();
    readData_ = reader.read<uint64_t>();
    readSelectedId_ = reader.read<uint8_t>();
    readResponse_ = reader.read<uint8_t>();
    for (auto &read : reads_) {
        read.active = reader.read<bool>();
        read.address = reader.read<uint64_t>();
        read.size = reader.read<uint64_t>();
        read.length = reader.read<uint8_t>();
        read.beat = reader.read<uint8_t>();
    }
    writeActive_ = reader.read<bool>();
    writeResponseValid_ = reader.read<bool>();
    writeAddress_ = reader.read<uint64_t>();
    writeSize_ = reader.read<uint64_t>();
    writeLength_ = reader.read<uint8_t>();
    writeBeat_ = reader.read<uint8_t>();
    writeId_ = reader.read<uint8_t>();
    writeResponse_ = reader.read<uint8_t>();
}
