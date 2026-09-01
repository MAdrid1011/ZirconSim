#include "CommitTrace.h"

#include <charconv>
#include <cstdint>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>

namespace zircon::sim {
namespace {

std::string field(const std::string& line, const char* name) {
  const std::string key = std::string("\"") + name + "\":";
  const size_t begin = line.find(key);
  if (begin == std::string::npos) {
    throw std::runtime_error(std::string("retire trace is missing field ") + name);
  }
  const size_t value_begin = begin + key.size();
  const size_t value_end = line.find_first_of(",}", value_begin);
  if (value_end == std::string::npos) {
    throw std::runtime_error(std::string("retire trace has unterminated field ") + name);
  }
  return line.substr(value_begin, value_end - value_begin);
}

uint64_t decimal(const std::string& value, const char* name) {
  uint64_t parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw std::runtime_error(std::string("invalid decimal ") + name);
  }
  return parsed;
}

uint64_t hexadecimal(const std::string& value, const char* name) {
  uint64_t parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed, 16);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw std::runtime_error(std::string("invalid hexadecimal ") + name);
  }
  return parsed;
}

uint32_t hexadecimal32(const std::string& value, const char* name) {
  const uint64_t parsed = hexadecimal(value, name);
  if (parsed > UINT32_MAX) {
    throw std::runtime_error(std::string(name) + " does not fit in RV32");
  }
  return static_cast<uint32_t>(parsed);
}

bool boolean(const std::string& value, const char* name) {
  if (value == "true") return true;
  if (value == "false") return false;
  throw std::runtime_error(std::string("invalid boolean ") + name);
}

CommitRecord zirconRecord(const std::string& line) {
  if (boolean(field(line, "trap"), "trap") ||
      boolean(field(line, "interrupt"), "interrupt") ||
      boolean(field(line, "fprWrite"), "fprWrite")) {
    throw std::runtime_error("commit comparison only accepts normal integer retirements");
  }

  CommitRecord record;
  record.order = decimal(field(line, "order"), "order");
  record.pc = static_cast<uint32_t>(decimal(field(line, "pc"), "pc"));
  record.instruction = static_cast<uint32_t>(decimal(field(line, "instruction"), "instruction"));
  record.privilege = static_cast<uint8_t>(decimal(field(line, "privilege"), "privilege"));
  record.gpr_write = boolean(field(line, "gprWrite"), "gprWrite");
  record.gpr_address = static_cast<uint8_t>(decimal(field(line, "gprAddress"), "gprAddress"));
  record.gpr_data = static_cast<uint32_t>(decimal(field(line, "gprData"), "gprData"));
  record.csr_write = boolean(field(line, "csrWrite"), "csrWrite");
  record.csr_address = static_cast<uint16_t>(decimal(field(line, "csrAddress"), "csrAddress"));
  record.csr_data = static_cast<uint32_t>(decimal(field(line, "csrData"), "csrData"));
  record.memory.address = static_cast<uint32_t>(decimal(field(line, "memoryAddress"),
                                                         "memoryAddress"));
  record.memory.read_mask = static_cast<uint8_t>(decimal(field(line, "memoryReadMask"),
                                                           "memoryReadMask"));
  record.memory.write_mask = static_cast<uint8_t>(decimal(field(line, "memoryWriteMask"),
                                                            "memoryWriteMask"));
  record.memory.read_data = static_cast<uint32_t>(decimal(field(line, "memoryReadData"),
                                                           "memoryReadData"));
  record.memory.write_data = static_cast<uint32_t>(decimal(field(line, "memoryWriteData"),
                                                            "memoryWriteData"));
  if (record.memory.read_mask > 0xf || record.memory.write_mask > 0xf) {
    throw std::runtime_error("Zircon memory mask exceeds one RV32 word");
  }
  return record;
}

uint8_t sailPrivilege(const std::string& value) {
  if (value == "M") return 3;
  if (value == "S") return 1;
  if (value == "U") return 0;
  throw std::runtime_error("unknown Sail privilege");
}

bool isCsrInstruction(uint32_t instruction) {
  return (instruction & 0x7fu) == 0x73u &&
    ((instruction >> 12u) & 0x7u) != 0;
}

