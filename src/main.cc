#include <verilated.h>
#ifdef ZIRCON_ENABLE_VCD
#include <verilated_vcd_c.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#include "AXIMemory.h"
#include "ElfImage.h"
#include "SpikeReference.h"
#include "Statistic.h"
#include "VZirconCore.h"

#ifndef ZIRCON_SPIKE_PATH
#define ZIRCON_SPIKE_PATH "spike"
#endif

namespace {

constexpr const char *kAnsiReset = "\033[0m";
constexpr const char *kAnsiRed = "\033[1;31m";
constexpr const char *kAnsiGreen = "\033[1;32m";
constexpr const char *kAnsiYellow = "\033[1;33m";
constexpr const char *kAnsiBlue = "\033[1;34m";
constexpr const char *kAnsiMagenta = "\033[1;35m";
constexpr const char *kAnsiCyan = "\033[1;36m";
constexpr const char *kSeparator = "============================================================";

struct RunMetrics {
    uint64_t instructions = 0;
    double ipc = 0.0;
    double elapsed_seconds = 0.0;
    double cycles_per_second = 0.0;
};

enum class RunOutcome {
    Pass,
    Fail,
    Stall,
    Timeout,
};

struct Options {
    std::string elf;
    uint64_t seed = 1;
    uint64_t max_cycles = 1000000;
    uint64_t stall_cycles = 10000;
    uint64_t wave_start = 0;
    uint64_t wave_cycles = 0;
    bool allow_timeout = false;
    bool wave = false;
    bool difftest = true;
    bool progress = true;
    bool json = false;
    bool color = true;
};

const char *ansi(bool enabled, const char *code) { return enabled ? code : ""; }

std::string grouped(uint64_t value) {
    std::string result = std::to_string(value);
    for (std::ptrdiff_t position = static_cast<std::ptrdiff_t>(result.size()) - 3; position > 0; position -= 3) {
        result.insert(static_cast<size_t>(position), ",");
    }
    return result;
}

std::string hex32(uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return output.str();
}

std::string decimal(double value, int precision) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(precision) << value;
    return output.str();
}

void printField(const char *label, const std::string &value, const char *valueColor, bool color) {
    std::cout << ansi(color, kAnsiCyan) << ' ' << std::left << std::setw(14) << label << ansi(color, kAnsiReset)
              << ansi(color, valueColor) << value << ansi(color, kAnsiReset) << std::right << '\n';
}

void printBanner(const Options &options, uint32_t entry, bool color) {
    std::cout << ansi(color, kAnsiBlue) << kSeparator << '\n'
              << ansi(color, kAnsiMagenta) << " Zircon-2026 RTL Simulation" << ansi(color, kAnsiReset) << '\n'
              << ansi(color, kAnsiCyan) << "------------------------------------------------------------"
              << ansi(color, kAnsiReset) << '\n';
    printField("Program", std::filesystem::path(options.elf).filename().string(), kAnsiMagenta, color);
    printField("Entry PC", hex32(entry), kAnsiYellow, color);
    printField("Difftest", options.difftest ? "Spike" : "disabled", kAnsiGreen, color);
    printField("Seed", std::to_string(options.seed), kAnsiYellow, color);
    printField("Waveform", options.wave ? "bounded VCD enabled" : "disabled", kAnsiBlue, color);
    std::cout << ansi(color, kAnsiBlue) << kSeparator << ansi(color, kAnsiReset) << std::endl;
}

