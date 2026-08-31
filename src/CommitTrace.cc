#include "CommitTrace.h"

#include <charconv>
#include <regex>
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

bool boolean(const std::string& value, const char* name) {
  if (value == "true") return true;
  if (value == "false") return false;
  throw std::runtime_error(std::string("invalid boolean ") + name);
}

CommitRecord zirconRecord(const std::string& line) {
  if (boolean(field(line, "trap"), "trap") ||
      boolean(field(line, "interrupt"), "interrupt") ||
      boolean(field(line, "fprWrite"), "fprWrite") ||
      decimal(field(line, "memoryReadMask"), "memoryReadMask") != 0 ||
      decimal(field(line, "memoryWriteMask"), "memoryWriteMask") != 0) {
    throw std::runtime_error("Spike prefix comparison only accepts normal integer retirements");
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
  return record;
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
    records.push_back(record);
  }
  return records;
}

void compareCommitPrefixes(const std::vector<CommitRecord>& zircon,
                           const std::vector<CommitRecord>& spike) {
  if (zircon.size() != spike.size()) {
    throw std::runtime_error("Zircon and Spike commit-prefix lengths differ");
  }
  for (size_t index = 0; index < zircon.size(); ++index) {
    const CommitRecord& actual = zircon[index];
    const CommitRecord& expected = spike[index];
    const bool gpr_mismatch = actual.gpr_write != expected.gpr_write ||
      (actual.gpr_write && (actual.gpr_address != expected.gpr_address ||
        actual.gpr_data != expected.gpr_data));
    const bool csr_mismatch = actual.csr_write != expected.csr_write ||
      (actual.csr_write && (actual.csr_address != expected.csr_address ||
        actual.csr_data != expected.csr_data));
    if (actual.privilege != expected.privilege || actual.pc != expected.pc ||
        actual.instruction != expected.instruction || gpr_mismatch || csr_mismatch) {
      throw std::runtime_error("Zircon and Spike commit records differ at order " +
                               std::to_string(index));
    }
  }
}

}  // namespace zircon::sim
