#include "SpikeReference.h"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>

#include <riscv/cfg.h>
#include <riscv/encoding.h>
#include <riscv/isa_parser.h>
#include <riscv/processor.h>
#include <riscv/simif.h>

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

class SpikeReference::Impl : public simif_t {
  public:
    Impl(const SparseMemory &memory, uint32_t entry)
        : memory_(memory), isa_("RV32IMAF_Zicsr_Zifencei_Zaamo_Zalrsc_Zicntr_Zihpm", "M") {
        debug_mmu = nullptr;
        log_.reset(std::fopen("/dev/null", "w"));
        if (log_ == nullptr) {
            throw std::runtime_error("failed to open the Spike commit-log sink");
        }
        processor_ = std::make_unique<processor_t>(&isa_, &cfg_, this, 0, false, log_.get(), output_);
        harts_.emplace(0, processor_.get());
        processor_->enable_log_commits();
        processor_->get_state()->pc = entry;
    }

    char *addr_to_mem(reg_t) override { return nullptr; }

    bool reservable(reg_t address) override {
        return address >= 0x80000000ULL && address < 0xa0000000ULL;
    }

    bool mmio_load(reg_t address, size_t size, uint8_t *bytes) override {
        if (address > std::numeric_limits<uint32_t>::max() || size > sizeof(uint64_t) ||
            address + size > uint64_t{1} << 32) {
            return false;
        }
        for (size_t byte = 0; byte < size; ++byte) {
            bytes[byte] = memory_.read8(static_cast<uint32_t>(address + byte));
        }
        return true;
    }

    bool mmio_store(reg_t address, size_t size, const uint8_t *bytes) override {
        if (address > std::numeric_limits<uint32_t>::max() || size > sizeof(uint64_t) ||
            address + size > uint64_t{1} << 32) {
            return false;
        }
        for (size_t byte = 0; byte < size; ++byte) {
            memory_.write8(static_cast<uint32_t>(address + byte), bytes[byte]);
        }
        return true;
    }

    void proc_reset(unsigned) override {}

    const cfg_t &get_cfg() const override { return cfg_; }

    const std::map<size_t, processor_t *> &get_harts() const override { return harts_; }

    const char *get_symbol(uint64_t) override { return nullptr; }

    SpikeCommit next() {
        state_t *const state = processor_->get_state();
        while (true) {
            SpikeCommit commit;
            commit.pc = static_cast<uint32_t>(state->pc);
            commit.instruction = memory_.read32(commit.pc);

            processor_->step(1);
            const uint32_t nextPc = static_cast<uint32_t>(state->pc);
            const uint32_t machineVector = static_cast<uint32_t>(state->mtvec->read()) & ~uint32_t{3};
            const uint32_t supervisorVector = static_cast<uint32_t>(state->stvec->read()) & ~uint32_t{3};
            const bool trapped =
                (static_cast<uint32_t>(state->mepc->read()) == commit.pc && nextPc == machineVector) ||
                (static_cast<uint32_t>(state->sepc->read()) == commit.pc && nextPc == supervisorVector);
            if (trapped) {
                continue;
            }
            for (const auto &[encodedRegister, value] : state->log_reg_write) {
                const reg_t kind = encodedRegister & 0xf;
                if (kind != 0 && kind != 1) {
                    continue;
                }
                if (commit.write_valid) {
                    throw std::runtime_error("Spike instruction wrote multiple architectural registers");
                }
                const reg_t index = encodedRegister >> 4;
                if (index >= 32) {
                    throw std::runtime_error("Spike register index is outside the architectural file");
                }
                if (kind == 0 && index == 0) {
                    continue;
                }
                commit.write_valid = true;
                commit.is_fp = kind == 1;
                commit.rd = static_cast<uint8_t>(index);
                commit.value = static_cast<uint32_t>(value.v[0]);
            }
            return commit;
        }
    }

    void synchronize(const SpikeArchitecturalState &architectural) {
        state_t *const state = processor_->get_state();
        for (size_t index = 1; index < architectural.integer.size(); ++index) {
            state->XPR.write(index, architectural.integer[index]);
        }
        for (size_t index = 0; index < architectural.floating.size(); ++index) {
            const freg_t value = {
                uint64_t{0xffffffff00000000} | architectural.floating[index],
                std::numeric_limits<uint64_t>::max(),
            };
            state->FPR.write(index, value);
        }
    }

  private:
    struct FileCloser {
        void operator()(FILE *file) const {
            if (file != nullptr) {
                std::fclose(file);
            }
        }
    };

    SparseMemory memory_;
    cfg_t cfg_;
    isa_parser_t isa_;
    std::ostringstream output_;
    std::map<size_t, processor_t *> harts_;
    std::unique_ptr<FILE, FileCloser> log_;
    std::unique_ptr<processor_t> processor_;
};

SpikeReference::SpikeReference(const SparseMemory &memory, uint32_t entry)
    : impl_(std::make_unique<Impl>(memory, entry)) {}

SpikeReference::~SpikeReference() = default;

std::optional<SpikeCommit> SpikeReference::next() { return impl_->next(); }

void SpikeReference::synchronize(const SpikeArchitecturalState &state) { impl_->synchronize(state); }

bool SpikeReference::requiresSynchronization(uint32_t instruction) {
    if ((instruction & 0x7f) != 0x73 || ((instruction >> 12) & 0x7) == 0) {
        return false;
    }
    switch (instruction >> 20) {
    case CSR_MCYCLE:
    case CSR_MINSTRET:
    case CSR_MCYCLEH:
    case CSR_MINSTRETH:
    case CSR_CYCLE:
    case CSR_TIME:
    case CSR_INSTRET:
    case CSR_CYCLEH:
    case CSR_TIMEH:
    case CSR_INSTRETH:
        return true;
    default:
        return false;
    }
}

} // namespace zircon::sim