uint8_t accessMask(uint32_t address, uint8_t bytes, const char* name) {
  const uint8_t offset = static_cast<uint8_t>(address & 3u);
  if (bytes == 0 || bytes > 4 || offset + bytes > 4) {
    throw std::runtime_error(std::string(name) + " crosses an RV32 word");
  }
  return static_cast<uint8_t>(((1u << bytes) - 1u) << offset);
}

uint8_t loadBytes(uint32_t instruction) {
  const uint8_t opcode = static_cast<uint8_t>(instruction & 0x7fu);
  const uint8_t funct3 = static_cast<uint8_t>((instruction >> 12u) & 0x7u);
  if (opcode == 0x03u) {
    switch (funct3) {
      case 0x0u:
      case 0x4u:
        return 1;
      case 0x1u:
      case 0x5u:
        return 2;
      case 0x2u:
        return 4;
      default:
        break;
    }
  }
  if (opcode == 0x2fu && funct3 == 0x2u) {
    return 4;
  }
  throw std::runtime_error("Spike commit log has a load with an unsupported RV32 encoding");
}

void parseSpikeMemory(const std::string& effects, CommitRecord& record) {
  const std::regex memory(R"(\smem\s+0x([0-9a-fA-F]+)(?:\s+0x([0-9a-fA-F]+))?)");
  for (std::sregex_iterator current(effects.begin(), effects.end(), memory), end;
       current != end; ++current) {
    const std::smatch& match = *current;
    const uint32_t address = hexadecimal32(match[1].str(), "Spike memory address");
    if (!match[2].matched) {
      if (record.memory.read_mask != 0) {
        throw std::runtime_error("Spike instruction has multiple memory reads");
      }
      record.memory.address = address;
      record.memory.read_mask = accessMask(address, loadBytes(record.instruction),
                                           "Spike memory read");
      continue;
    }

    if (record.memory.write_mask != 0) {
      throw std::runtime_error("Spike instruction has multiple memory writes");
    }
    const std::string data = match[2].str();
    const size_t digits = data.size();
    if (digits == 0 || (digits & 1u) != 0 || digits > 8) {
      throw std::runtime_error("Spike store data has an invalid byte width");
    }
    const uint8_t bytes = static_cast<uint8_t>(digits / 2);
    const uint8_t mask = accessMask(address, bytes, "Spike memory write");
    if (record.memory.read_mask != 0 && record.memory.address != address) {
      throw std::runtime_error("Spike atomic read and write addresses differ");
    }
    record.memory.address = address;
    record.memory.write_mask = mask;
    record.memory.write_data = hexadecimal32(data, "Spike store data") <<
      ((address & 3u) * 8u);
  }
}

void compareArchitecturalRecord(const CommitRecord& actual, const CommitRecord& expected,
                                size_t index) {
  const bool gpr_mismatch = actual.gpr_write != expected.gpr_write ||
    (actual.gpr_write && (actual.gpr_address != expected.gpr_address ||
      actual.gpr_data != expected.gpr_data));
  const bool csr_mismatch = actual.csr_write != expected.csr_write ||
    (actual.csr_write && (actual.csr_address != expected.csr_address ||
      actual.csr_data != expected.csr_data));
  if (actual.privilege != expected.privilege || actual.pc != expected.pc ||
      actual.instruction != expected.instruction || gpr_mismatch || csr_mismatch) {
    throw std::runtime_error("Zircon and reference commit records differ at order " +
                             std::to_string(index));
  }
}

CommitMemoryRecord expectedMemory(const CommitRecord& reference, const SparseMemory& memory) {
  CommitMemoryRecord result = reference.memory;
  if (result.read_mask != 0) {
    result.read_data = memory.read32(result.address & ~uint32_t{3});
  }
  return result;
}

void compareMemoryRecord(const CommitMemoryRecord& actual,
                         const CommitMemoryRecord& expected, size_t index) {
  if (actual.address != expected.address || actual.read_mask != expected.read_mask ||
      actual.write_mask != expected.write_mask || actual.read_data != expected.read_data ||
      actual.write_data != expected.write_data) {
    throw std::runtime_error("Zircon and reference memory metadata differ at order " +
                             std::to_string(index));
  }
}

}  // namespace