void printSummary(RunOutcome outcome, const std::string &message, const Options &options, uint64_t cycles,
                  const RunMetrics &metrics, const std::string &report, const std::string &diagnostic, bool color) {
    const char *label = "PASS";
    const char *status_color = kAnsiGreen;
    if (outcome == RunOutcome::Fail) {
        label = "FAIL";
        status_color = kAnsiRed;
    } else if (outcome == RunOutcome::Stall) {
        label = "STALL";
        status_color = kAnsiYellow;
    } else if (outcome == RunOutcome::Timeout) {
        label = "TIMEOUT";
        status_color = kAnsiYellow;
    }

    std::cout << ansi(color, kAnsiBlue) << kSeparator << '\n'
              << ansi(color, status_color) << ' ' << label << ansi(color, kAnsiReset) << "  " << message << '\n'
              << ansi(color, kAnsiCyan) << "------------------------------------------------------------"
              << ansi(color, kAnsiReset) << '\n';
    printField("Program", std::filesystem::path(options.elf).filename().string(), kAnsiMagenta, color);
    printField("Difftest", options.difftest ? "Spike matched" : "disabled", kAnsiGreen, color);
    printField("Cycles", grouped(cycles), kAnsiYellow, color);
    printField("Instructions", grouped(metrics.instructions), kAnsiYellow, color);
    printField("IPC", decimal(metrics.ipc, 6), kAnsiGreen, color);
    printField("Elapsed", decimal(metrics.elapsed_seconds, 3) + " s", kAnsiBlue, color);
    printField("Speed", grouped(static_cast<uint64_t>(metrics.cycles_per_second)) + " cycles/s", kAnsiBlue, color);
    if (!diagnostic.empty()) {
        printField("Diagnostic", diagnostic, status_color, color);
    }
    printField("Report", report, kAnsiMagenta, color);
    std::cout << ansi(color, kAnsiBlue) << kSeparator << ansi(color, kAnsiReset) << std::endl;
}

struct RetiredStore {
    uint32_t address = 0;
    uint32_t data = 0;
    uint8_t size = 0;
};

std::optional<RetiredStore> decodeRetiredStore(uint32_t instruction, const std::array<uint32_t, 32> &integerRegisters) {
    if ((instruction & 0x7fu) != 0x23u) {
        return std::nullopt;
    }

    const uint32_t funct3 = instruction >> 12 & 0x7u;
    if (funct3 > 2) {
        return std::nullopt;
    }
    const uint32_t immediate = (instruction >> 25 << 5) | (instruction >> 7 & 0x1fu);
    const int32_t signedImmediate = static_cast<int32_t>(immediate << 20) >> 20;
    const uint32_t rs1 = instruction >> 15 & 0x1fu;
    const uint32_t rs2 = instruction >> 20 & 0x1fu;
    return RetiredStore{
        integerRegisters[rs1] + static_cast<uint32_t>(signedImmediate),
        integerRegisters[rs2],
        static_cast<uint8_t>(1u << funct3),
    };
}

bool storeCoversAddress(const RetiredStore &store, uint32_t address) { return address - store.address < store.size; }

class ProgressReporter {
  public:
    ProgressReporter(const std::atomic<uint64_t> &cycles, const std::atomic<uint64_t> &instructions, bool enabled,
                     bool color)
        : cycles_(cycles), instructions_(instructions), color_(color) {
        if (enabled) {
            thread_ = std::thread([this]() { run(); });
        }
    }

    ~ProgressReporter() { finish(); }

