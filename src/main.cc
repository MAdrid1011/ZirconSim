#include <verilated.h>
#ifdef ZIRCON_ENABLE_CHECKPOINTS
#include <verilated_save.h>
#endif
#ifdef ZIRCON_ENABLE_VCD
#include <verilated_vcd_c.h>
#endif

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

#include "AXIMemory.h"
#include "Checkpoint.h"
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
#ifdef ZIRCON_ENABLE_CHECKPOINTS
constexpr uint64_t kCheckpointMagic = UINT64_C(0x5a4952434f4e4350);
constexpr uint32_t kCheckpointVersion = 3;
#endif

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

enum class PlatformMode {
    Baremetal,
    Linux,
};

struct Options {
    std::string elf;
    std::string uart_input;
    std::string pass_marker;
    std::string checkpoint_save;
    std::string checkpoint_load;
    std::string checkpoint_marker;
    uint64_t seed = 1;
    uint64_t max_cycles = 1000000;
    uint64_t stall_cycles = 10000;
    uint64_t wave_start = 0;
    uint64_t wave_cycles = 0;
    uint64_t progress_interval_seconds = 30;
    uint64_t checkpoint_cycle = 0;
    uint64_t checkpoint_interval = 0;
    bool allow_timeout = false;
    bool wave = false;
    bool difftest = true;
    bool progress = true;
    bool json = false;
    bool color = true;
    bool branch_profile = false;
    bool uart_stdio = false;
    PlatformMode platform = PlatformMode::Baremetal;
};

struct BranchProfileEntry {
    uint32_t instruction = 0;
    uint64_t executions = 0;
    uint64_t mispredictions = 0;
    uint64_t taken = 0;
};

struct PendingBranch {
    uint32_t pc = 0;
};

struct RecentCommit {
    uint64_t cycle = 0;
    uint32_t dut_pc = 0;
    uint32_t dut_instruction = 0;
    uint32_t spike_pc = 0;
    uint32_t spike_instruction = 0;
    uint32_t spike_next_pc = 0;
    uint8_t lane = 0;
};

struct SimulationState {
    uint64_t next_cycle = 0;
    uint64_t cycles_without_retirement = 0;
    uint64_t retired_instructions = 0;
    uint64_t measured_cycles = 0;
    uint64_t measured_instructions = 0;
    zircon::sim::SpikeArchitecturalState architectural_state;
    std::optional<zircon::sim::PerformanceSnapshot> measured_performance;
    bool capture_measured_performance = false;
    bool reference_complete = false;
    uint32_t last_retire_pc = 0;
    uint64_t simulation_time = 0;
    std::unordered_map<uint32_t, BranchProfileEntry> branch_profile;
    std::optional<PendingBranch> pending_branch;
    std::array<RecentCommit, 16> recent_commits{};
    size_t recent_commit_count = 0;
    bool checkpoint_marker_seen = false;
    bool waiting_after_wfi = false;
};

const char *ansi(bool enabled, const char *code) { return enabled ? code : ""; }

std::string grouped(uint64_t value) {
    std::string result = std::to_string(value);
    for (std::ptrdiff_t position = static_cast<std::ptrdiff_t>(result.size()) - 3; position > 0; position -= 3) {
        result.insert(static_cast<size_t>(position), ",");
    }
    return result;
}

std::string csrContext(const VZirconCore &dut) {
    std::ostringstream stream;
    stream << "priv=" << std::dec << static_cast<unsigned>(dut.io_debug_privilege) << std::hex << " mstatus=0x"
           << dut.io_debug_csr_mstatus << " mie=0x" << dut.io_debug_csr_mie << " mip=0x" << dut.io_debug_csr_mip
           << " medeleg=0x" << dut.io_debug_csr_medeleg << " mideleg=0x" << dut.io_debug_csr_mideleg << " mtvec=0x"
           << dut.io_debug_csr_mtvec << " mepc=0x" << dut.io_debug_csr_mepc << " mcause=0x" << dut.io_debug_csr_mcause
           << " mtval=0x" << dut.io_debug_csr_mtval << " stvec=0x" << dut.io_debug_csr_stvec << " sepc=0x"
           << dut.io_debug_csr_sepc << " scause=0x" << dut.io_debug_csr_scause << " stval=0x" << dut.io_debug_csr_stval
           << " satp=0x" << dut.io_debug_csr_satp << " frm=0x" << static_cast<unsigned>(dut.io_debug_csr_frm)
           << " fflags=0x" << static_cast<unsigned>(dut.io_debug_csr_fflags);
    return stream.str();
}

std::string hex32(uint32_t value);

void printRecentCommits(const std::array<RecentCommit, 16> &recentCommits, size_t recentCommitCount) {
    std::cerr << "Recent commits:\n";
    const size_t historySize = std::min(recentCommitCount, recentCommits.size());
    const size_t historyStart = recentCommitCount - historySize;
    for (size_t history = historyStart; history < recentCommitCount; ++history) {
        const RecentCommit &entry = recentCommits[history % recentCommits.size()];
        std::cerr << "  cycle=" << entry.cycle << " lane=" << static_cast<unsigned>(entry.lane)
                  << " DUT=" << hex32(entry.dut_pc) << '/' << hex32(entry.dut_instruction)
                  << " Spike=" << hex32(entry.spike_pc) << '/' << hex32(entry.spike_instruction) << " -> "
                  << hex32(entry.spike_next_pc) << '\n';
    }
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
    printField("Platform", options.platform == PlatformMode::Linux ? "linux" : "baremetal", kAnsiBlue, color);
    printField("Seed", std::to_string(options.seed), kAnsiYellow, color);
    printField("Waveform", options.wave ? "bounded VCD enabled" : "disabled", kAnsiBlue, color);
    std::cout << ansi(color, kAnsiBlue) << kSeparator << ansi(color, kAnsiReset) << std::endl;
}

