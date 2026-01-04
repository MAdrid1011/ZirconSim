#include "Config.h"

#ifndef USE_SIMULATOR_ONLY

#include "Emulator.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>
#include "utils.h"

#define DUMP_WAVE 0

// 提交信息结构体
struct CommitInfo {
    uint32_t pc;
    uint32_t inst;
    uint8_t rd;
    bool isFPR;
    uint32_t data;
    int pipeline;
    
    bool operator<(const CommitInfo& other) const {
        return pc < other.pc;  // 按PC排序
    }
};

void Emulator::reset() {
    cpu->reset = 1;
    cpu->clock = 0;
    cpu->eval();
    cpu->clock = 1;
    cpu->eval();
    cpu->reset = 0;
}

bool Emulator::difftestPC(uint32_t pc) {
    return simulator->getPC() == pc;
}
bool Emulator::difftestRF(uint8_t rd, uint32_t rdData, uint32_t pc) {
    return simulator->getRf(rd) == rdData;
}
bool Emulator::difftestStep(uint8_t rd, uint32_t rdData, uint32_t pc, uint32_t step) {
    if(!difftestPC(pc)){
        std::cout << ANSI_FG_RED << "PC mismatch at pc 0x" << std::hex << simulator->getPC() << ", dut: 0x" << pc << ANSI_NONE << std::dec << std::endl;
        return false;
    }
    for(uint32_t i = 0; i < step; i++){
        simulator->step(1);
    }
    if(!difftestRF(rd, rdData, pc)){
        std::cout << ANSI_FG_RED << "RF mismatch at pc 0x" << std::hex << pc << std::dec;
        std::cout << ", reg " << (uint32_t)rd << ", dut: 0x" << std::hex << rdData;
        std::cout << ", ref: 0x" << simulator->getRf(rd) << std::dec << ANSI_NONE << std::endl;
        return false;
    }
    return true;
}

int Emulator::step(uint32_t num) {

    std::thread printThread([this](){
        while(true){
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::cout << "\r";
            std::cout << "Total cycles: " << stat->getCycles() << ", IPC: " << stat->getIPC();
            std::cout << std::flush;
        }
    });
    printThread.detach();
    
    // 8条流水线的WB阶段提交信息的指针数组
    uint8_t *wbValid[8] = {
        &cpu->io_debug_wbValid_0, &cpu->io_debug_wbValid_1,
        &cpu->io_debug_wbValid_2, &cpu->io_debug_wbValid_3,
        &cpu->io_debug_wbValid_4, &cpu->io_debug_wbValid_5,
        &cpu->io_debug_wbValid_6, &cpu->io_debug_wbValid_7
    };
    uint32_t *wbPC[8] = {
        &cpu->io_debug_wbPC_0, &cpu->io_debug_wbPC_1,
        &cpu->io_debug_wbPC_2, &cpu->io_debug_wbPC_3,
        &cpu->io_debug_wbPC_4, &cpu->io_debug_wbPC_5,
        &cpu->io_debug_wbPC_6, &cpu->io_debug_wbPC_7
    };
    uint32_t *wbInst[8] = {
        &cpu->io_debug_wbInst_0, &cpu->io_debug_wbInst_1,
        &cpu->io_debug_wbInst_2, &cpu->io_debug_wbInst_3,
        &cpu->io_debug_wbInst_4, &cpu->io_debug_wbInst_5,
        &cpu->io_debug_wbInst_6, &cpu->io_debug_wbInst_7
    };
    uint8_t *wbRd[8] = {
        &cpu->io_debug_wbRd_0, &cpu->io_debug_wbRd_1,
        &cpu->io_debug_wbRd_2, &cpu->io_debug_wbRd_3,
        &cpu->io_debug_wbRd_4, &cpu->io_debug_wbRd_5,
        &cpu->io_debug_wbRd_6, &cpu->io_debug_wbRd_7
    };
    uint32_t *wbData[8] = {
        &cpu->io_debug_wbData_0, &cpu->io_debug_wbData_1,
        &cpu->io_debug_wbData_2, &cpu->io_debug_wbData_3,
        &cpu->io_debug_wbData_4, &cpu->io_debug_wbData_5,
        &cpu->io_debug_wbData_6, &cpu->io_debug_wbData_7
    };
    
    // GPR调试接口
    uint32_t *dbgGpr = &cpu->io_debug_gpr_0;
    
    while(num-- > 0){
        stat->addCycles(1);
        
        // 收集本周期所有有效提交
        std::vector<CommitInfo> commits;
        for(int i = 0; i < 8; i++){
            if(*wbValid[i]){
                CommitInfo info;
                info.pc = *wbPC[i];
                info.inst = *wbInst[i];
                info.rd = *wbRd[i] & 0x1F;
                info.isFPR = (*wbRd[i] >> 5) & 0x1;
                info.data = *wbData[i];
                info.pipeline = i;
                commits.push_back(info);
            }
        }
        
        // 如果有提交，按PC排序后进行difftest
        if(!commits.empty()){
            stallCount = 0;
            
            // 按PC排序
            std::sort(commits.begin(), commits.end());
            
            // 获取最小PC作为VLIW包的起始地址
            uint32_t vliwPackPC = commits[0].pc;
            
            // 检查DUT提交的VLIW包起始PC是否与Simulator匹配
            if(simulator->getPC() != vliwPackPC){
                std::cout << ANSI_FG_RED << "VLIW Pack PC mismatch: ref=0x" << std::hex << simulator->getPC() 
                          << ", dut=0x" << vliwPackPC << ANSI_NONE << std::dec << std::endl;
                return -2;
            }
            
            // 检查是否是程序结束（检查第一条指令）
            if(simEnd(commits[0].inst)){
                return (dbgGpr[10] == 0 ? 0 : -1);
            }
            
            // 让Simulator执行一整个VLIW包（会缓存所有写回结果后统一提交）
            simulator->stepVLIWPack();
            
            // 比较每条提交的指令结果
            for(const auto& commit : commits){
                stat->addInsts(1);
                stat->pcBufferPush(commit.pc);
                
                // 只对GPR写入进行寄存器值比较（FPR暂不支持）
                if(!commit.isFPR && commit.rd != 0){
                    if(!difftestRF(commit.rd, commit.data, commit.pc)){
                        std::cout << ANSI_FG_RED << "RF mismatch at pc 0x" << std::hex << commit.pc << std::dec;
                        std::cout << ", reg " << (uint32_t)commit.rd << ", dut: 0x" << std::hex << commit.data;
                        std::cout << ", ref: 0x" << simulator->getRf(commit.rd) << std::dec << ANSI_NONE << std::endl;
                        std::cout << "Difftest failed at pipeline " << commit.pipeline << std::endl;
                        std::cout << "Inst: 0x" << std::hex << commit.inst << std::dec << std::endl;
                        return -2;
                    }
                }
            }
        } else {
            stallCount++;
            if(stallCount > stallThreshold) {
                std::cout << "Stalled for too long at PC: 0x" << std::hex << cpu->io_imem_pc << std::dec << std::endl;
                return -3;
            }
        }
        
        // 提供内存数据（组合逻辑）
        memory->imemRead(cpu);
        memory->dmemAccess(cpu);
        cpu->eval();  // 评估组合逻辑
        
        // 时钟上升沿，更新寄存器
        cpu->clock = 1;
        cpu->eval();
        
        // 时钟下降沿
        cpu->clock = 0;
        cpu->eval();
#ifdef DUMP_WAVE
        m_trace->dump(simTime);
        simTime++;
#endif
    }
    return 1;
}

#endif // USE_SIMULATOR_ONLY