    void finish() {
        if (!thread_.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_one();
        thread_.join();
        if (printed_) {
            std::cerr << "\r\033[2K" << std::flush;
        }
    }

    ProgressReporter(const ProgressReporter &) = delete;
    ProgressReporter &operator=(const ProgressReporter &) = delete;

  private:
    void run() {
        uint64_t previous_cycles = 0;
        auto previous_time = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(mutex_);
        while (!condition_.wait_for(lock, std::chrono::seconds(1), [this]() { return stop_; })) {
            lock.unlock();
            const auto now = std::chrono::steady_clock::now();
            const uint64_t cycles = cycles_.load(std::memory_order_relaxed);
            const uint64_t instructions = instructions_.load(std::memory_order_relaxed);
            const double seconds = std::chrono::duration<double>(now - previous_time).count();
            const double speed = seconds == 0.0 ? 0.0 : (cycles - previous_cycles) / seconds;
            const double ipc = cycles == 0 ? 0.0 : static_cast<double>(instructions) / cycles;
            std::cerr << "\r\033[2K " << ansi(color_, kAnsiMagenta) << "RUN" << ansi(color_, kAnsiReset)
                      << ansi(color_, kAnsiCyan) << "  cycles " << ansi(color_, kAnsiYellow) << grouped(cycles)
                      << ansi(color_, kAnsiCyan) << "  instructions " << ansi(color_, kAnsiYellow)
                      << grouped(instructions) << ansi(color_, kAnsiCyan) << "  IPC " << ansi(color_, kAnsiGreen)
                      << std::fixed << std::setprecision(4) << ipc << ansi(color_, kAnsiCyan) << "  speed "
                      << ansi(color_, kAnsiBlue) << grouped(static_cast<uint64_t>(speed)) << " cycles/s"
                      << ansi(color_, kAnsiReset) << std::flush;
            printed_ = true;
            previous_cycles = cycles;
            previous_time = now;
            lock.lock();
        }
    }

    const std::atomic<uint64_t> &cycles_;
    const std::atomic<uint64_t> &instructions_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
    bool stop_ = false;
    bool printed_ = false;
    bool color_ = false;
};

uint64_t parseUnsigned(const char *text, const char *option) {
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (text[0] == '\0' || end == nullptr || *end != '\0') {
        throw std::invalid_argument(std::string("invalid value for ") + option);
    }
    return value;
}

Options parseOptions(int argc, char **argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if ((argument == "--elf" || argument == "--seed" || argument == "--max-cycles" ||
             argument == "--stall-cycles" || argument == "--wave-start" || argument == "--wave-cycles") &&
            index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + argument);
        }
        if (argument == "--elf") {
            options.elf = argv[++index];
        } else if (argument == "--seed") {
            options.seed = parseUnsigned(argv[++index], "--seed");
        } else if (argument == "--max-cycles") {
            options.max_cycles = parseUnsigned(argv[++index], "--max-cycles");
        } else if (argument == "--stall-cycles") {
            options.stall_cycles = parseUnsigned(argv[++index], "--stall-cycles");
        } else if (argument == "--wave-start") {
            options.wave_start = parseUnsigned(argv[++index], "--wave-start");
        } else if (argument == "--wave-cycles") {
            options.wave_cycles = parseUnsigned(argv[++index], "--wave-cycles");
        } else if (argument == "--allow-timeout") {
            options.allow_timeout = true;
        } else if (argument == "--wave") {
            options.wave = true;
        } else if (argument == "--no-difftest") {
            options.difftest = false;
        } else if (argument == "--no-progress") {
            options.progress = false;
        } else if (argument == "--json") {
            options.json = true;
        } else if (argument == "--no-color") {
            options.color = false;
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.elf.empty()) {
        throw std::invalid_argument("--elf is required");
    }
    if (options.wave && options.wave_cycles == 0 && options.max_cycles > 100000) {
        throw std::invalid_argument("long simulations require a bounded --wave-cycles window");
    }
#ifndef ZIRCON_ENABLE_VCD
    if (options.wave) {
        throw std::invalid_argument("--wave requires a build configured with -DZIRCON_SIM_ENABLE_VCD=ON");
    }
#endif
    return options;
}

bool waveEnabled(const Options &options, uint64_t cycle) {
    if (!options.wave || cycle < options.wave_start) {
        return false;
    }
    return options.wave_cycles == 0 || cycle - options.wave_start < options.wave_cycles;
}

void driveInterrupts(VZirconCore &dut) {
    dut.io_interrupts_meip = 0;
    dut.io_interrupts_msip = 0;
    dut.io_interrupts_mtip = 0;
    dut.io_interrupts_seip = 0;
}