std::string readFile(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open UART input: " + path);
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class NonblockingInput {
  public:
    explicit NonblockingInput(bool enabled) : enabled_(enabled) {
        if (!enabled_) {
            return;
        }
        originalFlags_ = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (originalFlags_ < 0 || fcntl(STDIN_FILENO, F_SETFL, originalFlags_ | O_NONBLOCK) < 0) {
            throw std::runtime_error("failed to make standard input nonblocking");
        }
    }

    ~NonblockingInput() {
        if (enabled_ && originalFlags_ >= 0) {
            static_cast<void>(fcntl(STDIN_FILENO, F_SETFL, originalFlags_));
        }
    }

    NonblockingInput(const NonblockingInput &) = delete;
    NonblockingInput &operator=(const NonblockingInput &) = delete;

    std::string poll() const {
        if (!enabled_) {
            return {};
        }
        std::array<char, 256> buffer{};
        const ssize_t count = read(STDIN_FILENO, buffer.data(), buffer.size());
        return count > 0 ? std::string(buffer.data(), static_cast<size_t>(count)) : std::string{};
    }

  private:
    bool enabled_;
    int originalFlags_ = -1;
};

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
                     bool color, uint64_t intervalSeconds)
        : cycles_(cycles), instructions_(instructions), color_(color), interval_(intervalSeconds) {
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
    }

    ProgressReporter(const ProgressReporter &) = delete;
    ProgressReporter &operator=(const ProgressReporter &) = delete;

  private:
    void run() {
        uint64_t previous_cycles = cycles_.load(std::memory_order_relaxed);
        uint64_t previous_instructions = instructions_.load(std::memory_order_relaxed);
        const auto start_time = std::chrono::steady_clock::now();
        auto previous_time = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(mutex_);
        while (!condition_.wait_for(lock, interval_, [this]() { return stop_; })) {
            lock.unlock();
            const auto now = std::chrono::steady_clock::now();
            const uint64_t cycles = cycles_.load(std::memory_order_relaxed);
            const uint64_t instructions = instructions_.load(std::memory_order_relaxed);
            const double seconds = std::chrono::duration<double>(now - previous_time).count();
            const double elapsed = std::chrono::duration<double>(now - start_time).count();
            const uint64_t window_cycles = cycles - previous_cycles;
            const uint64_t window_instructions = instructions - previous_instructions;
            const double speed = seconds == 0.0 ? 0.0 : (cycles - previous_cycles) / seconds;
            const double window_ipc =
                window_cycles == 0 ? 0.0 : static_cast<double>(window_instructions) / window_cycles;
            const double cumulative_ipc = cycles == 0 ? 0.0 : static_cast<double>(instructions) / cycles;
            std::cerr << ansi(color_, kAnsiMagenta) << "RUN" << ansi(color_, kAnsiReset) << ansi(color_, kAnsiCyan)
                      << "  elapsed " << ansi(color_, kAnsiBlue) << std::fixed << std::setprecision(1) << elapsed
                      << " s" << ansi(color_, kAnsiCyan) << "  cycles " << ansi(color_, kAnsiYellow) << grouped(cycles)
                      << ansi(color_, kAnsiCyan) << "  instructions " << ansi(color_, kAnsiYellow)
                      << grouped(instructions) << ansi(color_, kAnsiCyan) << "  window IPC " << ansi(color_, kAnsiGreen)
                      << std::setprecision(4) << window_ipc << ansi(color_, kAnsiCyan) << "  cumulative IPC "
                      << ansi(color_, kAnsiGreen) << cumulative_ipc << ansi(color_, kAnsiCyan) << "  speed "
                      << ansi(color_, kAnsiBlue) << grouped(static_cast<uint64_t>(speed)) << " cycles/s"
                      << ansi(color_, kAnsiReset) << '\n'
                      << std::flush;
            previous_cycles = cycles;
            previous_instructions = instructions;
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
    bool color_ = false;
    std::chrono::seconds interval_;
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
             argument == "--stall-cycles" || argument == "--wave-start" || argument == "--wave-cycles" ||
             argument == "--platform" || argument == "--uart-input" || argument == "--pass-marker" ||
             argument == "--progress-interval" || argument == "--checkpoint-save" || argument == "--checkpoint-load" ||
             argument == "--checkpoint-cycle" || argument == "--checkpoint-interval" ||
             argument == "--checkpoint-marker") &&
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
        } else if (argument == "--platform") {
            const std::string platform = argv[++index];
            if (platform == "baremetal") {
                options.platform = PlatformMode::Baremetal;
            } else if (platform == "linux") {
                options.platform = PlatformMode::Linux;
            } else {
                throw std::invalid_argument("--platform must be baremetal or linux");
            }
        } else if (argument == "--uart-input") {
            options.uart_input = argv[++index];
        } else if (argument == "--pass-marker") {
            options.pass_marker = argv[++index];
        } else if (argument == "--progress-interval") {
            options.progress_interval_seconds = parseUnsigned(argv[++index], "--progress-interval");
            if (options.progress_interval_seconds == 0) {
                throw std::invalid_argument("--progress-interval must be greater than zero");
            }
        } else if (argument == "--checkpoint-save") {
            options.checkpoint_save = argv[++index];
        } else if (argument == "--checkpoint-load") {
            options.checkpoint_load = argv[++index];
        } else if (argument == "--checkpoint-cycle") {
            options.checkpoint_cycle = parseUnsigned(argv[++index], "--checkpoint-cycle");
            if (options.checkpoint_cycle == 0) {
                throw std::invalid_argument("--checkpoint-cycle must be greater than zero");
            }
        } else if (argument == "--checkpoint-interval") {
            options.checkpoint_interval = parseUnsigned(argv[++index], "--checkpoint-interval");
            if (options.checkpoint_interval == 0) {
                throw std::invalid_argument("--checkpoint-interval must be greater than zero");
            }
        } else if (argument == "--checkpoint-marker") {
            options.checkpoint_marker = argv[++index];
            if (options.checkpoint_marker.empty()) {
                throw std::invalid_argument("--checkpoint-marker must not be empty");
            }
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
        } else if (argument == "--branch-profile") {
            options.branch_profile = true;
        } else if (argument == "--uart-stdio") {
            options.uart_stdio = true;
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.elf.empty()) {
        throw std::invalid_argument("--elf is required");
    }
    if (options.platform != PlatformMode::Linux &&
        (!options.uart_input.empty() || options.uart_stdio || !options.pass_marker.empty())) {
        throw std::invalid_argument("UART input and pass markers require --platform linux");
    }
    if (options.wave && options.wave_cycles == 0 && options.max_cycles > 100000) {
        throw std::invalid_argument("long simulations require a bounded --wave-cycles window");
    }
    if ((options.checkpoint_cycle != 0 || options.checkpoint_interval != 0 || !options.checkpoint_marker.empty()) &&
        options.checkpoint_save.empty()) {
        throw std::invalid_argument("checkpoint triggers require --checkpoint-save");
    }
    if (!options.checkpoint_save.empty() && options.checkpoint_cycle == 0 && options.checkpoint_interval == 0 &&
        options.checkpoint_marker.empty()) {
        throw std::invalid_argument("--checkpoint-save requires a cycle, interval, or marker trigger");
    }
