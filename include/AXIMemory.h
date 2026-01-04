#ifndef AXIMEMORY_HH
#define AXIMEMORY_HH

#include <cstdint>
#include <unordered_map>
#include <fstream>
#include "Device.h"
#include "Config.h"

#ifndef USE_SIMULATOR_ONLY
#include <verilated_vcd_c.h>
#include "VCPU.h"
#endif

// LSU 操作码定义
namespace LSUOp {
    // load
    constexpr uint8_t LB  = 0x10;
    constexpr uint8_t LH  = 0x11;
    constexpr uint8_t LW  = 0x12;
    constexpr uint8_t LBU = 0x14;
    constexpr uint8_t LHU = 0x15;
    constexpr uint8_t FLW = 0x54;
    // store
    constexpr uint8_t SB  = 0x20;
    constexpr uint8_t SH  = 0x21;
    constexpr uint8_t SW  = 0x22;
    constexpr uint8_t FSW = 0x62;
    
    inline bool isLoad(uint8_t op) {
        return op == LB || op == LH || op == LW || op == LBU || op == LHU || op == FLW;
    }
    inline bool isStore(uint8_t op) {
        return op == SB || op == SH || op == SW || op == FSW;
    }
}

class AXIMemory {
    private:
    std::unordered_map<uint32_t, uint32_t> memory;
    std::unordered_map<uint32_t, uint32_t> refMemory;
    Device* device = nullptr;
    uint32_t byteMasks[4] = {0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000};

    public:
    AXIMemory(std::string imgPath, uint32_t baseAddr, Device* device);
#ifndef USE_SIMULATOR_ONLY
    void imemRead(VCPU* cpu);   // 取指内存访问
    void dmemAccess(VCPU* cpu); // 数据内存访问（两个LSU）
#endif
    uint32_t debugRead(uint32_t addr);
    uint32_t refMemoryRead(uint32_t addr);
    void refMemoryWrite(uint32_t addr, uint32_t data, uint8_t wstrb);

};

#endif
