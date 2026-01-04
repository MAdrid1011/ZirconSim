#ifndef CONFIG_HH
#define CONFIG_HH

// USE_SIMULATOR_ONLY 宏由 Makefile 通过编译选项 -DUSE_SIMULATOR_ONLY 传递
// 如果定义了此宏，ZirconSim将只使用Simulator执行指令，不进行CPU仿真对比
// 如果未定义此宏，ZirconSim将使用Emulator进行CPU仿真并与Simulator对比
// 在 Makefile 中设置 USE_SIMULATOR_ONLY_MODE=1 来启用纯Simulator模式

#endif