#ifndef ZIRCON_ENABLE_CHECKPOINTS
    if (!options.checkpoint_save.empty() || !options.checkpoint_load.empty()) {
        throw std::invalid_argument(
            "checkpoint options require a build configured with -DZIRCON_SIM_ENABLE_CHECKPOINTS=ON");
    }
#endif
#ifndef ZIRCON_ENABLE_VCD
    if (options.wave) {
        throw std::invalid_argument("--wave requires a build configured with -DZIRCON_SIM_ENABLE_VCD=ON");
    }
#endif
    return options;
}

void printBranchProfile(const std::unordered_map<uint32_t, BranchProfileEntry> &profile) {
    std::vector<std::pair<uint32_t, BranchProfileEntry>> entries(profile.begin(), profile.end());
    std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
        if (left.second.mispredictions != right.second.mispredictions) {
            return left.second.mispredictions > right.second.mispredictions;
        }
        return left.second.executions > right.second.executions;
    });

    std::cerr << "\nConditional branch profile (top misprediction PCs)\n"
              << "PC          instruction executions mispredict accuracy  taken\n";
    const size_t limit = std::min<size_t>(entries.size(), 32);
    for (size_t index = 0; index < limit; ++index) {
        const auto &[pc, entry] = entries[index];
        const double accuracy = entry.executions == 0
                                    ? 0.0
                                    : 100.0 * static_cast<double>(entry.executions - entry.mispredictions) /
                                          static_cast<double>(entry.executions);
        const double taken = entry.executions == 0
                                 ? 0.0
                                 : 100.0 * static_cast<double>(entry.taken) / static_cast<double>(entry.executions);
        std::cerr << hex32(pc) << "  " << hex32(entry.instruction) << ' ' << std::setw(10) << entry.executions << ' '
                  << std::setw(10) << entry.mispredictions << ' ' << std::fixed << std::setprecision(2) << std::setw(7)
                  << accuracy << "% " << std::setw(6) << taken << "%\n";
    }
}

bool waveEnabled(const Options &options, uint64_t cycle) {
    if (!options.wave || cycle < options.wave_start) {
        return false;
    }
    return options.wave_cycles == 0 || cycle - options.wave_start < options.wave_cycles;
}

