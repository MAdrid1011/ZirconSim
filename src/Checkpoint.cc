#include "Checkpoint.h"

#include <array>

namespace zircon::sim {

CheckpointWriter::CheckpointWriter(const std::filesystem::path &path)
    : stream_(path, std::ios::binary | std::ios::trunc) {
    if (!stream_) {
        throw std::runtime_error("cannot create checkpoint state: " + path.string());
    }
}

void CheckpointWriter::writeBytes(const void *data, size_t size) {
    stream_.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    requireGood();
}

void CheckpointWriter::writeString(const std::string &value) {
    write<uint64_t>(value.size());
    writeBytes(value.data(), value.size());
}

void CheckpointWriter::close() {
    stream_.flush();
    requireGood();
    stream_.close();
}

void CheckpointWriter::requireGood() const {
    if (!stream_) {
        throw std::runtime_error("failed to write checkpoint state");
    }
}

CheckpointReader::CheckpointReader(const std::filesystem::path &path) : stream_(path, std::ios::binary) {
    if (!stream_) {
        throw std::runtime_error("cannot open checkpoint state: " + path.string());
    }
}

void CheckpointReader::readBytes(void *data, size_t size) {
    stream_.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
    requireGood();
}

std::string CheckpointReader::readString(size_t maximumSize) {
    const uint64_t size = readCount(maximumSize, "string");
    std::string value(static_cast<size_t>(size), '\0');
    readBytes(value.data(), value.size());
    return value;
}

uint64_t CheckpointReader::readCount(uint64_t maximum, const char *name) {
    const uint64_t value = read<uint64_t>();
    if (value > maximum) {
        throw std::runtime_error(std::string("checkpoint ") + name + " count exceeds its limit");
    }
    return value;
}

void CheckpointReader::requireEnd() {
    std::array<char, 1> extra{};
    stream_.read(extra.data(), extra.size());
    if (!stream_.eof()) {
        throw std::runtime_error("checkpoint state contains trailing data");
    }
}

void CheckpointReader::requireGood() const {
    if (!stream_) {
        throw std::runtime_error("truncated checkpoint state");
    }
}

uint64_t fileFingerprint(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot fingerprint image: " + path.string());
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    std::array<char, 64 * 1024> buffer{};
    while (stream) {
        stream.read(buffer.data(), buffer.size());
        const std::streamsize count = stream.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<uint8_t>(buffer[static_cast<size_t>(index)]);
            hash *= UINT64_C(1099511628211);
        }
    }
    if (!stream.eof()) {
        throw std::runtime_error("failed to fingerprint image: " + path.string());
    }
    return hash;
}

} // namespace zircon::sim
