#include "ElfImage.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace zircon::sim {
namespace {

constexpr uint8_t kElfClass32 = 1;
constexpr uint8_t kElfDataLittleEndian = 1;
constexpr uint16_t kMachineRiscV = 243;
constexpr uint32_t kProgramLoad = 1;
constexpr uint32_t kSectionSymbolTable = 2;

#pragma pack(push, 1)
struct Elf32Header {
  std::array<uint8_t, 16> ident;
  uint16_t type;
  uint16_t machine;
  uint32_t version;
  uint32_t entry;
  uint32_t program_header_offset;
  uint32_t section_header_offset;
  uint32_t flags;
  uint16_t header_size;
  uint16_t program_header_entry_size;
  uint16_t program_header_count;
  uint16_t section_header_entry_size;
  uint16_t section_header_count;
  uint16_t section_name_index;
};

struct Elf32ProgramHeader {
  uint32_t type;
  uint32_t offset;
  uint32_t virtual_address;
  uint32_t physical_address;
  uint32_t file_size;
  uint32_t memory_size;
  uint32_t flags;
  uint32_t align;
};

struct Elf32SectionHeader {
  uint32_t name;
  uint32_t type;
  uint32_t flags;
  uint32_t address;
  uint32_t offset;
  uint32_t size;
  uint32_t link;
  uint32_t info;
  uint32_t address_align;
  uint32_t entry_size;
};

struct Elf32Symbol {
  uint32_t name;
  uint32_t value;
  uint32_t size;
  uint8_t info;
  uint8_t other;
  uint16_t section_index;
};
#pragma pack(pop)

template <typename T>
T objectAt(const std::vector<uint8_t>& file, size_t offset) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (offset > file.size() || sizeof(T) > file.size() - offset) {
    throw std::runtime_error("truncated ELF structure");
  }
  T value;
  std::memcpy(&value, file.data() + offset, sizeof(T));
  return value;
}

void requireRange(const std::vector<uint8_t>& file, size_t offset, size_t size) {
  if (offset > file.size() || size > file.size() - offset) {
    throw std::runtime_error("ELF range exceeds file size");
  }
}

std::string readString(const std::vector<uint8_t>& file, size_t base, size_t size, uint32_t offset) {
  if (offset >= size) {
    throw std::runtime_error("ELF string offset is outside its table");
  }
  requireRange(file, base, size);
  const char* first = reinterpret_cast<const char*>(file.data() + base + offset);
  const char* limit = reinterpret_cast<const char*>(file.data() + base + size);
  const char* end = std::find(first, limit, '\0');
  if (end == limit) {
    throw std::runtime_error("unterminated ELF string");
  }
  return std::string(first, end);
}

}  // namespace

uint8_t SparseMemory::read8(uint32_t address) const {
  const auto value = bytes_.find(address);
  return value == bytes_.end() ? 0 : value->second;
}

uint32_t SparseMemory::read32(uint32_t address) const {
  uint32_t value = 0;
  for (uint32_t byte = 0; byte < 4; ++byte) {
    value |= static_cast<uint32_t>(read8(address + byte)) << (byte * 8);
  }
  return value;
}

void SparseMemory::write8(uint32_t address, uint8_t value) { bytes_[address] = value; }

void SparseMemory::write32(uint32_t address, uint32_t value, uint8_t strobe) {
  for (uint32_t byte = 0; byte < 4; ++byte) {
    if ((strobe & (1u << byte)) != 0) {
      write8(address + byte, static_cast<uint8_t>(value >> (byte * 8)));
    }
  }
}

void SparseMemory::load(uint32_t address, const uint8_t* data, size_t size) {
  for (size_t byte = 0; byte < size; ++byte) {
    write8(address + static_cast<uint32_t>(byte), data[byte]);
  }
}

