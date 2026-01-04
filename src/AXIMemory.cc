#include "AXIMemory.h"
#include "Config.h"

AXIMemory::AXIMemory(std::string imgPath, uint32_t baseAddr, Device* device) {
    this->device = device;

    // 在主存开始仿真之前，先初始化baseAddr位置（0x80000000）为死循环指令（0x80000000）
    // 这用于标识程序结束
    memory.emplace(baseAddr >> 2, 0x80000000);
    refMemory.emplace(baseAddr >> 2, 0x80000000);

    if(imgPath.empty()) {
        return;
    } 
    
    // open binary file
    std::ifstream img(imgPath, std::ios::binary);
    if(!img.is_open()) {
        return;
    }
    
    // read binary file to memory
    // 如果程序从baseAddr开始，会覆盖之前设置的死循环指令（这是正常的，程序的第一条指令应该在这里）
    uint32_t word;
    uint32_t addr = baseAddr >> 2;
    bool programStartsAtBase = false;  // 记录程序是否从baseAddr开始
    if(img.read(reinterpret_cast<char*>(&word), sizeof(uint32_t))) {
        // 程序的第一条指令加载到baseAddr位置
        memory[addr] = word;
        refMemory[addr] = word;
        programStartsAtBase = true;
        addr += 1;
        // 继续加载剩余的程序
        while(img.read(reinterpret_cast<char*>(&word), sizeof(uint32_t))) {
            memory[addr] = word;
            refMemory[addr] = word;
            addr += 1;
        }
    }
    img.close();
    
    // 在主存开始仿真之前，确保baseAddr位置（0x80000000）有死循环指令（0x80000000）
    // 这用于标识程序结束
    // 如果程序从baseAddr开始，保持程序的第一条指令（不覆盖）
    // 如果程序没有从baseAddr开始，设置死循环指令
    // 程序执行时，如果跳转到0x80000000就会检测到结束指令
    if(!programStartsAtBase) {
        // 如果程序没有从baseAddr开始，设置死循环指令
        memory[baseAddr >> 2] = 0x80000000;
        refMemory[baseAddr >> 2] = 0x80000000;
    }
    // 如果程序从baseAddr开始，保持程序的第一条指令不变
}

#ifndef USE_SIMULATOR_ONLY
void AXIMemory::imemRead(VCPU* cpu) {
    // 根据 PC 读取8条连续的指令
    uint32_t pc = cpu->io_imem_pc;
    uint32_t wordAddr = pc >> 2;
    
    // 读取8条指令
    uint32_t* insts[8] = {
        &cpu->io_imem_insts_0, &cpu->io_imem_insts_1,
        &cpu->io_imem_insts_2, &cpu->io_imem_insts_3,
        &cpu->io_imem_insts_4, &cpu->io_imem_insts_5,
        &cpu->io_imem_insts_6, &cpu->io_imem_insts_7
    };
    
    for(int i = 0; i < 8; i++) {
        uint32_t addr = wordAddr + i;
        // 如果地址不存在，返回0（NOP）
        *insts[i] = (memory.find(addr) != memory.end()) ? memory[addr] : 0;
    }
}

