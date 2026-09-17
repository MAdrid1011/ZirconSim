#include "SpikeReference.h"

#include <charconv>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>

namespace zircon::sim {
namespace {

uint64_t parseUnsigned(const std::string &text, int base, const char *field) {
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error(std::string("invalid Spike ") + field + ": " + text);
    }
    return value;
}

std::string shellQuote(const std::string &value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    return quoted + "'";
}

} // namespace

std::optional<SpikeCommit> parseSpikeCommitLine(const std::string &line) {
    static const std::regex instruction(R"(^core\s+0:\s+([0-9]+)\s+0x([0-9a-fA-F]+)\s+\(0x([0-9a-fA-F]+)\)(.*)$)");
    static const std::regex integerRegister(R"(\sx\s*([0-9]+)\s+0x([0-9a-fA-F]+))");
    static const std::regex floatingRegister(R"(\sf\s*([0-9]+)\s+0x([0-9a-fA-F]+))");

    std::smatch match;
    if (!std::regex_match(line, match, instruction)) {
        return std::nullopt;
    }

    SpikeCommit commit;
    commit.pc = static_cast<uint32_t>(parseUnsigned(match[2].str(), 16, "PC"));
    commit.instruction = static_cast<uint32_t>(parseUnsigned(match[3].str(), 16, "instruction"));

    const std::string effects = match[4].str();
    std::smatch integerEffect;
    std::smatch floatingEffect;
    const bool integerWrite = std::regex_search(effects, integerEffect, integerRegister);
    const bool floatingWrite = std::regex_search(effects, floatingEffect, floatingRegister);
    if (integerWrite && floatingWrite) {
        throw std::runtime_error("Spike commit writes both an integer and floating register");
    }
    if (integerWrite || floatingWrite) {
        const std::smatch &effect = integerWrite ? integerEffect : floatingEffect;
        const uint64_t rd = parseUnsigned(effect[1].str(), 10, "register index");
        if (rd >= 32) {
            throw std::runtime_error("Spike register index is outside the architectural file");
        }
        commit.write_valid = true;
        commit.is_fp = floatingWrite;
        commit.rd = static_cast<uint8_t>(rd);
        commit.value = static_cast<uint32_t>(parseUnsigned(effect[2].str(), 16, "register value"));
    }
    return commit;
}

SpikeReference::SpikeReference(const std::string &spike, const std::string &elf, uint32_t entry) : entry_(entry) {
    std::ostringstream command;
    command << "exec " << shellQuote(spike) << " -l --log-commits --isa=RV32IMAF_Zicsr_Zifencei --priv=m --pc=0x"
            << std::hex << std::setw(8) << std::setfill('0') << entry << " " << shellQuote(elf) << " 2>&1";
    stream_ = popen(command.str().c_str(), "r");
    if (stream_ == nullptr) {
        throw std::runtime_error("failed to start Spike");
    }
}

SpikeReference::~SpikeReference() {
    if (stream_ != nullptr) {
        pclose(stream_);
    }
}

std::optional<SpikeCommit> SpikeReference::next() {
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), stream_) != nullptr) {
        last_line_ = buffer;
        while (!last_line_.empty() && (last_line_.back() == '\n' || last_line_.back() == '\r')) {
            last_line_.pop_back();
        }
        const auto parsed = parseSpikeCommitLine(last_line_);
        if (!parsed.has_value()) {
            continue;
        }
        if (!reached_entry_) {
            if (parsed->pc != entry_) {
                continue;
            }
            reached_entry_ = true;
        }
        return parsed;
    }

    const int status = pclose(stream_);
    stream_ = nullptr;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && reached_entry_) {
        return std::nullopt;
    }
    std::ostringstream message;
    message << "Spike ended before the next DUT retirement";
    if (!reached_entry_) {
        message << " without reaching the ELF entry point";
    }
    if (WIFEXITED(status)) {
        message << " (exit " << WEXITSTATUS(status) << ")";
    }
    if (!last_line_.empty()) {
        message << "; last output: " << last_line_;
    }
    throw std::runtime_error(message.str());
}

} // namespace zircon::sim
