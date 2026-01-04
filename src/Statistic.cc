#include "Statistic.h"
#include "Config.h"
#include "utils.h"
#include <iostream>
#include <filesystem>

void Statistic::printPerformance() {
    std::cout << ANSI_FG_CYAN << "Total cycles: " << cycles << ", Total insts: " << insts << ", IPC: " << getIPC() << ANSI_NONE << std::endl;
}
void Statistic::printLastInstrucions(AXIMemory* mem){
    std::cout << ANSI_FG_CYAN << "Last 8 instructions:" << ANSI_NONE << std::endl;
    std::cout << ANSI_FG_MAGENTA;
    for(int i = 0; i < 8; i++){
        uint32_t pc = pcRingBuffer[(pcRingBufferIndex + i) % 8];
        uint32_t inst = mem->debugRead(pc);
        // 格式：0x十六进制地址：十六进制指令码
        std::cout << "0x" << std::hex << std::setw(8) << std::setfill('0') << pc;
        std::cout << ": 0x" << std::hex << std::setw(8) << std::setfill('0') << inst << std::dec << std::endl;
    }
    std::cout << ANSI_NONE;
}
#ifndef USE_SIMULATOR_ONLY
void Statistic::printMarkdownReport(VCPU* cpu, std::string imgName, Simulator* sim){
    // 创建reports文件夹
    std::string reportsDir = "reports";
    if(!std::filesystem::exists(reportsDir)){
        std::filesystem::create_directory(reportsDir);
    }
    std::ofstream fout = std::ofstream("reports/report-" + imgName + ".md");

    fout << "## 程序基本情况" << std::endl;
    fout << "| 程序名 | 总周期数 | 总指令数 | IPC |" << std::endl;
    fout << "| --- | --- | --- | --- |" << std::endl;
    fout << "| " << imgName << " | " << cycles << " | " << insts << " | " << getIPC() << " |" << std::endl;

    fout << "### 指令统计" << std::endl;
    fout << "| 指令类型 | 总数 | 占比 |" << std::endl;
    fout << "| --- | --- | --- |" << std::endl;
    fout << "| ALU | " << sim->getInstStat().aluInsts << " | " << sim->getInstStat().aluInsts * 100.0 / insts << "% |" << std::endl;
    fout << "| Branch | " << sim->getInstStat().branchInsts << " | " << sim->getInstStat().branchInsts * 100.0 / insts << "% |" << std::endl;
    fout << "| Load | " << sim->getInstStat().loadInsts << " | " << sim->getInstStat().loadInsts * 100.0 / insts << "% |" << std::endl;
    fout << "| Store | " << sim->getInstStat().storeInsts << " | " << sim->getInstStat().storeInsts * 100.0 / insts << "% |" << std::endl;
    fout << "| Mul | " << sim->getInstStat().mulInsts << " | " << sim->getInstStat().mulInsts * 100.0 / insts << "% |" << std::endl;
    fout << "| Div | " << sim->getInstStat().divInsts << " | " << sim->getInstStat().divInsts * 100.0 / insts << "% |" << std::endl;

    // 当前CPU没有调试接口，暂时不输出分支预测和缓存统计
    fout << "## 备注" << std::endl;
    fout << "当前CPU版本没有调试接口，无法统计分支预测和缓存命中率。" << std::endl;

    fout.close();

}
#endif
