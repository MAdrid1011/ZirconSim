#include "AXIMemory.h"

#include <iostream>

namespace {
constexpr uint32_t kUartAddress = 0xa00003f8u;
}

AXIMemory::AXIMemory(zircon::sim::SparseMemory& memory, uint32_t tohost, uint64_t seed)
    : memory_(memory), exit_(tohost), rng_(seed) {}

bool AXIMemory::randomReady() {
    return (rng_.next32() & 3u) != 0;
}

bool AXIMemory::randomValid() {
    return (rng_.next32() & 3u) != 0;
}

void AXIMemory::drive(VZirconCore& cpu) {
    if (readActive_ && !readDataValid_ && randomValid()) {
        readDataValid_ = true;
        readData_ = memory_.read32(static_cast<uint32_t>(readAddress_));
    }

    cpu.io_axi_ar_ready = !readActive_ && randomReady();
    cpu.io_axi_r_valid = readDataValid_;
    cpu.io_axi_r_bits_id = readId_;
    cpu.io_axi_r_bits_data = readData_;
    cpu.io_axi_r_bits_resp = 0;
    cpu.io_axi_r_bits_last = readActive_ && readBeat_ == readLength_;

    cpu.io_axi_aw_ready = !writeActive_ && !writeResponseValid_ && randomReady();
    cpu.io_axi_w_ready = writeActive_ && randomReady();
    cpu.io_axi_b_valid = writeResponseValid_;
    cpu.io_axi_b_bits_id = writeId_;
    cpu.io_axi_b_bits_resp = 0;
}

std::optional<int> AXIMemory::update(VZirconCore& cpu) {
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
    }

    if (cpu.io_axi_w_valid && cpu.io_axi_w_ready) {
        const uint32_t address = static_cast<uint32_t>(writeAddress_);
        const uint32_t data = cpu.io_axi_w_bits_data;
        const uint8_t strobe = cpu.io_axi_w_bits_strb;
        memory_.write32(address, data, strobe);
        if (address == kUartAddress && (strobe & 1u) != 0) {
            std::cout.put(static_cast<char>(data & 0xffu));
            std::cout.flush();
        }
        const auto result = exit_.observeWrite(address, data, strobe);

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