ElfImage ElfImage::load(const std::string& path, uint32_t raw_base) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot open image: " + path);
  }
  const std::vector<uint8_t> file{
      std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  if (file.empty()) {
    throw std::runtime_error("image is empty: " + path);
  }

  ElfImage image;
  const bool magic = file.size() >= 4 && file[0] == 0x7f && file[1] == 'E' &&
                     file[2] == 'L' && file[3] == 'F';
  if (!magic) {
    image.memory_.load(raw_base, file.data(), file.size());
    image.entry_ = raw_base;
    return image;
  }

  const auto& header = objectAt<Elf32Header>(file, 0);
  if (header.ident[4] != kElfClass32 || header.ident[5] != kElfDataLittleEndian ||
      header.machine != kMachineRiscV) {
    throw std::runtime_error("ELF is not little-endian RV32");
  }
  if (header.program_header_entry_size != sizeof(Elf32ProgramHeader) ||
      header.section_header_entry_size != sizeof(Elf32SectionHeader)) {
    throw std::runtime_error("unexpected ELF32 table entry size");
  }

  image.is_elf_ = true;
  image.entry_ = header.entry;

  for (uint16_t index = 0; index < header.program_header_count; ++index) {
    const size_t offset = header.program_header_offset +
                          static_cast<size_t>(index) * header.program_header_entry_size;
    const auto& program = objectAt<Elf32ProgramHeader>(file, offset);
    if (program.type != kProgramLoad) {
      continue;
    }
    requireRange(file, program.offset, program.file_size);
    const uint32_t address = program.physical_address != 0 ? program.physical_address
                                                           : program.virtual_address;
    image.memory_.load(address, file.data() + program.offset, program.file_size);
    for (uint32_t byte = program.file_size; byte < program.memory_size; ++byte) {
      image.memory_.write8(address + byte, 0);
    }
  }

  std::vector<Elf32SectionHeader> sections;
  sections.reserve(header.section_header_count);
  for (uint16_t index = 0; index < header.section_header_count; ++index) {
    const size_t offset = header.section_header_offset +
                          static_cast<size_t>(index) * header.section_header_entry_size;
    sections.push_back(objectAt<Elf32SectionHeader>(file, offset));
  }

  for (const auto& section : sections) {
    if (section.type != kSectionSymbolTable || section.entry_size != sizeof(Elf32Symbol)) {
      continue;
    }
    if (section.link >= sections.size()) {
      throw std::runtime_error("ELF symbol table has an invalid string table link");
    }
    const auto& strings = sections[section.link];
    requireRange(file, section.offset, section.size);
    const size_t count = section.size / section.entry_size;
    for (size_t index = 0; index < count; ++index) {
      const auto& symbol = objectAt<Elf32Symbol>(file, section.offset + index * section.entry_size);
      if (symbol.name == 0) {
        continue;
      }
      const std::string name = readString(file, strings.offset, strings.size, symbol.name);
      image.symbols_.insert_or_assign(name, symbol.value);
    }
  }

  return image;
}

std::optional<uint32_t> ElfImage::symbol(const std::string& name) const {
  const auto value = symbols_.find(name);
  return value == symbols_.end() ? std::nullopt : std::optional<uint32_t>(value->second);
}

std::optional<int> TestExitMonitor::observeWrite(uint32_t address, uint32_t data, uint8_t strobe) {
  const uint32_t aligned_host = tohost_address_ & ~uint32_t{3};
  const uint32_t aligned_write = address & ~uint32_t{3};
  if (aligned_host != aligned_write) {
    return std::nullopt;
  }
  const uint32_t byte_offset = address & 3u;
  for (uint32_t byte = 0; byte < 4; ++byte) {
    if ((strobe & (1u << byte)) != 0 && byte + byte_offset < 4) {
      const uint32_t mask = 0xffu << ((byte + byte_offset) * 8);
      value_ = (value_ & ~mask) |
               (((data >> (byte * 8)) & 0xffu) << ((byte + byte_offset) * 8));
    }
  }
  if ((value_ & 1u) == 0 || value_ == 0) {
    return std::nullopt;
  }
  return value_ == 1u ? std::optional<int>(0)
                      : std::optional<int>(static_cast<int>(value_ >> 1));
}

}  // namespace zircon::sim
