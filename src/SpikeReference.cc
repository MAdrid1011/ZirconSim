#include "SpikeReference.h"

#include "Checkpoint.h"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <regex>
#include <algorithm>
#include <sstream>
#include <stdexcept>

#include <riscv/cfg.h>
#include <riscv/encoding.h>
#include <riscv/isa_parser.h>
#include <riscv/mmu.h>
#include <riscv/processor.h>
#include <riscv/simif.h>
#include <riscv/trap.h>

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

bool dutSupportsCsr(reg_t address) {
    if ((address >= CSR_MHPMEVENT3 && address <= CSR_MHPMEVENT31) ||
        (address >= CSR_MHPMCOUNTER3 && address <= CSR_MHPMCOUNTER31) ||
        (address >= CSR_MHPMCOUNTER3H && address <= CSR_MHPMCOUNTER31H) ||
        (address >= CSR_HPMCOUNTER3 && address <= CSR_HPMCOUNTER31) ||
        (address >= CSR_HPMCOUNTER3H && address <= CSR_HPMCOUNTER31H)) {
        return true;
    }
    switch (address) {
    case CSR_FFLAGS:
    case CSR_FRM:
    case CSR_FCSR:
    case CSR_SSTATUS:
    case CSR_SIE:
    case CSR_STVEC:
    case CSR_SCOUNTEREN:
    case CSR_SSCRATCH:
    case CSR_SEPC:
    case CSR_SCAUSE:
    case CSR_STVAL:
    case CSR_SIP:
    case CSR_SATP:
    case CSR_MSTATUS:
    case CSR_MISA:
    case CSR_MEDELEG:
    case CSR_MIDELEG:
    case CSR_MIE:
    case CSR_MTVEC:
    case CSR_MCOUNTEREN:
    case CSR_MSTATUSH:
    case CSR_MCOUNTINHIBIT:
    case CSR_MSCRATCH:
    case CSR_MEPC:
    case CSR_MCAUSE:
    case CSR_MTVAL:
    case CSR_MIP:
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
    case CSR_MVENDORID:
    case CSR_MARCHID:
    case CSR_MIMPID:
    case CSR_MHARTID:
    case CSR_MCONFIGPTR:
        return true;
    default:
        return false;
    }
}

