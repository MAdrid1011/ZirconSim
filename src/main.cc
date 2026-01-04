#include "AXIMemory.h"
#include "Device.h"
#include "Config.h"
#include "Statistic.h"
#include "Simulator.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include "utils.h"

#ifndef USE_SIMULATOR_ONLY
#include "verilated_vcd_c.h"
#include "VCPU.h"
#include "Emulator.h"
#endif


int main(int argc, char** argv) {
    // 如果没有提供程序文件，使用空字符串（将使用默认的内存初始值）
    std::string imgPath = (argc >= 2) ? argv[1] : "";

    Device *device = new Device();
    AXIMemory *memory = new AXIMemory(imgPath, 0x80000000, device);
    Statistic *stat = new Statistic();
    Simulator *simulator = new Simulator(memory);

#ifdef USE_SIMULATOR_ONLY
    // 纯Simulator模式：只使用Simulator执行指令（VLIW模式）
    std::cout << "========================================" << std::endl;
    std::cout << ANSI_FG_CYAN << "VLIW SIMULATOR-ONLY MODE STARTED." << ANSI_NONE << std::endl;

    // 用于控制进度打印线程的标志
    std::atomic<bool> running(true);

    // 启动进度打印线程
    std::thread printThread([stat, simulator, &running](){
        while(running.load()){
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if(running.load()) {
                std::cout << "\r";
                std::cout << "PC: 0x" << std::hex << simulator->getPC() << std::dec 
                          << ", VLIW Packs executed: " << stat->getCycles()
                          << ", Instructions executed: " << stat->getInsts();
                std::cout << std::flush;
            }
        }
    });

    int ret = 0;
    uint32_t stallCount = 0;
    const uint32_t stallThreshold = 5;  // 防止无限循环（以VLIW包为单位）
    uint32_t prevPC = 0;

    // 循环执行VLIW包直到检测到结束
    while(true) {
        // 记录当前PC到统计信息中
        stat->pcBufferPush(simulator->getPC());
        if(prevPC == simulator->getPC()) {
            stallCount++;
        } else {
            stallCount = 0;
        }
        prevPC = simulator->getPC();
        if(simulator->isSimEnd()) {
            // 检测到结束指令，检查a0寄存器
            ret = (simulator->getRf(10) == 0 ? 0 : -1);
            break;
        }
        if(stallCount > stallThreshold) {
            ret = -3;  // 超时
            break;
        }
        
        // 执行一个完整的VLIW包（8条指令）
        simulator->stepVLIWPack();
        stat->addInsts(VLIW_PACK_SIZE);  // 每个VLIW包包含8条指令
        stat->addCycles(1);               // 每个VLIW包算1个周期
    }

    // 停止进度打印线程
    running = false;
    printThread.join();

    std::cout << "\n========================================" << std::endl;
    if(ret == -3) {
        std::cout << ANSI_FG_YELLOW << "STALL FOR TOO LONG WITHOUT REACHING END INSTRUCTION." << ANSI_NONE << std::endl;
    } else if(ret == -1) {
        std::cout << ANSI_FG_RED << "SIMULATION ENDED WITH a0 != 0." << ANSI_NONE << std::endl;
    } else if(ret == 0) {
        std::cout << ANSI_FG_GREEN << "SIMULATION ENDED SUCCESSFULLY." << ANSI_NONE << std::endl;
    }
    stat->printLastInstrucions(memory);
    stat->printPerformance();

    std::string imgName = (imgPath.empty()) ? "default" : imgPath.substr(imgPath.find_last_of('/') + 1, imgPath.find_last_of('.') - imgPath.find_last_of('/') - 1);
    std::cout << "========================================" << std::endl;
    return ret;

#else
    // 对比模式：使用Emulator进行CPU仿真并与Simulator对比
    VCPU*cpu = new VCPU();
    VerilatedVcdC *vcd = new VerilatedVcdC();

    Verilated::traceEverOn(true);
    VerilatedVcdC *m_trace = new VerilatedVcdC;
    cpu->trace(m_trace, 5);
    m_trace->open("waveform.vcd");

    Emulator *emulator = new Emulator(cpu, memory, stat, simulator, m_trace);
    std::cout << "========================================" << std::endl;
    std::cout << ANSI_FG_CYAN << "DIFFTEST MODE STARTED." << ANSI_NONE << std::endl;

    emulator->reset();
    int ret = emulator->step(-1);

    std::cout << "========================================" <<  std::endl;
    if(ret == -3) {
        std::cout << ANSI_FG_YELLOW<< "STALL FOR TOO LONG WITHOUT COMMITTING INSTRUCTIONS." << ANSI_NONE << std::endl;
    }else if(ret == -2) {
        std::cout << ANSI_FG_YELLOW << "DIFFTEST FAILED." << ANSI_NONE << std::endl;
    }else if(ret == -1) {
        std::cout << ANSI_FG_RED << "SIMULATION ENDED WITH a0 != 0." << ANSI_NONE << std::endl;
    }else if(ret == 0) {
        std::cout << ANSI_FG_GREEN << "SIMULATION ENDED SUCCESSFULLY." << ANSI_NONE << std::endl;
        
    }
    stat->printLastInstrucions(memory);
    stat->printPerformance();

    std::string imgName = imgPath.substr(imgPath.find_last_of('/') + 1, imgPath.find_last_of('.') - imgPath.find_last_of('/') - 1);
    stat->printMarkdownReport(cpu, imgName, simulator);
    std::cout <<  "========================================" << std::endl;
    m_trace->close();
    return ret;
#endif
}