zircon::sim::PerformanceSnapshot readPerformance(const VZirconCore &dut) {
    zircon::sim::PerformanceSnapshot result;
    result.icacheVisit = dut.io_debug_performance_icacheVisit;
    result.icacheHit = dut.io_debug_performance_icacheHit;
    result.icacheMissCycles = dut.io_debug_performance_icacheMissCycles;
    result.fqBlockedCycles = dut.io_debug_performance_fqBlockedCycles;
    result.fqEmptyCycles = dut.io_debug_performance_fqEmptyCycles;
    result.ftqBlockedCycles = dut.io_debug_performance_ftqBlockedCycles;
    result.integerFreeListBlockedCycles = dut.io_debug_performance_integerFreeListBlockedCycles;
    result.floatingFreeListBlockedCycles = dut.io_debug_performance_floatingFreeListBlockedCycles;
    result.dispatchBlockedCycles = dut.io_debug_performance_dispatchBlockedCycles;
    result.branch = dut.io_debug_performance_branch;
    result.branchFail = dut.io_debug_performance_branchFail;
    result.directJump = dut.io_debug_performance_directJump;
    result.directJumpFail = dut.io_debug_performance_directJumpFail;
    result.call = dut.io_debug_performance_call;
    result.callFail = dut.io_debug_performance_callFail;
    result.ret = dut.io_debug_performance_ret;
    result.retFail = dut.io_debug_performance_retFail;
    result.indirect = dut.io_debug_performance_indirect;
    result.indirectFail = dut.io_debug_performance_indirectFail;
    result.robFullCycles = dut.io_debug_performance_robFullCycles;
    result.storeBufferFullCycles = dut.io_debug_performance_storeBufferFullCycles;
    result.storeBufferBusyCycles = dut.io_debug_performance_storeBufferBusyCycles;
    result.issueQueueFullCycles = {
        dut.io_debug_performance_issueQueueFullCycles_0, dut.io_debug_performance_issueQueueFullCycles_1,
        dut.io_debug_performance_issueQueueFullCycles_2, dut.io_debug_performance_issueQueueFullCycles_3,
        dut.io_debug_performance_issueQueueFullCycles_4, dut.io_debug_performance_issueQueueFullCycles_5,
    };
    result.pipelineIssueCycles = {
        dut.io_debug_performance_pipelineIssueCycles_0, dut.io_debug_performance_pipelineIssueCycles_1,
        dut.io_debug_performance_pipelineIssueCycles_2, dut.io_debug_performance_pipelineIssueCycles_3,
        dut.io_debug_performance_pipelineIssueCycles_4, dut.io_debug_performance_pipelineIssueCycles_5,
    };
    result.pipelineOperandWaitCycles = {
        dut.io_debug_performance_pipelineOperandWaitCycles_0, dut.io_debug_performance_pipelineOperandWaitCycles_1,
        dut.io_debug_performance_pipelineOperandWaitCycles_2, dut.io_debug_performance_pipelineOperandWaitCycles_3,
        dut.io_debug_performance_pipelineOperandWaitCycles_4, dut.io_debug_performance_pipelineOperandWaitCycles_5,
    };
    result.pipelineReplayBlockedCycles = {
        dut.io_debug_performance_pipelineReplayBlockedCycles_0, dut.io_debug_performance_pipelineReplayBlockedCycles_1,
        dut.io_debug_performance_pipelineReplayBlockedCycles_2, dut.io_debug_performance_pipelineReplayBlockedCycles_3,
        dut.io_debug_performance_pipelineReplayBlockedCycles_4, dut.io_debug_performance_pipelineReplayBlockedCycles_5,
    };
    result.pipelineExecutionBlockedCycles = {
        dut.io_debug_performance_pipelineExecutionBlockedCycles_0,
        dut.io_debug_performance_pipelineExecutionBlockedCycles_1,
        dut.io_debug_performance_pipelineExecutionBlockedCycles_2,
        dut.io_debug_performance_pipelineExecutionBlockedCycles_3,
        dut.io_debug_performance_pipelineExecutionBlockedCycles_4,
        dut.io_debug_performance_pipelineExecutionBlockedCycles_5,
    };
    result.divideBusyCycles = dut.io_debug_performance_divideBusyCycles;
    result.dcacheLoadVisits = {
        dut.io_debug_performance_dcacheLoadVisits_0,
        dut.io_debug_performance_dcacheLoadVisits_1,
    };
    result.dcacheLoadHits = {
        dut.io_debug_performance_dcacheLoadHits_0,
        dut.io_debug_performance_dcacheLoadHits_1,
    };
    result.dcacheLoadMisses = {
        dut.io_debug_performance_dcacheLoadMisses_0,
        dut.io_debug_performance_dcacheLoadMisses_1,
    };
    result.dcacheLoadRetries = {
        dut.io_debug_performance_dcacheLoadRetries_0,
        dut.io_debug_performance_dcacheLoadRetries_1,
    };
    result.dcacheStoreVisits = dut.io_debug_performance_dcacheStoreVisits;
    result.dcacheStoreHits = dut.io_debug_performance_dcacheStoreHits;
    result.dcacheStoreMisses = dut.io_debug_performance_dcacheStoreMisses;
    result.dcacheMissBusyCycles = dut.io_debug_performance_dcacheMissBusyCycles;
    result.l2InstructionVisits = dut.io_debug_performance_l2InstructionVisits;
    result.l2InstructionHits = dut.io_debug_performance_l2InstructionHits;
    result.l2InstructionMisses = dut.io_debug_performance_l2InstructionMisses;
    result.l2DataVisits = dut.io_debug_performance_l2DataVisits;
    result.l2DataHits = dut.io_debug_performance_l2DataHits;
    result.l2DataMisses = dut.io_debug_performance_l2DataMisses;
    result.l2InstructionVictimInsertions = dut.io_debug_performance_l2InstructionVictimInsertions;
    result.l2DataVictimInsertions = dut.io_debug_performance_l2DataVictimInsertions;
    result.lowerMemoryReads = dut.io_debug_performance_lowerMemoryReads;
    result.lowerMemoryWrites = dut.io_debug_performance_lowerMemoryWrites;
    result.l2EngineBusyCycles = dut.io_debug_performance_l2EngineBusyCycles;
    return result;
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parseOptions(argc, argv);
        auto image = zircon::sim::ElfImage::load(options.elf);
        const auto tohost = image.symbol("tohost");
        if (!tohost.has_value()) {
            throw std::runtime_error("ELF does not define tohost");
        }
        const bool human_output = !options.json && isatty(STDOUT_FILENO);
        const bool color = human_output && options.color && std::getenv("NO_COLOR") == nullptr;
        if (human_output) {
            printBanner(options, image.entry(), color);
        }

        Verilated::commandArgs(argc, argv);
        VZirconCore dut;
#ifdef ZIRCON_ENABLE_VCD
        VerilatedVcdC trace;
#endif
        AXIMemory memory(image.memory(), *tohost, options.seed);
        zircon::sim::Statistic statistic;
        std::unique_ptr<zircon::sim::SpikeReference> reference;
        if (options.difftest) {
            reference = std::make_unique<zircon::sim::SpikeReference>(image.memory(), image.entry());
        }
        uint64_t cycles_without_retirement = 0;
        uint64_t retired_instructions = 0;
        uint64_t measured_cycles = 0;
        uint64_t measured_instructions = 0;
        zircon::sim::SpikeArchitecturalState architectural_state;
        std::optional<zircon::sim::PerformanceSnapshot> measured_performance;
        bool capture_measured_performance = false;
        bool reference_complete = false;
        uint32_t last_retire_pc = image.entry();
        uint64_t simulation_time = 0;
#ifdef ZIRCON_ENABLE_VCD
        if (options.wave) {
            Verilated::traceEverOn(true);
            dut.trace(&trace, 8);
            trace.open("build/zircon.vcd");
        }
#endif
        auto dumpTrace = [&](bool enabled, uint64_t time) {
#ifdef ZIRCON_ENABLE_VCD
            if (enabled) {
                trace.dump(time);
            }
#else
            (void)enabled;
            (void)time;
#endif
        };
        auto closeTrace = [&]() {
#ifdef ZIRCON_ENABLE_VCD
            if (options.wave) {
                trace.close();
            }
#endif
        };

        driveInterrupts(dut);
        dut.reset = 1;
        for (int reset_cycle = 0; reset_cycle < 4; ++reset_cycle) {
            memory.drive(dut);
            dut.clock = 0;
            dut.eval();
            if (options.wave) {
                dumpTrace(true, simulation_time++);
            }
            dut.clock = 1;
            dut.eval();
            if (options.wave) {
                dumpTrace(true, simulation_time++);
            }
        }
        dut.reset = 0;
        const auto simulation_start = std::chrono::steady_clock::now();
        std::atomic<uint64_t> progress_cycles = 0;
        std::atomic<uint64_t> progress_instructions = 0;
        const bool progress_enabled = options.progress && human_output;
        ProgressReporter progress_reporter(progress_cycles, progress_instructions, progress_enabled, color);

        auto collectMetrics = [&](uint64_t cycles, uint64_t simulatedCycles) {
            RunMetrics metrics;
            metrics.elapsed_seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - simulation_start).count();
            metrics.instructions = measured_cycles == 0 ? retired_instructions : measured_instructions;
            metrics.ipc = cycles == 0 ? 0.0 : static_cast<double>(metrics.instructions) / cycles;
            metrics.cycles_per_second =
                metrics.elapsed_seconds == 0.0 ? 0.0 : simulatedCycles / metrics.elapsed_seconds;
            return metrics;
        };
        auto printJsonMetrics = [](const RunMetrics &metrics) {
            std::cout << ",\"instructions\":" << metrics.instructions << ",\"ipc\":" << std::fixed
                      << std::setprecision(6) << metrics.ipc << ",\"elapsedSeconds\":" << metrics.elapsed_seconds
                      << ",\"cyclesPerSecond\":" << metrics.cycles_per_second;
        };
        auto writeReport = [&](uint64_t cycles, const RunMetrics &metrics) {
            statistic.setPerformance(measured_performance.value_or(readPerformance(dut)));
            return statistic.writeMarkdownReport(options.elf, cycles, metrics.instructions, metrics.elapsed_seconds,
                                                 metrics.cycles_per_second);
        };

        for (uint64_t cycle = 0; cycle < options.max_cycles; ++cycle) {
            driveInterrupts(dut);
            memory.drive(dut);
            dut.clock = 0;
            dut.eval();
            const bool dump_wave = waveEnabled(options, cycle);
            dumpTrace(dump_wave, simulation_time);
            ++simulation_time;
            const auto result = memory.update(dut);

            const bool retire_valid[3] = {
                static_cast<bool>(dut.io_debug_retire_0_valid),
                static_cast<bool>(dut.io_debug_retire_1_valid),
                static_cast<bool>(dut.io_debug_retire_2_valid),
            };
            bool retired = false;
            for (size_t lane = 0; lane < 3; ++lane) {
                if (retire_valid[lane]) {
                    uint32_t retire_pc = 0;
                    uint32_t retire_instruction = 0;
                    uint8_t retire_rd = 0;
                    bool retire_is_fp = false;
                    bool retire_write_valid = false;
                    uint32_t retire_value = 0;
                    switch (lane) {
                    case 0:
                        retire_pc = dut.io_debug_retire_0_pc;
                        retire_instruction = dut.io_debug_retire_0_instruction;
                        retire_rd = dut.io_debug_retire_0_rd;
                        retire_is_fp = dut.io_debug_retire_0_isFp;
                        retire_write_valid = dut.io_debug_retire_0_writeValid;
                        retire_value = dut.io_debug_retire_0_value;
                        break;
                    case 1:
                        retire_pc = dut.io_debug_retire_1_pc;
                        retire_instruction = dut.io_debug_retire_1_instruction;
                        retire_rd = dut.io_debug_retire_1_rd;
                        retire_is_fp = dut.io_debug_retire_1_isFp;
                        retire_write_valid = dut.io_debug_retire_1_writeValid;
                        retire_value = dut.io_debug_retire_1_value;
                        break;
                    case 2:
                        retire_pc = dut.io_debug_retire_2_pc;
                        retire_instruction = dut.io_debug_retire_2_instruction;
                        retire_rd = dut.io_debug_retire_2_rd;
                        retire_is_fp = dut.io_debug_retire_2_isFp;
                        retire_write_valid = dut.io_debug_retire_2_writeValid;
                        retire_value = dut.io_debug_retire_2_value;
                        break;
                    }
                    retired = true;
                    ++retired_instructions;
                    if (measured_cycles == 0) {
                        statistic.observeInstruction(retire_instruction);
                    }
                    last_retire_pc = retire_pc;
                    if (options.difftest && !reference_complete) {
                        const auto next_expected = reference->next();
                        if (!next_expected.has_value()) {
                            reference_complete = true;
                            continue;
                        }
                        const zircon::sim::SpikeCommit expected = *next_expected;
                        const bool synchronize =
                            zircon::sim::SpikeReference::requiresSynchronization(retire_instruction);
                        const bool destinationMismatch =
                            expected.write_valid != retire_write_valid ||
                            (expected.write_valid && (expected.is_fp != retire_is_fp || expected.rd != retire_rd ||
                                                      expected.value != retire_value));
                        if (expected.pc != retire_pc || expected.instruction != retire_instruction ||
                            (!synchronize && destinationMismatch)) {
                            progress_reporter.finish();
                            dut.final();
                            closeTrace();
                            std::cerr << ansi(color, kAnsiRed) << "DIFFTEST FAILED" << ansi(color, kAnsiReset)
                                      << " at instruction " << retired_instructions << ", cycle " << cycle + 1
                                      << ", lane " << lane << ": DUT pc=0x" << std::hex << retire_pc << " inst=0x"
                                      << retire_instruction << " write=" << retire_write_valid;
                            if (retire_write_valid) {
                                std::cerr << " " << (retire_is_fp ? 'f' : 'x') << std::dec
                                          << static_cast<unsigned>(retire_rd) << "=0x" << std::hex << retire_value;
                            }
                            std::cerr << "; Spike pc=0x" << expected.pc << " inst=0x" << expected.instruction
                                      << " write=" << expected.write_valid;
                            if (expected.write_valid) {
                                std::cerr << " " << (expected.is_fp ? 'f' : 'x') << std::dec
                                          << static_cast<unsigned>(expected.rd) << "=0x" << std::hex << expected.value;
                            }
                            std::cerr << std::dec << std::endl;
                            return 126;
                        }
                        if (retire_write_valid && retire_rd != 0) {
                            auto &registers = retire_is_fp ? architectural_state.floating : architectural_state.integer;
                            registers[retire_rd] = retire_value;
                        }
                        if (synchronize) {
                            reference->synchronize(architectural_state);
                        }
                    }
                    if (measured_cycles == 0) {
                        const auto store = decodeRetiredStore(retire_instruction, architectural_state.integer);
                        if (store.has_value() && storeCoversAddress(*store, *tohost)) {
                            measured_cycles = cycle + 1;
                            measured_instructions = retired_instructions;
                            capture_measured_performance = true;
                        }
                    }
                    if (retire_write_valid && !retire_is_fp && retire_rd != 0) {
                        architectural_state.integer[retire_rd] = retire_value;
                    } else if (retire_write_valid && retire_is_fp) {
                        architectural_state.floating[retire_rd] = retire_value;
                    }
                }
            }
            cycles_without_retirement = retired ? 0 : cycles_without_retirement + 1;
            if (progress_enabled && (cycle & 0x3fffu) == 0) {
                progress_cycles.store(cycle + 1, std::memory_order_relaxed);
                progress_instructions.store(retired_instructions, std::memory_order_relaxed);
            }

            dut.clock = 1;
            dut.eval();
            if (capture_measured_performance) {
                measured_performance = readPerformance(dut);
                capture_measured_performance = false;
            }
            dumpTrace(dump_wave, simulation_time);
            ++simulation_time;
            if (result.has_value()) {
                if (progress_enabled) {
                    progress_cycles.store(cycle + 1, std::memory_order_relaxed);
                    progress_instructions.store(retired_instructions, std::memory_order_relaxed);
                }
                const uint64_t report_cycles = measured_cycles == 0 ? cycle + 1 : measured_cycles;
                progress_reporter.finish();
                const RunMetrics metrics = collectMetrics(report_cycles, cycle + 1);
                const std::string report = writeReport(report_cycles, metrics);
                dut.final();
                closeTrace();
                if (human_output) {
                    printSummary(*result == 0 ? RunOutcome::Pass : RunOutcome::Fail,
                                 *result == 0 ? "Program completed successfully" : "Program returned a failure code",
                                 options, report_cycles, metrics, report,
                                 *result == 0 ? "" : "tohost exit code " + std::to_string(*result), color);
                } else {
                    std::cout << "{\"status\":\"exit\",\"code\":" << *result << ",\"cycles\":" << report_cycles;
                    printJsonMetrics(metrics);
                    std::cout << ",\"seed\":" << options.seed << ",\"report\":\"" << report << "\"}" << std::endl;
                }
                return *result;
            }
            if (options.stall_cycles != 0 && cycles_without_retirement >= options.stall_cycles) {
                if (progress_enabled) {
                    progress_cycles.store(cycle + 1, std::memory_order_relaxed);
                    progress_instructions.store(retired_instructions, std::memory_order_relaxed);
                }
                const uint64_t report_cycles = measured_cycles == 0 ? cycle + 1 : measured_cycles;
                progress_reporter.finish();
                const RunMetrics metrics = collectMetrics(report_cycles, cycle + 1);
                const std::string report = writeReport(report_cycles, metrics);
                dut.final();
                closeTrace();
                if (human_output) {
                    printSummary(RunOutcome::Stall, "No instruction retired within the stall window", options,
                                 report_cycles, metrics, report,
                                 grouped(cycles_without_retirement) + " cycles without retirement; last PC " +
                                     hex32(last_retire_pc) + "; ROB head " + hex32(dut.io_debug_robHeadPc),
                                 color);
                } else {
                    std::cout << "{\"status\":\"stalled\",\"cycles\":" << report_cycles
                              << ",\"noRetireCycles\":" << cycles_without_retirement
                              << ",\"lastRetirePc\":" << last_retire_pc
                              << ",\"robHeadValid\":" << static_cast<unsigned>(dut.io_debug_robHeadValid)
                              << ",\"robHeadComplete\":" << static_cast<unsigned>(dut.io_debug_robHeadComplete)
                              << ",\"robHeadPc\":" << dut.io_debug_robHeadPc;
                    printJsonMetrics(metrics);
                    std::cout << ",\"report\":\"" << report << "\"}" << std::endl;
                }
                return 125;
            }
        }

        if (progress_enabled) {
            progress_cycles.store(options.max_cycles, std::memory_order_relaxed);
            progress_instructions.store(retired_instructions, std::memory_order_relaxed);
        }
        const uint64_t report_cycles = measured_cycles == 0 ? options.max_cycles : measured_cycles;
        progress_reporter.finish();
        const RunMetrics metrics = collectMetrics(report_cycles, options.max_cycles);
        const std::string report = writeReport(report_cycles, metrics);
        dut.final();
        closeTrace();
        if (human_output) {
            printSummary(RunOutcome::Timeout, "Maximum cycle limit reached", options, report_cycles, metrics, report,
                         "limit " + grouped(options.max_cycles) + " cycles; last PC " + hex32(last_retire_pc), color);
        } else {
            std::cout << "{\"status\":\"timeout\",\"cycles\":" << report_cycles;
            printJsonMetrics(metrics);
            std::cout << ",\"seed\":" << options.seed << ",\"entry\":" << image.entry() << ",\"tohost\":" << *tohost
                      << ",\"report\":\"" << report << "\"}" << std::endl;
        }
        return options.allow_timeout ? 0 : 124;
    } catch (const std::exception &error) {
        const bool color = isatty(STDERR_FILENO) && std::getenv("NO_COLOR") == nullptr;
        std::cerr << ansi(color, kAnsiRed) << "zircon-sim: " << error.what() << ansi(color, kAnsiReset) << std::endl;
        return 2;
    }
}
