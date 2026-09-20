#ifndef ZIRCON_SIM_PLATFORM_DEVICES_H
#define ZIRCON_SIM_PLATFORM_DEVICES_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace zircon::sim {

class CheckpointReader;
class CheckpointWriter;

class PlatformDevices {
  public:
    static constexpr uint32_t kRamBase = 0x80000000u;
    static constexpr uint32_t kRamSize = 64u * 1024u * 1024u;
    static constexpr uint32_t kClintBase = 0xa0000000u;
    static constexpr uint32_t kClintSize = 0x00010000u;
    static constexpr uint32_t kUartBase = 0xa1000000u;
    static constexpr uint32_t kUartSize = 0x00001000u;
    static constexpr uint32_t kCpuFrequencyHz = 100000000u;
    static constexpr uint32_t kTimebaseFrequencyHz = 10000000u;
    static constexpr uint32_t kTimerDivider = kCpuFrequencyHz / kTimebaseFrequencyHz;

    explicit PlatformDevices(bool emitUart = true, std::string uartInput = {});

    bool read(uint32_t address, size_t size, uint8_t *bytes);
    bool write(uint32_t address, size_t size, const uint8_t *bytes);
    bool readBus(uint32_t address, size_t size, uint32_t &data);
    bool writeBus(uint32_t address, uint32_t data, uint8_t strobe);

    bool isRam(uint64_t address, size_t size) const;
    bool isDevice(uint64_t address, size_t size) const;
    bool timerInterrupt() const;
    uint64_t time() const;
    void setTime(uint64_t value);
    void tick();
    void appendUartInput(std::string_view input);
    bool outputContains(std::string_view marker) const;
    const std::string &uartOutput() const;
    void save(CheckpointWriter &writer) const;
    void restore(CheckpointReader &reader);

  private:
    bool readByte(uint32_t address, uint8_t &value);
    bool writeByte(uint32_t address, uint8_t value);

    bool emitUart_;
    uint64_t mtime_ = 0;
    uint64_t mtimecmp_ = UINT64_MAX;
    uint32_t timerSubcycle_ = 0;
    uint8_t uartIer_ = 0;
    uint8_t uartLcr_ = 0;
    uint8_t uartMcr_ = 0;
    uint8_t uartDll_ = 1;
    uint8_t uartDlm_ = 0;
    std::deque<uint8_t> uartInput_;
    std::string uartOutput_;
};

} // namespace zircon::sim

#endif
