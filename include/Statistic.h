#ifndef STATISTIC_HH
#define STATISTIC_HH

#include <cstdint>
#include "AXIMemory.h"
#include "Simulator.h"
#include "Config.h"

#ifndef USE_SIMULATOR_ONLY
#include "verilated_vcd_c.h"
#include "VCPU.h"
#endif

class Statistic {
    private:
    uint32_t cycles = 0;
    uint32_t insts  = 0;
    // instructiong buffer
    uint32_t pcRingBuffer[8];
    uint8_t pcRingBufferIndex = 0;



    public:
    
    inline void addCycles(uint32_t num) {
        cycles += num;
    }
    inline void addInsts(uint32_t num) {
        insts += num;
    }
    inline uint32_t getCycles() {
        return cycles;
    }
    inline uint32_t getInsts() {
        return insts;
    }

    inline double getIPC() {
        return insts * 1.0 / cycles;
    }
    void printPerformance();

    void pcBufferPush(uint32_t pc) {
        pcRingBuffer[pcRingBufferIndex] = pc;
        pcRingBufferIndex = (pcRingBufferIndex + 1) % 8;
    }

    void printLastInstrucions(AXIMemory* mem);
#ifndef USE_SIMULATOR_ONLY
    void printMarkdownReport(VCPU* cpu, std::string imgName, Simulator* sim);
#endif

    
};

#endif