std::vector<CommitRecord> parseZirconTrace(std::istream& input, size_t max_records) {
  std::vector<CommitRecord> records;
  std::string line;
  while (records.size() < max_records && std::getline(input, line)) {
    if (line.empty()) continue;
    CommitRecord record = zirconRecord(line);
    if (record.order != records.size()) {
      throw std::runtime_error("Zircon retire trace order is not a contiguous prefix");
    }
    records.push_back(record);
  }
  return records;
}

std::vector<CommitRecord> parseSpikeCommitLog(std::istream& input, size_t max_records) {
  const std::regex instruction(
      R"(^core\s+0:\s+([0-9]+)\s+0x([0-9a-fA-F]+)\s+\(0x([0-9a-fA-F]+)\)(.*)$)");
  const std::regex gpr(R"(\sx\s*([0-9]+)\s+0x([0-9a-fA-F]+))");
  const std::regex csr(R"(\sc([0-9]+)_[^ ]+\s+0x([0-9a-fA-F]+))");

  std::vector<CommitRecord> records;
  std::string line;
  std::smatch match;
  while (records.size() < max_records && std::getline(input, line)) {
    if (!std::regex_match(line, match, instruction)) continue;
    CommitRecord record;
    record.order = records.size();
    record.privilege = static_cast<uint8_t>(decimal(match[1].str(), "Spike privilege"));
    record.pc = static_cast<uint32_t>(hexadecimal(match[2].str(), "Spike PC"));
    record.instruction = static_cast<uint32_t>(hexadecimal(match[3].str(), "Spike instruction"));
    const std::string effects = match[4].str();
    std::smatch effect;
    if (std::regex_search(effects, effect, gpr)) {
      record.gpr_write = true;
      record.gpr_address = static_cast<uint8_t>(decimal(effect[1].str(), "Spike GPR address"));
      record.gpr_data = static_cast<uint32_t>(hexadecimal(effect[2].str(), "Spike GPR data"));
    }
    if (std::regex_search(effects, effect, csr)) {
      record.csr_write = true;
      record.csr_address = static_cast<uint16_t>(decimal(effect[1].str(), "Spike CSR address"));
      record.csr_data = static_cast<uint32_t>(hexadecimal(effect[2].str(), "Spike CSR data"));
    }
    parseSpikeMemory(effects, record);
    records.push_back(record);
  }
  return records;
}

std::vector<CommitRecord> parseSailCommitLog(std::istream& input, size_t max_records) {
  const std::regex instruction(
      R"(^\[([0-9]+)\] \[([MSU])\]: 0x([0-9a-fA-F]+) \(0x([0-9a-fA-F]+)\).*$)");
  const std::regex gpr(R"(^x([0-9]+) <- 0x([0-9a-fA-F]+)$)");
  const std::regex csr(R"(^CSR .+ \(0x([0-9a-fA-F]+)\) <- 0x([0-9a-fA-F]+)$)");

  std::vector<CommitRecord> records;
  CommitRecord current;
  bool have_current = false;
  std::string line;
  std::smatch match;

  const auto appendCurrent = [&] {
    if (have_current) {
      records.push_back(current);
      have_current = false;
    }
  };

  while (records.size() < max_records && std::getline(input, line)) {
    if (std::regex_match(line, match, instruction)) {
      appendCurrent();
      if (records.size() == max_records) break;
      if (decimal(match[1].str(), "Sail order") != records.size()) {
        throw std::runtime_error("Sail trace order is not a contiguous prefix");
      }
      current = CommitRecord{};
      current.order = records.size();
      current.privilege = sailPrivilege(match[2].str());
      current.pc = static_cast<uint32_t>(hexadecimal(match[3].str(), "Sail PC"));
      current.instruction = static_cast<uint32_t>(hexadecimal(match[4].str(), "Sail instruction"));
      have_current = true;
    } else if (have_current && std::regex_match(line, match, gpr)) {
      if (current.gpr_write) {
        throw std::runtime_error("Sail instruction has multiple GPR writes");
      }
      current.gpr_write = true;
      current.gpr_address = static_cast<uint8_t>(decimal(match[1].str(), "Sail GPR address"));
      current.gpr_data = static_cast<uint32_t>(hexadecimal(match[2].str(), "Sail GPR data"));
    } else if (have_current && isCsrInstruction(current.instruction) &&
      std::regex_match(line, match, csr)) {
      if (current.csr_write) {
        throw std::runtime_error("Sail instruction has multiple CSR writes");
      }
      current.csr_write = true;
      current.csr_address = static_cast<uint16_t>(hexadecimal(match[1].str(), "Sail CSR address"));
      current.csr_data = static_cast<uint32_t>(hexadecimal(match[2].str(), "Sail CSR data"));
    }
  }
  if (records.size() < max_records) appendCurrent();
  return records;
}

