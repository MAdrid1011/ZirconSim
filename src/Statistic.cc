#include "Statistic.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace zircon::sim {
namespace {

double ratio(uint64_t numerator, uint64_t denominator) {
    return denominator == 0 ? 0.0 : 100.0 * static_cast<double>(numerator) / denominator;
}

void writeCount(std::ofstream &output, const char *name, uint64_t count, uint64_t total) {
    output << "| " << name << " | " << count << " | " << ratio(count, total) << "% |\n";
}

void writeCycleValue(std::ofstream &output, uint64_t count, uint64_t cycles) {
    output << count << " (" << ratio(count, cycles) << "%)";
}

void writeCache(
    std::ofstream &output,
    const char *name,
    uint64_t visits,
    uint64_t hits,
    uint64_t misses,
    uint64_t retries
) {
    output << "| " << name << " | " << visits << " | " << hits << " | " << misses << " | "
           << retries << " | " << ratio(hits, hits + misses) << "% |\n";
}

std::string reportName(const std::string &elf) {
    std::string name = std::filesystem::path(elf).stem().string();
    for (char &character : name) {
        const bool allowed = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
            character == '-' || character == '_';
        if (!allowed) {
            character = '_';
        }
    }
    return name.empty() ? "program" : name;
}

} // namespace

void Statistic::observeInstruction(uint32_t instruction) {
    const uint32_t opcode = instruction & 0x7f;
    const uint32_t rd = instruction >> 7 & 0x1f;
    const uint32_t funct3 = instruction >> 12 & 0x7;
    const uint32_t rs1 = instruction >> 15 & 0x1f;
    const uint32_t funct7 = instruction >> 25;
    switch (opcode) {
        case 0x03:
        case 0x07:
            ++instructions_.load;
            break;
        case 0x23:
        case 0x27:
            ++instructions_.store;
            break;
        case 0x63:
            ++instructions_.branch;
            ++instructions_.conditionalBranch;
            break;
        case 0x6f:
            ++instructions_.jump;
            if (rd == 1 || rd == 5) {
                ++instructions_.call;
            }
            break;
        case 0x67:
            ++instructions_.jump;
            if (rd == 0 && (rs1 == 1 || rs1 == 5)) {
                ++instructions_.ret;
            } else if (rd == 1 || rd == 5) {
                ++instructions_.call;
            } else {
                ++instructions_.indirectJump;
            }
            break;
        case 0x33:
            if (funct7 == 1) {
                if (funct3 < 4) {
                    ++instructions_.multiply;
                } else {
                    ++instructions_.divide;
                }
            } else {
                ++instructions_.alu;
            }
            break;
        case 0x53:
        case 0x43:
        case 0x47:
        case 0x4b:
        case 0x4f:
            ++instructions_.floating;
            break;
        case 0x0f:
        case 0x73:
            ++instructions_.system;
            break;
        case 0x13:
        case 0x17:
        case 0x37:
            ++instructions_.alu;
            break;
        default:
            ++instructions_.other;
            break;
    }
}

const InstructionStatistic &Statistic::instructions() const {
    return instructions_;
}

void Statistic::setPerformance(const PerformanceSnapshot &performance) {
    performance_ = performance;
}

