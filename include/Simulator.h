#ifndef SIMULATOR_HH
#define SIMULATOR_HH

#include <cstdint>
#include "AXIMemory.h"


struct InstStatistic {
    uint32_t aluInsts = 0;
    uint32_t branchInsts = 0;
    uint32_t loadInsts = 0;
    uint32_t storeInsts = 0;
    uint32_t mulInsts = 0;
    uint32_t divInsts = 0;
};

class Simulator {
    private:
    uint32_t pc = 0x80000000;
    uint32_t rf[32] = {0};
    AXIMemory* memory = nullptr;
    uint32_t bits(uint32_t value, uint32_t hi, uint32_t lo){
        return (value >> lo) & ~((-1) << (hi - lo + 1));
    }
    uint32_t signExtend(uint32_t value, uint32_t width){
        if (bits(value, width - 1, width - 1) == 1) {
            return value | ((-1) << width);
        } else {
            return value & ~((-1) << width);
        }
    }
    uint32_t zeroExtend(uint32_t value, uint32_t width){
        return value & ~((-1) << width);
    }
    void executeRType(uint32_t inst);
    void executeIType(uint32_t inst);
    void executeBType(uint32_t inst);
    void executeSType(uint32_t inst);
    void executeJType(uint32_t inst);
    void executeUType(uint32_t inst);

    // instruction type
    InstStatistic instStat;

    public:
    Simulator(AXIMemory* memory): memory(memory) {}

    void step(uint32_t num);
    uint32_t getPC(){
        return pc;
    }
    uint32_t getRf(uint8_t rd){
        return rf[rd];
    }
    InstStatistic getInstStat(){
        return instStat;
    }
};

#endif
