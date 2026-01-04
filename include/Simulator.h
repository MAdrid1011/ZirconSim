#ifndef SIMULATOR_HH
#define SIMULATOR_HH

#include <cstdint>
#include <cstring>
#include "AXIMemory.h"

// VLIW配置
#define VLIW_PACK_SIZE 8

struct InstStatistic {
    uint32_t aluInsts = 0;
    uint32_t branchInsts = 0;
    uint32_t loadInsts = 0;
    uint32_t storeInsts = 0;
    uint32_t mulInsts = 0;
    uint32_t divInsts = 0;
    uint32_t fpuInsts = 0;
    uint32_t fLoadInsts = 0;
    uint32_t fStoreInsts = 0;
};

// 写回缓冲区条目
struct WriteBackEntry {
    bool valid = false;      // 是否有效
    bool isFloat = false;    // 是否是浮点寄存器
    uint8_t rd = 0;          // 目标寄存器
    uint32_t intValue = 0;   // 整数值
    float floatValue = 0.0f; // 浮点值
};

class Simulator {
    private:
    uint32_t pc = 0x80000000;
    uint32_t rf[32] = {0};
    float fpr[32] = {0.0f};
    AXIMemory* memory = nullptr;
    
    // VLIW写回缓冲区
    WriteBackEntry writeBackBuffer[VLIW_PACK_SIZE];
    uint32_t vliwInstIndex = 0;  // 当前包内指令索引
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
    void executeFType(uint32_t inst);
    void executeR4Type(uint32_t inst);

    // VLIW写回辅助函数
    void stageIntWrite(uint8_t rd, uint32_t value);   // 暂存整数寄存器写回
    void stageFloatWrite(uint8_t rd, float value);    // 暂存浮点寄存器写回
    void commitWriteBack();                            // 提交写回（包执行完成后调用）
    void clearWriteBackBuffer();                       // 清空写回缓冲区

    // instruction type
    InstStatistic instStat;

    public:
    Simulator(AXIMemory* memory): memory(memory) {
        clearWriteBackBuffer();
    }

    void step(uint32_t num);        // 执行单条指令（用于VLIW包内）
    void stepVLIWPack();            // 执行一个完整的VLIW包（8条指令）
    uint32_t getPC(){
        return pc;
    }
    uint32_t getRf(uint8_t rd){
        return rf[rd];
    }
    float getFpr(uint8_t rd){
        return fpr[rd];
    }
    InstStatistic getInstStat(){
        return instStat;
    }
    bool isSimEnd(){
        uint32_t inst = memory->refMemoryRead(pc);
        return inst == 0x80000000;  // dead instruction
    }
};

#endif