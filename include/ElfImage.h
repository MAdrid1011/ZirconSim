#ifndef ZIRCON_SIM_ELF_IMAGE_H
#define ZIRCON_SIM_ELF_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace zircon::sim {

class SparseMemory {
 public:
  uint8_t read8(uint32_t address) const;
  uint32_t read32(uint32_t address) const;
  void write8(uint32_t address, uint8_t value);
  void write32(uint32_t address, uint32_t value, uint8_t strobe = 0xf);
  void load(uint32_t address, const uint8_t* data, size_t size);

 private:
  std::unordered_map<uint32_t, uint8_t> bytes_;
};

class ElfImage {
 public:
  static ElfImage load(const std::string& path, uint32_t raw_base = 0x80000000u);

  uint32_t entry() const { return entry_; }
  bool isElf() const { return is_elf_; }
  const SparseMemory& memory() const { return memory_; }
  SparseMemory& memory() { return memory_; }
  std::optional<uint32_t> symbol(const std::string& name) const;

 private:
  uint32_t entry_ = 0x80000000u;
  bool is_elf_ = false;
  SparseMemory memory_;
  std::unordered_map<std::string, uint32_t> symbols_;
};

class TestExitMonitor {
 public:
  explicit TestExitMonitor(uint32_t tohost_address) : tohost_address_(tohost_address) {}

  std::optional<int> observeWrite(uint32_t address, uint32_t data, uint8_t strobe);
  std::optional<int> observeBackingMemory(const SparseMemory& memory) const;

 private:
  uint32_t tohost_address_;
  uint32_t value_ = 0;
};

}  // namespace zircon::sim

#endif