void compareCommitPrefixes(const std::vector<CommitRecord>& zircon,
                           const std::vector<CommitRecord>& reference) {
  if (zircon.size() != reference.size()) {
    throw std::runtime_error("Zircon and reference commit-prefix lengths differ");
  }
  for (size_t index = 0; index < zircon.size(); ++index) {
    const CommitRecord& actual = zircon[index];
    const CommitRecord& expected = reference[index];
    compareArchitecturalRecord(actual, expected, index);
    if (actual.memory.active() || expected.memory.active()) {
      throw std::runtime_error("commit-prefix comparison cannot accept memory retirements");
    }
  }
}

SparseMemory parseBackingMemorySnapshot(std::istream& input) {
  SparseMemory memory;
  const std::regex line(R"(^([0-9a-fA-F]{8}) ([0-9a-fA-F]{2})$)");
  std::string text;
  size_t count = 0;
  while (std::getline(input, text)) {
    if (text.empty()) continue;
    std::smatch match;
    if (!std::regex_match(text, match, line)) {
      throw std::runtime_error("invalid backing-memory snapshot line");
    }
    memory.write8(hexadecimal32(match[1].str(), "snapshot address"),
                  static_cast<uint8_t>(hexadecimal(match[2].str(), "snapshot byte")));
    ++count;
  }
  if (count == 0) {
    throw std::runtime_error("backing-memory snapshot is empty");
  }
  return memory;
}

void compareCommittedMemory(const std::vector<CommitRecord>& zircon,
                            const std::vector<CommitRecord>& reference,
                            SparseMemory initial_memory,
                            const SparseMemory& backing_memory) {
  if (zircon.size() != reference.size()) {
    throw std::runtime_error("Zircon and reference committed-memory lengths differ");
  }
  SparseMemory architectural_memory = initial_memory;
  std::set<uint32_t> touched_words;
  size_t memory_events = 0;
  for (size_t index = 0; index < zircon.size(); ++index) {
    const CommitRecord& actual = zircon[index];
    const CommitRecord& expected = reference[index];
    compareArchitecturalRecord(actual, expected, index);
    const CommitMemoryRecord expected_memory = expectedMemory(expected, initial_memory);
    compareMemoryRecord(actual.memory, expected_memory, index);
    if (expected_memory.active()) {
      ++memory_events;
      touched_words.insert(expected_memory.address & ~uint32_t{3});
    }
    if (expected_memory.write_mask != 0) {
      initial_memory.write32(expected_memory.address & ~uint32_t{3},
                             expected_memory.write_data, expected_memory.write_mask);
      architectural_memory.write32(actual.memory.address & ~uint32_t{3},
                                   actual.memory.write_data, actual.memory.write_mask);
    }
  }
  for (const uint32_t address : touched_words) {
    if (architectural_memory.read32(address) != initial_memory.read32(address)) {
      throw std::runtime_error("Zircon committed-memory model diverges at address " +
                               std::to_string(address));
    }
    if (backing_memory.read32(address) != initial_memory.read32(address)) {
      throw std::runtime_error("Zircon AXI backing memory diverges at address " +
                               std::to_string(address));
    }
  }
  if (memory_events == 0) {
    throw std::runtime_error("committed-memory comparison observed no memory events");
  }
}

}  // namespace zircon::sim
