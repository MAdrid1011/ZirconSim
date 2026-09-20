#ifndef ZIRCON_SIM_CHECKPOINT_H
#define ZIRCON_SIM_CHECKPOINT_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace zircon::sim {

class CheckpointWriter {
  public:
    explicit CheckpointWriter(const std::filesystem::path &path);

    template <typename T> void write(const T &value) {
        static_assert(std::is_trivially_copyable_v<T>);
        stream_.write(reinterpret_cast<const char *>(&value), sizeof(value));
        requireGood();
    }

    void writeBytes(const void *data, size_t size);
    void writeString(const std::string &value);
    void close();

  private:
    void requireGood() const;

    std::ofstream stream_;
};

class CheckpointReader {
  public:
    explicit CheckpointReader(const std::filesystem::path &path);

    template <typename T> T read() {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        stream_.read(reinterpret_cast<char *>(&value), sizeof(value));
        requireGood();
        return value;
    }

    void readBytes(void *data, size_t size);
    std::string readString(size_t maximumSize);
    uint64_t readCount(uint64_t maximum, const char *name);
    void requireEnd();

  private:
    void requireGood() const;

    std::ifstream stream_;
};

uint64_t fileFingerprint(const std::filesystem::path &path);

} // namespace zircon::sim

#endif
