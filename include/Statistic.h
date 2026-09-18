#ifndef ZIRCON_SIM_STATISTIC_H
#define ZIRCON_SIM_STATISTIC_H

#include <array>
#include <cstdint>
#include <string>

namespace zircon::sim {

struct InstructionStatistic {
    uint64_t alu = 0;
    uint64_t branch = 0;
    uint64_t jump = 0;
    uint64_t load = 0;
    uint64_t store = 0;
    uint64_t multiply = 0;
    uint64_t divide = 0;
    uint64_t floating = 0;
    uint64_t system = 0;
    uint64_t other = 0;
    uint64_t conditionalBranch = 0;
    uint64_t call = 0;
    uint64_t ret = 0;
    uint64_t indirectJump = 0;
};

struct PerformanceSnapshot {
    uint64_t icacheVisit = 0;
    uint64_t icacheHit = 0;
    uint64_t icacheMissCycles = 0;
    uint64_t fqBlockedCycles = 0;
    uint64_t fqEmptyCycles = 0;
    uint64_t ftqBlockedCycles = 0;
    uint64_t integerFreeListBlockedCycles = 0;
    uint64_t floatingFreeListBlockedCycles = 0;
    uint64_t dispatchBlockedCycles = 0;
    uint64_t branch = 0;
    uint64_t branchFail = 0;
    uint64_t directJump = 0;
    uint64_t directJumpFail = 0;
    uint64_t call = 0;
    uint64_t callFail = 0;
    uint64_t ret = 0;
    uint64_t retFail = 0;
    uint64_t indirect = 0;
    uint64_t indirectFail = 0;
    uint64_t loopTraining = 0;
    uint64_t loopProvider = 0;
    uint64_t loopCorrect = 0;
    uint64_t robFullCycles = 0;
    uint64_t storeBufferFullCycles = 0;
    uint64_t storeBufferBusyCycles = 0;
    std::array<uint64_t, 6> issueQueueFullCycles{};
    std::array<uint64_t, 6> pipelineIssueCycles{};
    std::array<uint64_t, 6> pipelineOperandWaitCycles{};
    std::array<uint64_t, 6> pipelineReplayBlockedCycles{};
    std::array<uint64_t, 6> pipelineExecutionBlockedCycles{};
    uint64_t divideBusyCycles = 0;
    std::array<uint64_t, 2> dcacheLoadVisits{};
    std::array<uint64_t, 2> dcacheLoadHits{};
    std::array<uint64_t, 2> dcacheLoadMisses{};
    std::array<uint64_t, 2> dcacheLoadRetries{};
    std::array<uint64_t, 2> dcacheLoadRetryTranslation{};
    std::array<uint64_t, 2> dcacheLoadRetryForwardBlocked{};
    std::array<uint64_t, 2> dcacheLoadRetryUncachedOrder{};
    std::array<uint64_t, 2> dcacheLoadRetryStaleLookup{};
    std::array<uint64_t, 2> dcacheLoadRetryMissBusy{};
    std::array<uint64_t, 2> dcacheLoadRetryStoreConflict{};
    std::array<uint64_t, 2> dcacheLoadRetryLaneConflict{};
    uint64_t dcacheStoreVisits = 0;
    uint64_t dcacheStoreHits = 0;
    uint64_t dcacheStoreMisses = 0;
    uint64_t dcacheMissBusyCycles = 0;
    uint64_t l2InstructionVisits = 0;
    uint64_t l2InstructionHits = 0;
    uint64_t l2InstructionMisses = 0;
    uint64_t l2DataVisits = 0;
    uint64_t l2DataHits = 0;
    uint64_t l2DataMisses = 0;
    uint64_t l2InstructionVictimInsertions = 0;
    uint64_t l2DataVictimInsertions = 0;
    uint64_t lowerMemoryReads = 0;
    uint64_t lowerMemoryWrites = 0;
    uint64_t l2EngineBusyCycles = 0;
};

class Statistic {
  public:
    void observeInstruction(uint32_t instruction);
    const InstructionStatistic &instructions() const;
    void setPerformance(const PerformanceSnapshot &performance);
    std::string writeMarkdownReport(
        const std::string &elf,
        uint64_t cycles,
        uint64_t retiredInstructions,
        double elapsedSeconds,
        double cyclesPerSecond
    ) const;

  private:
    InstructionStatistic instructions_;
    PerformanceSnapshot performance_;
};

} // namespace zircon::sim

#endif