void driveInterrupts(VZirconCore &dut, bool timerInterrupt) {
    dut.io_interrupts_meip = 0;
    dut.io_interrupts_msip = 0;
    dut.io_interrupts_mtip = timerInterrupt;
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
    result.loopTraining = dut.io_debug_performance_loopTraining;
    result.loopProvider = dut.io_debug_performance_loopProvider;
    result.loopCorrect = dut.io_debug_performance_loopCorrect;
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
    result.dcacheLoadRetryTranslation = {
        dut.io_debug_performance_dcacheLoadRetryTranslation_0,
        dut.io_debug_performance_dcacheLoadRetryTranslation_1,
    };
    result.dcacheLoadRetryForwardBlocked = {
        dut.io_debug_performance_dcacheLoadRetryForwardBlocked_0,
        dut.io_debug_performance_dcacheLoadRetryForwardBlocked_1,
    };
    result.dcacheLoadRetryUncachedOrder = {
        dut.io_debug_performance_dcacheLoadRetryUncachedOrder_0,
        dut.io_debug_performance_dcacheLoadRetryUncachedOrder_1,
    };
    result.dcacheLoadRetryStaleLookup = {
        dut.io_debug_performance_dcacheLoadRetryStaleLookup_0,
        dut.io_debug_performance_dcacheLoadRetryStaleLookup_1,
    };
    result.dcacheLoadRetryMissBusy = {
        dut.io_debug_performance_dcacheLoadRetryMissBusy_0,
        dut.io_debug_performance_dcacheLoadRetryMissBusy_1,
    };
    result.dcacheLoadRetryStoreConflict = {
        dut.io_debug_performance_dcacheLoadRetryStoreConflict_0,
        dut.io_debug_performance_dcacheLoadRetryStoreConflict_1,
    };
    result.dcacheLoadRetryLaneConflict = {
        dut.io_debug_performance_dcacheLoadRetryLaneConflict_0,
        dut.io_debug_performance_dcacheLoadRetryLaneConflict_1,
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

#ifdef ZIRCON_ENABLE_CHECKPOINTS
std::filesystem::path checkpointPath(const std::string &base, const char *suffix) {
    return std::filesystem::path(base + suffix);
}

void saveCheckpoint(const std::string &base, const Options &options, uint64_t imageFingerprint, VZirconCore &dut,
                    const zircon::sim::ElfImage &image, const AXIMemory &memory,
                    const std::optional<zircon::sim::PlatformDevices> &platform,
                    const zircon::sim::SpikeReference *reference, const zircon::sim::Statistic &statistic,
                    const SimulationState &state) {
    const std::filesystem::path rtlPath = checkpointPath(base, ".rtl");
    const std::filesystem::path hostPath = checkpointPath(base, ".host");
    const std::filesystem::path rtlTemporary = checkpointPath(base, ".rtl.tmp");
    const std::filesystem::path hostTemporary = checkpointPath(base, ".host.tmp");
    if (!rtlPath.parent_path().empty()) {
        std::filesystem::create_directories(rtlPath.parent_path());
    }
    std::filesystem::remove(rtlTemporary);
    std::filesystem::remove(hostTemporary);
    try {
        VerilatedSave rtl;
        rtl.open(rtlTemporary.string());
        if (!rtl.isOpen()) {
            throw std::runtime_error("cannot create checkpoint RTL state: " + rtlTemporary.string());
        }
        rtl << dut;
        rtl.close();
        const uint64_t rtlFingerprint = zircon::sim::fileFingerprint(rtlTemporary);

        zircon::sim::CheckpointWriter writer(hostTemporary);
        writer.write(kCheckpointMagic);
        writer.write(kCheckpointVersion);
        writer.write(imageFingerprint);
        writer.write(rtlFingerprint);
        writer.write(static_cast<uint8_t>(options.platform));
        writer.write(options.difftest);
        writer.write(options.seed);
        writer.write(state.next_cycle);
        writer.write(state.cycles_without_retirement);
        writer.write(state.retired_instructions);
        writer.write(state.measured_cycles);
        writer.write(state.measured_instructions);
        writer.write(state.architectural_state);
        writer.write(state.measured_performance.has_value());
        if (state.measured_performance.has_value()) {
            writer.write(*state.measured_performance);
        }
        writer.write(state.capture_measured_performance);
        writer.write(state.reference_complete);
        writer.write(state.last_retire_pc);
        writer.write(state.simulation_time);
        writer.write<uint64_t>(state.branch_profile.size());
        for (const auto &[pc, entry] : state.branch_profile) {
            writer.write(pc);
            writer.write(entry);
        }
        writer.write(state.pending_branch.has_value());
        if (state.pending_branch.has_value()) {
            writer.write(*state.pending_branch);
        }
        writer.write(state.recent_commits);
        writer.write<uint64_t>(state.recent_commit_count);
        writer.write(state.checkpoint_marker_seen);
        writer.write(state.waiting_after_wfi);
        statistic.save(writer);
        image.memory().save(writer);
        memory.save(writer);
        writer.write(platform.has_value());
        if (platform.has_value()) {
            platform->save(writer);
        }
        writer.write(reference != nullptr);
        if (reference != nullptr) {
            reference->save(writer);
        }
        writer.close();

        std::filesystem::rename(rtlTemporary, rtlPath);
        std::filesystem::rename(hostTemporary, hostPath);
    } catch (...) {
        std::filesystem::remove(rtlTemporary);
        std::filesystem::remove(hostTemporary);
        throw;
    }
}

void restoreCheckpoint(const std::string &base, const Options &options, uint64_t imageFingerprint, VZirconCore &dut,
                       zircon::sim::ElfImage &image, AXIMemory &memory,
                       std::optional<zircon::sim::PlatformDevices> &platform, zircon::sim::SpikeReference *reference,
                       zircon::sim::Statistic &statistic, SimulationState &state) {
    const std::filesystem::path rtlPath = checkpointPath(base, ".rtl");
    const std::filesystem::path hostPath = checkpointPath(base, ".host");
    zircon::sim::CheckpointReader reader(hostPath);
    if (reader.read<uint64_t>() != kCheckpointMagic) {
        throw std::runtime_error("checkpoint host state has an invalid magic value");
    }
    const uint32_t checkpointVersion = reader.read<uint32_t>();
    if (checkpointVersion != kCheckpointVersion) {
        throw std::runtime_error("checkpoint host state uses an unsupported version");
    }
    if (reader.read<uint64_t>() != imageFingerprint) {
        throw std::runtime_error("checkpoint was created for a different ELF image");
    }
    const uint64_t expectedRtlFingerprint = reader.read<uint64_t>();
    if (zircon::sim::fileFingerprint(rtlPath) != expectedRtlFingerprint) {
        throw std::runtime_error("checkpoint RTL and host state files do not form a matching pair");
    }
    if (reader.read<uint8_t>() != static_cast<uint8_t>(options.platform)) {
        throw std::runtime_error("checkpoint platform does not match --platform");
    }
    if (reader.read<bool>() != options.difftest) {
        throw std::runtime_error("checkpoint differential-testing mode does not match this run");
    }
    if (reader.read<uint64_t>() != options.seed) {
        throw std::runtime_error("checkpoint simulation seed does not match --seed");
    }

    state.next_cycle = reader.read<uint64_t>();
    state.cycles_without_retirement = reader.read<uint64_t>();
    state.retired_instructions = reader.read<uint64_t>();
    state.measured_cycles = reader.read<uint64_t>();
    state.measured_instructions = reader.read<uint64_t>();
    state.architectural_state = reader.read<zircon::sim::SpikeArchitecturalState>();
    if (reader.read<bool>()) {
        state.measured_performance = reader.read<zircon::sim::PerformanceSnapshot>();
    } else {
        state.measured_performance.reset();
    }
    state.capture_measured_performance = reader.read<bool>();
    state.reference_complete = reader.read<bool>();
    state.last_retire_pc = reader.read<uint32_t>();
    state.simulation_time = reader.read<uint64_t>();
    const uint64_t branchCount = reader.readCount(1 << 24, "branch profile");
    state.branch_profile.clear();
    state.branch_profile.reserve(static_cast<size_t>(branchCount));
    for (uint64_t index = 0; index < branchCount; ++index) {
        const uint32_t pc = reader.read<uint32_t>();
        state.branch_profile.emplace(pc, reader.read<BranchProfileEntry>());
    }
    if (reader.read<bool>()) {
        state.pending_branch = reader.read<PendingBranch>();
    } else {
        state.pending_branch.reset();
    }
    state.recent_commits = reader.read<std::array<RecentCommit, 16>>();
    state.recent_commit_count = static_cast<size_t>(reader.read<uint64_t>());
    state.checkpoint_marker_seen = reader.read<bool>();
    state.waiting_after_wfi = reader.read<bool>();
    statistic.restore(reader);
    image.memory().restore(reader);
    memory.restore(reader);
    const bool hasPlatform = reader.read<bool>();
    if (hasPlatform != platform.has_value()) {
        throw std::runtime_error("checkpoint device platform does not match --platform");
    }
    if (platform.has_value()) {
        platform->restore(reader);
    }
    const bool hasReference = reader.read<bool>();
    if (hasReference != (reference != nullptr)) {
        throw std::runtime_error("checkpoint Spike state does not match differential-testing mode");
    }
    if (reference != nullptr) {
        reference->restore(reader);
    }
    reader.requireEnd();

    VerilatedRestore rtl;
    rtl.open(rtlPath.string());
    if (!rtl.isOpen()) {
        throw std::runtime_error("cannot open checkpoint RTL state: " + rtlPath.string());
    }
    rtl >> dut;
    rtl.close();
}
#endif

} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parseOptions(argc, argv);
        auto image = zircon::sim::ElfImage::load(options.elf);
#ifdef ZIRCON_ENABLE_CHECKPOINTS
        const uint64_t image_fingerprint = zircon::sim::fileFingerprint(options.elf);
#endif
        const auto tohost = image.symbol("tohost");
        if (!tohost.has_value() && options.platform == PlatformMode::Baremetal) {
            throw std::runtime_error("ELF does not define tohost");
        }
        const std::string initialUartInput = options.uart_input.empty() ? std::string{} : readFile(options.uart_input);
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
        std::optional<zircon::sim::PlatformDevices> platform;
        if (options.platform == PlatformMode::Linux) {
            platform.emplace(true, initialUartInput);
        }
        const std::optional<uint32_t> exitAddress = options.platform == PlatformMode::Baremetal ? tohost : std::nullopt;
        AXIMemory memory(image.memory(), exitAddress, options.seed, platform.has_value() ? &*platform : nullptr);
        NonblockingInput terminal(options.uart_stdio);
        zircon::sim::Statistic statistic;
        std::unique_ptr<zircon::sim::SpikeReference> reference;
        if (options.difftest) {
            reference = std::make_unique<zircon::sim::SpikeReference>(
                image.memory(), image.entry(), options.platform == PlatformMode::Linux, initialUartInput);
        }
        SimulationState simulation_state;
        simulation_state.last_retire_pc = image.entry();
        auto &cycles_without_retirement = simulation_state.cycles_without_retirement;
        auto &retired_instructions = simulation_state.retired_instructions;
        auto &measured_cycles = simulation_state.measured_cycles;
        auto &measured_instructions = simulation_state.measured_instructions;
        auto &architectural_state = simulation_state.architectural_state;
        auto &measured_performance = simulation_state.measured_performance;
        auto &capture_measured_performance = simulation_state.capture_measured_performance;
        auto &reference_complete = simulation_state.reference_complete;
        auto &last_retire_pc = simulation_state.last_retire_pc;
        auto &simulation_time = simulation_state.simulation_time;
        auto &branch_profile = simulation_state.branch_profile;
        auto &pending_branch = simulation_state.pending_branch;
        auto &recent_commits = simulation_state.recent_commits;
        auto &recent_commit_count = simulation_state.recent_commit_count;
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

        if (options.checkpoint_load.empty()) {
            driveInterrupts(dut, memory.timerInterrupt());
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
        } else {
#ifdef ZIRCON_ENABLE_CHECKPOINTS
            restoreCheckpoint(options.checkpoint_load, options, image_fingerprint, dut, image, memory, platform,
                              reference.get(), statistic, simulation_state);
            std::cerr << "CHECKPOINT restored " << options.checkpoint_load << " at cycle "
                      << grouped(simulation_state.next_cycle) << '\n';
#endif
        }
        if (simulation_state.next_cycle > options.max_cycles) {
            throw std::invalid_argument("--max-cycles is below the restored checkpoint cycle");
        }
        const uint64_t run_start_cycle = simulation_state.next_cycle;
        const auto simulation_start = std::chrono::steady_clock::now();
        std::atomic<uint64_t> progress_cycles = simulation_state.next_cycle;
        std::atomic<uint64_t> progress_instructions = retired_instructions;
        const bool progress_enabled = options.progress && !options.json;
        ProgressReporter progress_reporter(progress_cycles, progress_instructions, progress_enabled, color,
                                           options.progress_interval_seconds);

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

        for (uint64_t cycle = simulation_state.next_cycle; cycle < options.max_cycles; ++cycle) {
            if (options.uart_stdio && (cycle & 0x3ffu) == 0) {
                const std::string terminalInput = terminal.poll();
                if (!terminalInput.empty()) {
                    memory.appendUartInput(terminalInput);
                    if (reference != nullptr) {
                        reference->appendUartInput(terminalInput);
                    }
                }
            }
            driveInterrupts(dut, memory.timerInterrupt());
            memory.drive(dut);
            dut.clock = 0;
            dut.eval();
            const bool dump_wave = waveEnabled(options, cycle);
            dumpTrace(dump_wave, simulation_time);
            ++simulation_time;
            const auto result = memory.update(dut);

            std::optional<zircon::sim::SpikeTrap> pending_trap;
            if (reference != nullptr) {
                reference->setTime(memory.platformTime());
                if (dut.io_debug_trap_valid) {
                    pending_trap = zircon::sim::SpikeTrap{
                        dut.io_debug_trap_bits_cause,
                        dut.io_debug_trap_bits_epc,
                        dut.io_debug_trap_bits_tval,
                        static_cast<uint8_t>(dut.io_debug_trap_bits_targetPrivilege),
                    };
                }
            }

            const bool retire_valid[3] = {
                static_cast<bool>(dut.io_debug_retire_0_valid),
                static_cast<bool>(dut.io_debug_retire_1_valid),
                static_cast<bool>(dut.io_debug_retire_2_valid),
            };
            bool retired = false;
            bool lastRetiredWasWfi = false;
            for (size_t lane = 0; lane < 3; ++lane) {
                if (retire_valid[lane]) {
                    uint32_t retire_pc = 0;
                    uint32_t retire_instruction = 0;
                    uint8_t retire_rd = 0;
                    bool retire_is_fp = false;
                    bool retire_write_valid = false;
                    bool retire_mispredicted = false;
                    uint32_t retire_value = 0;
                    switch (lane) {
                    case 0:
                        retire_pc = dut.io_debug_retire_0_pc;
                        retire_instruction = dut.io_debug_retire_0_instruction;
                        retire_mispredicted = dut.io_debug_retire_0_mispredicted;
                        retire_rd = dut.io_debug_retire_0_rd;
                        retire_is_fp = dut.io_debug_retire_0_isFp;
                        retire_write_valid = dut.io_debug_retire_0_writeValid;
                        retire_value = dut.io_debug_retire_0_value;
                        break;
                    case 1:
                        retire_pc = dut.io_debug_retire_1_pc;
                        retire_instruction = dut.io_debug_retire_1_instruction;
                        retire_mispredicted = dut.io_debug_retire_1_mispredicted;
                        retire_rd = dut.io_debug_retire_1_rd;
                        retire_is_fp = dut.io_debug_retire_1_isFp;
                        retire_write_valid = dut.io_debug_retire_1_writeValid;
                        retire_value = dut.io_debug_retire_1_value;
                        break;
                    case 2:
                        retire_pc = dut.io_debug_retire_2_pc;
                        retire_instruction = dut.io_debug_retire_2_instruction;
                        retire_mispredicted = dut.io_debug_retire_2_mispredicted;
                        retire_rd = dut.io_debug_retire_2_rd;
                        retire_is_fp = dut.io_debug_retire_2_isFp;
                        retire_write_valid = dut.io_debug_retire_2_writeValid;
                        retire_value = dut.io_debug_retire_2_value;
                        break;
                    }
                    retired = true;
                    lastRetiredWasWfi = retire_instruction == 0x10500073u;
                    if (options.branch_profile) {
                        if (pending_branch.has_value()) {
                            branch_profile[pending_branch->pc].taken += retire_pc != pending_branch->pc + 4;
                            pending_branch.reset();
                        }
                        if ((retire_instruction & 0x7fu) == 0x63u) {
                            auto &entry = branch_profile[retire_pc];
                            entry.instruction = retire_instruction;
                            ++entry.executions;
                            entry.mispredictions += retire_mispredicted;
                            pending_branch = PendingBranch{retire_pc};
                        }
                    }
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
                        recent_commits[recent_commit_count % recent_commits.size()] = RecentCommit{
                            cycle + 1,
                            retire_pc,
                            retire_instruction,
                            expected.pc,
                            expected.instruction,
                            expected.next_pc,
                            static_cast<uint8_t>(lane),
                        };
                        ++recent_commit_count;
                        const bool synchronize =
                            zircon::sim::SpikeReference::requiresSynchronization(retire_instruction);
                        const bool destinationMismatch =
                            expected.write_valid != retire_write_valid ||
                            (expected.write_valid && (expected.is_fp != retire_is_fp || expected.rd != retire_rd ||
                                                      expected.value != retire_value));
                        if (expected.pc != retire_pc || expected.instruction != retire_instruction ||
                            (!synchronize && destinationMismatch)) {
                            const std::string csr_context = csrContext(dut);
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
                            std::cerr << std::dec << '\n';
                            printRecentCommits(recent_commits, recent_commit_count);
                            std::cerr << "Trap: valid=" << static_cast<unsigned>(dut.io_debug_trap_valid)
                                      << " cause=" << hex32(dut.io_debug_trap_bits_cause)
                                      << " epc=" << hex32(dut.io_debug_trap_bits_epc)
                                      << " tval=" << hex32(dut.io_debug_trap_bits_tval) << " targetPrivilege="
                                      << static_cast<unsigned>(dut.io_debug_trap_bits_targetPrivilege) << '\n'
                                      << "Registers: x13=" << hex32(architectural_state.integer[13])
                                      << " x14=" << hex32(architectural_state.integer[14])
                                      << " x15=" << hex32(architectural_state.integer[15])
                                      << " tp=" << hex32(architectural_state.integer[4])
                                      << "; Spike x14=" << hex32(reference->integerRegister(14))
                                      << " tp=" << hex32(reference->integerRegister(4)) << '\n'
                                      << "CSR: " << csr_context << std::endl;
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
                        if (store.has_value() && tohost.has_value() && storeCoversAddress(*store, *tohost)) {
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
            if (pending_trap.has_value()) {
                try {
                    if ((pending_trap->cause & 0x80000000u) != 0) {
                        reference->injectInterrupt(*pending_trap);
                    } else if (pending_trap->cause == 12u || pending_trap->cause == 13u || pending_trap->cause == 15u) {
                        reference->injectPageFault(*pending_trap);
                    }
                } catch (const std::exception &error) {
                    progress_reporter.finish();
                    std::cerr << ansi(color, kAnsiRed) << "DIFFTEST TRAP FAILED" << ansi(color, kAnsiReset)
                              << " at instruction " << retired_instructions << ", cycle " << cycle + 1 << ": "
                              << error.what() << '\n'
                              << "DUT trap: cause=" << hex32(pending_trap->cause) << " epc=" << hex32(pending_trap->epc)
                              << " tval=" << hex32(pending_trap->tval)
                              << " targetPrivilege=" << static_cast<unsigned>(pending_trap->target_privilege) << '\n'
                              << "DUT CSR: " << csrContext(dut) << '\n'
                              << "Spike: " << reference->diagnosticContext() << '\n'
                              << "ROB: valid=" << static_cast<unsigned>(dut.io_debug_robHeadValid)
                              << " complete=" << static_cast<unsigned>(dut.io_debug_robHeadComplete)
                              << " pc=" << hex32(dut.io_debug_robHeadPc) << '\n';
                    printRecentCommits(recent_commits, recent_commit_count);
                    dut.final();
                    closeTrace();
                    return 126;
                }
            }
            if (retired) {
                simulation_state.waiting_after_wfi = lastRetiredWasWfi;
            }
            if (pending_trap.has_value()) {
                simulation_state.waiting_after_wfi = false;
            }
            cycles_without_retirement =
                retired || simulation_state.waiting_after_wfi ? 0 : cycles_without_retirement + 1;
            if (progress_enabled && (cycle & 0x3fffu) == 0) {
                progress_cycles.store(cycle + 1, std::memory_order_relaxed);
                progress_instructions.store(retired_instructions, std::memory_order_relaxed);
            }

            dut.clock = 1;
            dut.eval();
            memory.tick();
            if (capture_measured_performance) {
                measured_performance = readPerformance(dut);
                capture_measured_performance = false;
            }
            dumpTrace(dump_wave, simulation_time);
            ++simulation_time;
            simulation_state.next_cycle = cycle + 1;
            const bool markerSeen = memory.outputContains(options.pass_marker);
            const bool checkpointMarkerSeen =
                !options.checkpoint_marker.empty() && memory.outputContains(options.checkpoint_marker);
            const bool checkpointTriggered =
                !result.has_value() && !markerSeen &&
                ((!options.checkpoint_save.empty() && options.checkpoint_cycle == cycle + 1) ||
                 (!options.checkpoint_save.empty() && options.checkpoint_interval != 0 &&
                  (cycle + 1) % options.checkpoint_interval == 0) ||
                 (checkpointMarkerSeen && !simulation_state.checkpoint_marker_seen));
            if (checkpointMarkerSeen) {
                simulation_state.checkpoint_marker_seen = true;
            }
            if (checkpointTriggered) {
#ifdef ZIRCON_ENABLE_CHECKPOINTS
                saveCheckpoint(options.checkpoint_save, options, image_fingerprint, dut, image, memory, platform,
                               reference.get(), statistic, simulation_state);
                std::cerr << "CHECKPOINT saved " << options.checkpoint_save << " at cycle " << grouped(cycle + 1)
                          << '\n';
#endif
            }
            if (result.has_value() || markerSeen) {
                if (progress_enabled) {
                    progress_cycles.store(cycle + 1, std::memory_order_relaxed);
                    progress_instructions.store(retired_instructions, std::memory_order_relaxed);
                }
                const uint64_t report_cycles = measured_cycles == 0 ? cycle + 1 : measured_cycles;
                progress_reporter.finish();
                const RunMetrics metrics = collectMetrics(report_cycles, cycle + 1 - run_start_cycle);
                const std::string report = writeReport(report_cycles, metrics);
                if (options.branch_profile) {
                    printBranchProfile(branch_profile);
                    const auto performance = measured_performance.value_or(readPerformance(dut));
                    std::cerr << "Loop predictor: training=" << performance.loopTraining
                              << " provider=" << performance.loopProvider << " correct=" << performance.loopCorrect
                              << '\n';
                }
                dut.final();
                closeTrace();
                if (human_output) {
                    const bool passed = markerSeen || *result == 0;
                    printSummary(passed ? RunOutcome::Pass : RunOutcome::Fail,
                                 markerSeen
                                     ? "Linux boot marker observed"
                                     : (passed ? "Program completed successfully" : "Program returned a failure code"),
                                 options, report_cycles, metrics, report,
                                 passed ? "" : "tohost exit code " + std::to_string(*result), color);
                } else {
                    std::cout << "{\"status\":\"exit\",\"code\":" << (markerSeen ? 0 : *result)
                              << ",\"cycles\":" << report_cycles;
                    printJsonMetrics(metrics);
                    std::cout << ",\"seed\":" << options.seed << ",\"report\":\"" << report << "\"}" << std::endl;
                }
                return markerSeen ? 0 : *result;
            }
            if (options.stall_cycles != 0 && cycles_without_retirement >= options.stall_cycles) {
                if (progress_enabled) {
                    progress_cycles.store(cycle + 1, std::memory_order_relaxed);
                    progress_instructions.store(retired_instructions, std::memory_order_relaxed);
                }
                const uint64_t report_cycles = measured_cycles == 0 ? cycle + 1 : measured_cycles;
                const std::string csr_context = csrContext(dut);
                progress_reporter.finish();
                const RunMetrics metrics = collectMetrics(report_cycles, cycle + 1 - run_start_cycle);
                const std::string report = writeReport(report_cycles, metrics);
                dut.final();
                closeTrace();
                if (human_output) {
                    printSummary(RunOutcome::Stall, "No instruction retired within the stall window", options,
                                 report_cycles, metrics, report,
                                 grouped(cycles_without_retirement) + " cycles without retirement; last PC " +
                                     hex32(last_retire_pc) + "; ROB head " + hex32(dut.io_debug_robHeadPc) +
                                     "; CSR: " + csr_context,
                                 color);
                } else {
                    std::cout
                        << "{\"status\":\"stalled\",\"cycles\":" << report_cycles
                        << ",\"noRetireCycles\":" << cycles_without_retirement << ",\"lastRetirePc\":" << last_retire_pc
                        << ",\"robHeadValid\":" << static_cast<unsigned>(dut.io_debug_robHeadValid)
                        << ",\"robHeadComplete\":" << static_cast<unsigned>(dut.io_debug_robHeadComplete)
                        << ",\"robHeadPc\":" << dut.io_debug_robHeadPc << ",\"a0\":" << architectural_state.integer[10]
                        << ",\"memoryIdle\":" << static_cast<unsigned>(dut.io_debug_memoryIdle)
                        << ",\"l2Idle\":" << static_cast<unsigned>(dut.io_debug_l2Idle)
                        << ",\"dcacheIdle\":" << static_cast<unsigned>(dut.io_debug_backend_dcacheIdle)
                        << ",\"atomicState\":" << static_cast<unsigned>(dut.io_debug_backend_atomicState)
                        << ",\"atomicRequestValid\":" << static_cast<unsigned>(dut.io_debug_backend_atomicRequestValid)
                        << ",\"atomicRequestReady\":" << static_cast<unsigned>(dut.io_debug_backend_atomicRequestReady)
                        << ",\"atomicLoadRequestValid\":"
                        << static_cast<unsigned>(dut.io_debug_backend_atomicLoadRequestValid)
                        << ",\"atomicLoadResponseValid\":"
                        << static_cast<unsigned>(dut.io_debug_backend_atomicLoadResponseValid)
                        << ",\"atomicStoreRequestValid\":"
                        << static_cast<unsigned>(dut.io_debug_backend_atomicStoreRequestValid)
                        << ",\"atomicStoreResponseValid\":"
                        << static_cast<unsigned>(dut.io_debug_backend_atomicStoreResponseValid)
                        << ",\"dcacheRequestBufferValid\":"
                        << static_cast<unsigned>(dut.io_debug_backend_dcache_requestBufferValid)
                        << ",\"dcacheLookupValid\":" << static_cast<unsigned>(dut.io_debug_backend_dcache_lookupValid)
                        << ",\"dcacheLookupFresh\":" << static_cast<unsigned>(dut.io_debug_backend_dcache_lookupFresh)
                        << ",\"dcacheExecuteValid\":" << static_cast<unsigned>(dut.io_debug_backend_dcache_executeValid)
                        << ",\"dcacheResponseValid\":"
                        << static_cast<unsigned>(dut.io_debug_backend_dcache_responseValid)
                        << ",\"dcacheForwardQueryValid\":"
                        << static_cast<unsigned>(dut.io_debug_backend_dcache_forwardQueryValid)
                        << ",\"dcacheForwardResultValid\":"
                        << static_cast<unsigned>(dut.io_debug_backend_dcache_forwardResultValid)
                        << ",\"dcacheLookupResponseMatch\":"
                        << static_cast<unsigned>(dut.io_debug_backend_dcache_lookupResponseMatch)
                        << ",\"dcacheLookupMove\":" << static_cast<unsigned>(dut.io_debug_backend_dcache_lookupMove)
                        << ",\"dcacheExecuteRelease\":"
                        << static_cast<unsigned>(dut.io_debug_backend_dcache_executeRelease)
                        << ",\"dcacheMissBusy\":" << static_cast<unsigned>(dut.io_debug_backend_dcache_missBusy)
                        << ",\"dcacheStoreState\":" << static_cast<unsigned>(dut.io_debug_backend_dcache_storeState)
                        << ",\"dcacheFlush\":" << static_cast<unsigned>(dut.io_debug_backend_dcache_flush)
                        << ",\"csr\":\"" << csr_context << "\"";
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
        const RunMetrics metrics = collectMetrics(report_cycles, options.max_cycles - run_start_cycle);
        const std::string report = writeReport(report_cycles, metrics);
        dut.final();
        closeTrace();
        if (human_output) {
            printSummary(RunOutcome::Timeout, "Maximum cycle limit reached", options, report_cycles, metrics, report,
                         "limit " + grouped(options.max_cycles) + " cycles; last PC " + hex32(last_retire_pc), color);
        } else {
            std::cout << "{\"status\":\"timeout\",\"cycles\":" << report_cycles;
            printJsonMetrics(metrics);
            std::cout << ",\"seed\":" << options.seed << ",\"entry\":" << image.entry()
                      << ",\"tohost\":" << tohost.value_or(0) << ",\"lastPc\":" << last_retire_pc << ",\"report\":\""
                      << report << "\"}" << std::endl;
        }
        return options.allow_timeout ? 0 : 124;
    } catch (const std::exception &error) {
        const bool color = isatty(STDERR_FILENO) && std::getenv("NO_COLOR") == nullptr;
        std::cerr << ansi(color, kAnsiRed) << "zircon-sim: " << error.what() << ansi(color, kAnsiReset) << std::endl;
        return 2;
    }
}