void AXIMemory::dmemAccess(VCPU* cpu) {
    // 处理 LSU0
    uint8_t lsu0Op = cpu->io_dmem_lsu0_op;
    uint32_t lsu0Addr = cpu->io_dmem_lsu0_addr;
    uint32_t lsu0Wdata = cpu->io_dmem_lsu0_wdata;
    
    if(LSUOp::isLoad(lsu0Op)) {
        uint32_t wordAddr = lsu0Addr >> 2;
        uint32_t wordOffset = lsu0Addr & 0x3;
        uint32_t word = (memory.find(wordAddr) != memory.end()) ? memory[wordAddr] : 0;
        cpu->io_dmem_lsu0_rdata = word >> (wordOffset << 3);
    } else if(LSUOp::isStore(lsu0Op)) {
        uint32_t wordAddr = lsu0Addr >> 2;
        uint32_t wordOffset = lsu0Addr & 0x3;
        
        // 检查是否为设备地址
        if((wordAddr >> 26) == 0xa) {
            device->write(lsu0Addr, lsu0Wdata);
        } else {
            uint32_t word = (memory.find(wordAddr) != memory.end()) ? memory[wordAddr] : 0;
            uint32_t wdataShift = lsu0Wdata << (wordOffset << 3);
            
            // 根据操作类型确定写入掩码
            uint8_t wstrb = 0;
            switch(lsu0Op) {
                case LSUOp::SB:  wstrb = 0x1 << wordOffset; break;
                case LSUOp::SH:  wstrb = 0x3 << wordOffset; break;
                case LSUOp::SW:
                case LSUOp::FSW: wstrb = 0xF; break;
            }
            
            for(int i = 0; i < 4; i++) {
                if(wstrb & (1 << i)) {
                    word = (word & ~byteMasks[i]) | (wdataShift & byteMasks[i]);
                }
            }
            memory[wordAddr] = word;
            // std::cout << "dmemAccess: store addr: 0x" << std::hex << std::setw(8) << std::setfill('0') << lsu0Addr << std::dec << ", data: 0x" << std::hex << std::setw(8) << std::setfill('0') << lsu0Wdata << std::dec << std::endl;
        }
        cpu->io_dmem_lsu0_rdata = 0;
    } else {
        cpu->io_dmem_lsu0_rdata = 0;
    }
    
    // 处理 LSU1
    uint8_t lsu1Op = cpu->io_dmem_lsu1_op;
    uint32_t lsu1Addr = cpu->io_dmem_lsu1_addr;
    uint32_t lsu1Wdata = cpu->io_dmem_lsu1_wdata;
    
    if(LSUOp::isLoad(lsu1Op)) {
        uint32_t wordAddr = lsu1Addr >> 2;
        uint32_t wordOffset = lsu1Addr & 0x3;
        uint32_t word = (memory.find(wordAddr) != memory.end()) ? memory[wordAddr] : 0;
        cpu->io_dmem_lsu1_rdata = word >> (wordOffset << 3);
    } else if(LSUOp::isStore(lsu1Op)) {
        uint32_t wordAddr = lsu1Addr >> 2;
        uint32_t wordOffset = lsu1Addr & 0x3;
        
        // 检查是否为设备地址
        if((wordAddr >> 26) == 0xa) {
            device->write(lsu1Addr, lsu1Wdata);
        } else {
            uint32_t word = (memory.find(wordAddr) != memory.end()) ? memory[wordAddr] : 0;
            uint32_t wdataShift = lsu1Wdata << (wordOffset << 3);
            
            // 根据操作类型确定写入掩码
            uint8_t wstrb = 0;
            switch(lsu1Op) {
                case LSUOp::SB:  wstrb = 0x1 << wordOffset; break;
                case LSUOp::SH:  wstrb = 0x3 << wordOffset; break;
                case LSUOp::SW:
                case LSUOp::FSW: wstrb = 0xF; break;
            }
            
            for(int i = 0; i < 4; i++) {
                if(wstrb & (1 << i)) {
                    word = (word & ~byteMasks[i]) | (wdataShift & byteMasks[i]);
                }
            }
            memory[wordAddr] = word;
            // std::cout << "dmemAccess: store addr: 0x" << std::hex << std::setw(8) << std::setfill('0') << lsu1Addr << std::dec << ", data: 0x" << std::hex << std::setw(8) << std::setfill('0') << lsu1Wdata << std::dec << std::endl;
        }
        cpu->io_dmem_lsu1_rdata = 0;
    } else {
        cpu->io_dmem_lsu1_rdata = 0;
    }
}
#endif

uint32_t AXIMemory::refMemoryRead(uint32_t addr) {
    uint32_t wordAddr = addr >> 2;
    // 如果地址不存在，返回0（未初始化的内存）
    if(refMemory.find(wordAddr) == refMemory.end()) {
        return 0;
    }
    if(addr == 0x80000fcc){
        // std::cout << "refMemoryRead: addr: 0x" << std::hex << std::setw(8) << std::setfill('0') << addr << std::dec << ", data: 0x" << std::hex << std::setw(8) << std::setfill('0') << refMemory[wordAddr] << std::dec << std::endl;
    }
    // std::cout << "refMemoryRead: addr: 0x" << std::hex << std::setw(8) << std::setfill('0') << addr << std::dec << ", data: 0x" << std::hex << std::setw(8) << std::setfill('0') << refMemory[wordAddr] << std::dec << std::endl;
    return refMemory[wordAddr] >> ((addr & 0x3) << 3);
}

void AXIMemory::refMemoryWrite(uint32_t addr, uint32_t data, uint8_t wstrb) {
    uint32_t wordAddr = addr >> 2;
    uint32_t wordOffset = addr & 0x3;
    uint8_t wstrbShift = wstrb << wordOffset;
    uint32_t word = refMemory[wordAddr];
    uint32_t wdataShift = data << (wordOffset << 3);
    for(int i = 0; i < 4; i++) {
        if(wstrbShift & (1 << i)) {
            word = (word & ~byteMasks[i]) | (wdataShift & byteMasks[i]);
        }
    }
    refMemory[wordAddr] = word;
    if(addr == 0xa00003f8) {
        putchar(data);
        fflush(stdout);
    }
    // std::cout << "refMemoryWrite: addr: 0x" << std::hex << std::setw(8) << std::setfill('0') << addr << std::dec << ", data: 0x" << std::hex << std::setw(8) << std::setfill('0') << data << std::dec << ", wstrb: 0x" << std::hex << std::setw(2) << std::setfill('0') << (int)wstrb << std::dec << std::endl;
}

uint32_t AXIMemory::debugRead(uint32_t addr) {
    uint32_t wordAddr = addr >> 2;
    // 如果地址不存在，返回0（未初始化的内存）
    if(memory.find(wordAddr) == memory.end()) {
        return 0;
    }
    return memory[wordAddr] >> ((addr & 0x3) << 3);
}