std::string Statistic::writeMarkdownReport(
    const std::string &elf,
    uint64_t cycles,
    uint64_t retiredInstructions,
    double elapsedSeconds,
    double cyclesPerSecond
) const {
    const std::filesystem::path directory = "reports";
    std::filesystem::create_directories(directory);
    const std::filesystem::path path = directory / ("report-" + reportName(elf) + ".md");
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("failed to create statistics report: " + path.string());
    }
    output << std::fixed << std::setprecision(3);
    const double ipc = cycles == 0 ? 0.0 : static_cast<double>(retiredInstructions) / cycles;
    output << "## 程序基本情况\n"
           << "| 程序名 | 总周期数 | 总指令数 | IPC | 仿真时间（秒） | 仿真速度（周期/秒） |\n"
           << "| --- | --- | --- | --- | --- | --- |\n"
           << "| " << std::filesystem::path(elf).filename().string() << " | " << cycles << " | "
           << retiredInstructions << " | " << ipc << " | " << elapsedSeconds << " | "
           << cyclesPerSecond << " |\n";

    output << "## 指令统计\n"
           << "| 指令类型 | 总数 | 占比 |\n"
           << "| --- | --- | --- |\n";
    writeCount(output, "ALU", instructions_.alu, retiredInstructions);
    writeCount(output, "Branch", instructions_.branch + instructions_.jump, retiredInstructions);
    writeCount(output, "Load", instructions_.load, retiredInstructions);
    writeCount(output, "Store", instructions_.store, retiredInstructions);
    writeCount(output, "Mul", instructions_.multiply, retiredInstructions);
    writeCount(output, "Div", instructions_.divide, retiredInstructions);
    writeCount(output, "Floating", instructions_.floating, retiredInstructions);
    writeCount(output, "System", instructions_.system, retiredInstructions);
    writeCount(output, "Other", instructions_.other, retiredInstructions);

    const uint64_t directJump = instructions_.jump - instructions_.call - instructions_.ret -
        instructions_.indirectJump;
    const uint64_t branchCorrect = instructions_.conditionalBranch - performance_.branchFail;
    const uint64_t directJumpCorrect = directJump - performance_.directJumpFail;
    const uint64_t callCorrect = instructions_.call - performance_.callFail;
    const uint64_t retCorrect = instructions_.ret - performance_.retFail;
    const uint64_t indirectCorrect = instructions_.indirectJump - performance_.indirectFail;
    output << "## 分支预测\n"
           << "| 分支类型 | 总数 | 预测正确数 | 预测正确率 |\n"
           << "| --- | --- | --- | --- |\n"
           << "| Conditional Branch | " << instructions_.conditionalBranch << " | " << branchCorrect << " | "
           << ratio(branchCorrect, instructions_.conditionalBranch) << "% |\n"
           << "| Direct Jump | " << directJump << " | " << directJumpCorrect << " | "
           << ratio(directJumpCorrect, directJump) << "% |\n"
           << "| Call | " << instructions_.call << " | " << callCorrect << " | "
           << ratio(callCorrect, instructions_.call) << "% |\n"
           << "| Ret | " << instructions_.ret << " | " << retCorrect << " | "
           << ratio(retCorrect, instructions_.ret) << "% |\n"
           << "| Indirect Jump | " << instructions_.indirectJump << " | " << indirectCorrect << " | "
           << ratio(indirectCorrect, instructions_.indirectJump) << "% |\n";

    output << "## 高速缓存\n"
           << "### 缓存命中情况\n"
           << "| 高速缓存通道 | 访问次数 | 命中数 | 缺失数 | 重试数 | 命中率 |\n"
           << "| --- | --- | --- | --- | --- | --- |\n";
    writeCache(
        output,
        "ICache 取指",
        performance_.icacheVisit,
        performance_.icacheHit,
        performance_.icacheVisit - performance_.icacheHit,
        0
    );
    for (size_t lane = 0; lane < performance_.dcacheLoadVisits.size(); ++lane) {
        const char *name = lane == 0 ? "DCache LS0 Load" : "DCache LS1 Load";
        writeCache(
            output,
            name,
            performance_.dcacheLoadVisits[lane],
            performance_.dcacheLoadHits[lane],
            performance_.dcacheLoadMisses[lane],
            performance_.dcacheLoadRetries[lane]
        );
    }
    writeCache(
        output,
        "DCache Committed Store",
        performance_.dcacheStoreVisits,
        performance_.dcacheStoreHits,
        performance_.dcacheStoreMisses,
        0
    );
    writeCache(
        output,
        "L2 ICache 请求",
        performance_.l2InstructionVisits,
        performance_.l2InstructionHits,
        performance_.l2InstructionMisses,
        0
    );
    writeCache(
        output,
        "L2 DCache 请求",
        performance_.l2DataVisits,
        performance_.l2DataHits,
        performance_.l2DataMisses,
        0
    );
    output << "\n### 缓存替换情况\n"
           << "| L2/下级内存流量 | 次数 |\n"
           << "| --- | --- |\n"
           << "| ICache Victim 插入 L2 | " << performance_.l2InstructionVictimInsertions << " |\n"
           << "| DCache Victim 插入 L2 | " << performance_.l2DataVictimInsertions << " |\n"
           << "| L2 下级内存读 | " << performance_.lowerMemoryReads << " |\n"
           << "| L2 下级内存写回 | " << performance_.lowerMemoryWrites << " |\n";

    output << "## 流水线停顿\n"
           << "### 前端\n"
           << "| 停顿原因 | 停顿周期数 | 停顿率 |\n"
           << "| --- | --- | --- |\n";
    writeCount(output, "ICache缺失", performance_.icacheMissCycles, cycles);
    writeCount(output, "Fetch Queue满", performance_.fqBlockedCycles, cycles);
    writeCount(output, "Fetch Queue空", performance_.fqEmptyCycles, cycles);

    output << "### 中端\n"
           << "| 停顿原因 | 停顿周期数 | 停顿率 |\n"
           << "| --- | --- | --- |\n";
    writeCount(output, "整数物理寄存器不足", performance_.integerFreeListBlockedCycles, cycles);
    writeCount(output, "浮点物理寄存器不足", performance_.floatingFreeListBlockedCycles, cycles);
    writeCount(output, "FTQ满", performance_.ftqBlockedCycles, cycles);
    writeCount(output, "重排序缓存满", performance_.robFullCycles, cycles);
    writeCount(output, "派发阻塞", performance_.dispatchBlockedCycles, cycles);

    output << "### 后端\n"
           << "| 停顿原因 | 停顿周期数 | 停顿率 |\n"
           << "| --- | --- | --- |\n";
    static constexpr std::array<const char *, 6> queueNames = {
        "Arith0 IQ占满", "Arith1 IQ占满", "MixArith IQ占满",
        "LS0 Load IQ占满", "LS1 Load/Store Address IQ占满", "Store Data IQ占满",
    };
    static constexpr std::array<size_t, 6> queueIndices = {0, 1, 2, 3, 4, 5};
    for (size_t index = 0; index < queueNames.size(); ++index) {
        writeCount(output, queueNames[index], performance_.issueQueueFullCycles[queueIndices[index]], cycles);
    }
    writeCount(output, "除法器忙", performance_.divideBusyCycles, cycles);
    writeCount(output, "DCache Miss Unit忙", performance_.dcacheMissBusyCycles, cycles);
    writeCount(output, "L2 Miss/Victim引擎忙", performance_.l2EngineBusyCycles, cycles);
    writeCount(output, "Store Buffer满", performance_.storeBufferFullCycles, cycles);

    output << "## 流水线发射情况\n"
           << "| 流水线 | 发射周期数 | 等待操作数周期数 | Replay阻塞周期数 | 执行资源阻塞周期数 |\n"
           << "| --- | --- | --- | --- | --- |\n";
    static constexpr std::array<const char *, 6> pipelineNames = {
        "Arith0", "Arith1", "MixArith", "LS0 Load", "LS1 Load/STA", "LS1 Store Data",
    };
    for (size_t index = 0; index < pipelineNames.size(); ++index) {
        output << "| " << pipelineNames[index] << " | ";
        writeCycleValue(output, performance_.pipelineIssueCycles[index], cycles);
        output << " | ";
        writeCycleValue(output, performance_.pipelineOperandWaitCycles[index], cycles);
        output << " | ";
        writeCycleValue(output, performance_.pipelineReplayBlockedCycles[index], cycles);
        output << " | ";
        writeCycleValue(output, performance_.pipelineExecutionBlockedCycles[index], cycles);
        output << " |\n";
    }
    return path.string();
}

} // namespace zircon::sim