bool accessesInterruptEnableCsr(uint32_t instruction) {
    if ((instruction & 0x7fu) != 0x73u || ((instruction >> 12) & 0x7u) == 0) {
        return false;
    }
    const uint32_t address = instruction >> 20;
    return address == CSR_MIE || address == CSR_SIE;
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
    Impl(const SparseMemory &memory, uint32_t entry, bool linuxPlatform, std::string uartInput)
        : memory_(memory), isa_("RV32IMAF_Zicsr_Zifencei_Zaamo_Zalrsc_Zicntr_Zihpm", "MSU") {
        if (linuxPlatform) {
            platform_ = std::make_unique<PlatformDevices>(false, std::move(uartInput));
        }
        cfg_.pmpregions = 0;
        debug_mmu = nullptr;
        log_.reset(std::fopen("/dev/null", "w"));
        if (log_ == nullptr) {
            throw std::runtime_error("failed to open the Spike commit-log sink");
        }
        processor_ = std::make_unique<processor_t>(&isa_, &cfg_, this, 0, false, log_.get(), output_);
        harts_.emplace(0, processor_.get());
        auto &csrMap = processor_->get_state()->csrmap;
        const auto menvcfgh = csrMap.find(CSR_MENVCFGH);
        if (menvcfgh == csrMap.end()) {
            throw std::runtime_error("Spike does not expose menvcfgh.ADUE");
        }
        menvcfgh->second->write(menvcfgh->second->read() & ~reg_t{MENVCFGH_ADUE});
        for (auto csr = csrMap.begin(); csr != csrMap.end();) {
            csr = dutSupportsCsr(csr->first) ? std::next(csr) : csrMap.erase(csr);
        }
        processor_->enable_log_commits();
        processor_->get_state()->pc = entry;
    }

    char *addr_to_mem(reg_t) override { return nullptr; }

    bool reservable(reg_t address) override { return address >= 0x80000000ULL && address < 0xa0000000ULL; }

    bool mmio_load(reg_t address, size_t size, uint8_t *bytes) override {
        if (address > std::numeric_limits<uint32_t>::max() || size > sizeof(uint64_t) ||
            address + size > uint64_t{1} << 32) {
            return false;
        }
        if (platform_ != nullptr && platform_->isDevice(address, size)) {
            return platform_->read(static_cast<uint32_t>(address), size, bytes);
        }
        if (platform_ != nullptr && !platform_->isRam(address, size)) {
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
        if (platform_ != nullptr && platform_->isDevice(address, size)) {
            return platform_->write(static_cast<uint32_t>(address), size, bytes);
        }
        if (platform_ != nullptr && !platform_->isRam(address, size)) {
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
            std::optional<uint32_t> fetchedInstruction;
            try {
                fetchedInstruction = static_cast<uint32_t>(processor_->get_mmu()->load_insn(state->pc).insn.bits());
            } catch (const trap_t &) {
                // Let processor_t take the fetch exception and advance to the trap handler below.
            }

            const reg_t enabledInterrupts = state->mie->read();
            const bool suppressInterrupts =
                !fetchedInstruction.has_value() || !accessesInterruptEnableCsr(*fetchedInstruction);
            if (suppressInterrupts) {
                state->mie->write_with_mask(std::numeric_limits<reg_t>::max(), 0);
            }
            processor_->step(1);
            if (suppressInterrupts) {
                state->mie->write_with_mask(std::numeric_limits<reg_t>::max(), enabledInterrupts);
            }
            const bool wfi = fetchedInstruction.has_value() && *fetchedInstruction == 0x10500073u;
            if (wfi) {
                // WFI may legally resume for any reason. Zircon treats it as a hint,
                // so keep Spike at the same architectural retirement boundary.
                processor_->clear_waiting_for_interrupt();
                state->pc = commit.pc + 4;
            }
            const uint32_t nextPc = static_cast<uint32_t>(state->pc);
            commit.next_pc = nextPc;
            const uint32_t machineVector = static_cast<uint32_t>(state->mtvec->read()) & ~uint32_t{3};
            const uint32_t supervisorVector = static_cast<uint32_t>(state->stvec->read()) & ~uint32_t{3};
            const bool trapped =
                (static_cast<uint32_t>(state->mepc->read()) == commit.pc && nextPc == machineVector) ||
                (static_cast<uint32_t>(state->sepc->read()) == commit.pc && nextPc == supervisorVector);
            if (trapped) {
                continue;
            }
            if (!fetchedInstruction.has_value()) {
                throw std::runtime_error("Spike retired an instruction that could not be fetched");
            }
            commit.instruction = *fetchedInstruction;
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
            const uint32_t opcode = commit.instruction & 0x7fu;
            if (opcode == 0x23u) {
                reservationAddress_.reset();
            } else if (opcode == 0x2fu && ((commit.instruction >> 12) & 0x7u) == 2u) {
                const uint32_t operation = commit.instruction >> 27;
                if (operation == 2u && !state->log_mem_read.empty()) {
                    reservationAddress_ = static_cast<uint32_t>(std::get<0>(state->log_mem_read.front()));
                } else {
                    reservationAddress_.reset();
                }
            }
            return commit;
        }
    }

    void synchronize(const SpikeArchitecturalState &architectural) {
        state_t *const state = processor_->get_state();
        for (size_t index = 1; index < architectural.integer.size(); ++index) {
            state->XPR.write(index, static_cast<reg_t>(static_cast<int32_t>(architectural.integer[index])));
        }
        for (size_t index = 0; index < architectural.floating.size(); ++index) {
            const freg_t value = {
                uint64_t{0xffffffff00000000} | architectural.floating[index],
                std::numeric_limits<uint64_t>::max(),
            };
            state->FPR.write(index, value);
        }
    }

    void setTime(uint64_t value) {
        if (platform_ != nullptr) {
            platform_->setTime(value);
        }
    }

    void appendUartInput(std::string_view input) {
        if (platform_ != nullptr) {
            platform_->appendUartInput(input);
        }
    }

    uint32_t integerRegister(size_t index) const {
        if (index >= 32) {
            throw std::out_of_range("Spike integer register index is outside the architectural file");
        }
        return static_cast<uint32_t>(processor_->get_state()->XPR[index]);
    }

    std::string diagnosticContext() const {
        const state_t *const state = processor_->get_state();
        std::ostringstream stream;
        stream << "pc=0x" << std::hex << static_cast<uint32_t>(state->pc) << std::dec
               << " priv=" << static_cast<unsigned>(state->prv) << std::hex << " mstatus=0x" << state->mstatus->read()
               << " mie=0x" << state->mie->read() << " mip=0x" << state->mip->read() << " medeleg=0x"
               << state->medeleg->read() << " mideleg=0x" << state->mideleg->read() << " mtvec=0x"
               << state->mtvec->read() << " mepc=0x" << state->mepc->read() << " mcause=0x" << state->mcause->read()
               << " mtval=0x" << state->mtval->read() << " stvec=0x" << state->stvec->read() << " sepc=0x"
               << state->sepc->read() << " scause=0x" << state->scause->read() << " stval=0x" << state->stval->read()
               << " satp=0x" << state->satp->read();
        return stream.str();
    }

    void save(CheckpointWriter &writer) const {
        const state_t *const state = processor_->get_state();
        writer.write<uint64_t>(state->pc);
        writer.write<uint64_t>(state->prv);
        for (size_t index = 0; index < NXPR; ++index) {
            writer.write<uint64_t>(state->XPR[index]);
        }
        for (size_t index = 0; index < NFPR; ++index) {
            writer.write<uint64_t>(state->FPR[index].v[0]);
            writer.write<uint64_t>(state->FPR[index].v[1]);
        }
        std::vector<std::pair<reg_t, reg_t>> csrs;
        csrs.reserve(state->csrmap.size());
        for (const auto &[address, csr] : state->csrmap) {
            csrs.emplace_back(address, csr->read());
        }
        std::sort(csrs.begin(), csrs.end());
        writer.write<uint64_t>(csrs.size());
        for (const auto &[address, value] : csrs) {
            writer.write<uint64_t>(address);
            writer.write<uint64_t>(value);
        }
        writer.write(reservationAddress_.has_value());
        writer.write(reservationAddress_.value_or(0));
        memory_.save(writer);
        writer.write(platform_ != nullptr);
        if (platform_ != nullptr) {
            platform_->save(writer);
        }
    }

    void restore(CheckpointReader &reader) {
        state_t *const state = processor_->get_state();
        const reg_t pc = reader.read<uint64_t>();
        const reg_t privilege = reader.read<uint64_t>();
        std::array<reg_t, NXPR> integers{};
        for (reg_t &value : integers) {
            value = reader.read<uint64_t>();
        }
        std::array<freg_t, NFPR> floating{};
        for (freg_t &value : floating) {
            value.v[0] = reader.read<uint64_t>();
            value.v[1] = reader.read<uint64_t>();
        }
        const uint64_t csrCount = reader.readCount(4096, "Spike CSR");
        std::vector<std::pair<reg_t, reg_t>> csrs;
        csrs.reserve(static_cast<size_t>(csrCount));
        for (uint64_t index = 0; index < csrCount; ++index) {
            csrs.emplace_back(reader.read<uint64_t>(), reader.read<uint64_t>());
        }
        const bool reservationValid = reader.read<bool>();
        const uint32_t reservationAddress = reader.read<uint32_t>();
        memory_.restore(reader);
        const bool hasPlatform = reader.read<bool>();
        if (hasPlatform != (platform_ != nullptr)) {
            throw std::runtime_error("checkpoint Spike platform does not match the selected mode");
        }
        if (platform_ != nullptr) {
            platform_->restore(reader);
        }

        const auto restoredMstatus =
            std::find_if(csrs.begin(), csrs.end(), [](const auto &csr) { return csr.first == CSR_MSTATUS; });
        if (restoredMstatus == csrs.end()) {
            throw std::runtime_error("checkpoint does not contain Spike mstatus");
        }
        state->mstatus->write(restoredMstatus->second);
        for (const auto &[address, value] : csrs) {
            const auto found = state->csrmap.find(address);
            if (found == state->csrmap.end()) {
                throw std::runtime_error("checkpoint contains a Spike CSR absent from this build");
            }
            if (address == CSR_MSTATUS) {
                continue;
            }
            const bool floatingCsr = address == CSR_FFLAGS || address == CSR_FRM || address == CSR_FCSR;
            if (floatingCsr && (restoredMstatus->second & MSTATUS_FS) == 0) {
                continue;
            }
            found->second->write(value);
        }
        for (size_t index = 1; index < integers.size(); ++index) {
            state->XPR.write(index, integers[index]);
        }
        for (size_t index = 0; index < floating.size(); ++index) {
            state->FPR.write(index, floating[index]);
        }
        state->pc = pc;
        processor_->set_privilege(privilege, false);
        processor_->get_mmu()->flush_tlb();
        processor_->get_mmu()->yield_load_reservation();
        reservationAddress_.reset();
        if (reservationValid) {
            processor_->get_mmu()->load_reserved<uint32_t>(reservationAddress);
            reservationAddress_ = reservationAddress;
        }
        state->log_reg_write.clear();
        state->log_mem_read.clear();
        state->log_mem_write.clear();
    }

    void injectInterrupt(const SpikeTrap &trap) {
        if ((trap.cause & 0x80000000u) == 0) {
            throw std::runtime_error("attempted to inject a synchronous trap into Spike");
        }
        state_t *const state = processor_->get_state();
        if (static_cast<uint32_t>(state->pc) != trap.epc) {
            throw std::runtime_error("Spike interrupt PC does not match the DUT trap PC");
        }
        const uint32_t code = trap.cause & 0x1fu;
        const reg_t mask = reg_t{1} << code;
        const reg_t enabledInterrupts = state->mie->read();
        const reg_t pendingInterrupt = state->mip->read() & mask;
        state->mie->write_with_mask(std::numeric_limits<reg_t>::max(), mask);
        state->mip->backdoor_write_with_mask(mask, mask);
        processor_->step(1);
        state->mip->backdoor_write_with_mask(mask, pendingInterrupt);
        state->mie->write_with_mask(std::numeric_limits<reg_t>::max(), enabledInterrupts);

        const bool supervisor = trap.target_privilege == 1;
        const uint32_t actualCause = static_cast<uint32_t>((supervisor ? state->scause : state->mcause)->read());
        const uint32_t actualEpc = static_cast<uint32_t>((supervisor ? state->sepc : state->mepc)->read());
        const uint32_t actualTval = static_cast<uint32_t>((supervisor ? state->stval : state->mtval)->read());
        if (state->prv != trap.target_privilege || actualCause != trap.cause || actualEpc != trap.epc ||
            actualTval != trap.tval) {
            throw std::runtime_error("Spike accepted an interrupt with architectural state different from the DUT");
        }
    }

    void injectPageFault(const SpikeTrap &trap) {
        if (trap.cause != CAUSE_FETCH_PAGE_FAULT && trap.cause != CAUSE_LOAD_PAGE_FAULT &&
            trap.cause != CAUSE_STORE_PAGE_FAULT) {
            throw std::runtime_error("attempted to inject a non-page-fault exception into Spike");
        }

        state_t *const state = processor_->get_state();
        if (static_cast<uint32_t>(state->pc) != trap.epc) {
            throw std::runtime_error("Spike page-fault PC does not match the DUT trap PC");
        }
        const bool delegated = state->prv != PRV_M && ((state->medeleg->read() >> trap.cause) & 1u) != 0;
        const uint8_t targetPrivilege = delegated ? PRV_S : PRV_M;
        if (trap.target_privilege != targetPrivilege) {
            throw std::runtime_error("Spike page-fault delegation does not match the DUT target privilege");
        }

        reg_t status = state->mstatus->read();
        if (delegated) {
            status = set_field(status, MSTATUS_SPIE, get_field(status, MSTATUS_SIE));
            status = set_field(status, MSTATUS_SIE, 0);
            status = set_field(status, MSTATUS_SPP, state->prv);
            state->mstatus->write(status);
            state->sepc->write(trap.epc);
            state->scause->write(trap.cause);
            state->stval->write(trap.tval);
            state->pc = state->stvec->read() & ~reg_t{3};
            processor_->set_privilege(PRV_S, false);
        } else {
            status = set_field(status, MSTATUS_MPIE, get_field(status, MSTATUS_MIE));
            status = set_field(status, MSTATUS_MIE, 0);
            status = set_field(status, MSTATUS_MPP, state->prv);
            if (state->prv != PRV_M) {
                status = set_field(status, MSTATUS_MPRV, 0);
            }
            state->mstatus->write(status);
            state->mepc->write(trap.epc);
            state->mcause->write(trap.cause);
            state->mtval->write(trap.tval);
            state->pc = state->mtvec->read() & ~reg_t{3};
            processor_->set_privilege(PRV_M, false);
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
    std::unique_ptr<PlatformDevices> platform_;
    cfg_t cfg_;
    isa_parser_t isa_;
    std::ostringstream output_;
    std::map<size_t, processor_t *> harts_;
    std::unique_ptr<FILE, FileCloser> log_;
    std::unique_ptr<processor_t> processor_;
    std::optional<uint32_t> reservationAddress_;
};

SpikeReference::SpikeReference(const SparseMemory &memory, uint32_t entry, bool linuxPlatform, std::string uartInput)
    : impl_(std::make_unique<Impl>(memory, entry, linuxPlatform, std::move(uartInput))) {}

SpikeReference::~SpikeReference() = default;

std::optional<SpikeCommit> SpikeReference::next() { return impl_->next(); }

void SpikeReference::synchronize(const SpikeArchitecturalState &state) { impl_->synchronize(state); }

void SpikeReference::setTime(uint64_t value) { impl_->setTime(value); }

void SpikeReference::appendUartInput(std::string_view input) { impl_->appendUartInput(input); }

void SpikeReference::injectInterrupt(const SpikeTrap &trap) { impl_->injectInterrupt(trap); }

void SpikeReference::injectPageFault(const SpikeTrap &trap) { impl_->injectPageFault(trap); }

uint32_t SpikeReference::integerRegister(size_t index) const { return impl_->integerRegister(index); }

std::string SpikeReference::diagnosticContext() const { return impl_->diagnosticContext(); }

void SpikeReference::save(CheckpointWriter &writer) const { impl_->save(writer); }

void SpikeReference::restore(CheckpointReader &reader) { impl_->restore(reader); }

bool SpikeReference::requiresSynchronization(uint32_t instruction) {
    if ((instruction & 0x7f) != 0x73 || ((instruction >> 12) & 0x7) == 0) {
        return false;
    }
    switch (instruction >> 20) {
    case CSR_MVENDORID:
    case CSR_MARCHID:
    case CSR_MIMPID:
    case CSR_MHARTID:
    case CSR_MCONFIGPTR:
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
