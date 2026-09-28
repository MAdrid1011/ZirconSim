# ZirconSim

ZirconSim 是 Zircon-2026 的 Verilator 仿真与 Spike 差分环境。它加载 RV32 ELF，解析
入口和 `tohost` 等符号，在处理器退休点逐条比较 PC、指令与整数/浮点写回结果。

从 `ZirconSim/` 目录构建并运行：

```sh
cmake -S .. -B ../build/cmake
cmake --build ../build/cmake --target zircon-sim --parallel
../build/cmake/bin/zircon-sim --elf path/to/test.elf
```

构建需要 Spike 开发库及其 `riscv-riscv.pc` 元数据。顶层 CMake 会生成 `ZirconCore`
SystemVerilog，调用 Verilator 并链接进程内 Spike。终端运行时显示周期、退休指令数、IPC
和仿真速度；程序结束后输出结果、差分状态和性能报告位置。重定向输出为单条 JSON 记录，
`--json` 可在终端强制使用 JSON。

## Linux 仿真

Zircon-2026 主仓库提供软件镜像、PGO 构建、检查点和交互 UART 的统一入口：

```sh
make -C RV-Software/linux-system linux
```

默认配置使用 Clang/AppleClang、`O3`、ThinLTO、五个 Verilator 运行线程和进程内 Spike
差分。PGO 数据与 RTL、仿真器、工具链和 Linux 镜像绑定；输入变化时自动生成对应的优化数据。
终端显示 `~ #` 后可直接输入命令。启动环境与验证范围见主仓库的
[Linux 启动说明](https://github.com/MAdrid1011/Zircon-2026/blob/main/docs/Linux-Bringup.md)。

## 仿真接口

- `--seed` 指定可重复的初始化种子；`--max-cycles` 指定运行周期上限。
- `--no-color` 和 `--no-progress` 控制终端输出；`--uart-stdio` 将 UART 连接到当前终端。
- `--checkpoint-save`、`--checkpoint-load` 与 `--checkpoint-interval` 保存和恢复长时间仿真。

性能计数器在程序结束时汇总为 Markdown 报告。`zircon-sim-unit` 验证随机数生成与 RV32
ELF 装载，包括 `PT_LOAD`、入口地址和 `tohost/fromhost` 符号。功能测试目标使用
`RV-Software` 中的裸机程序；`--allow-timeout` 显式允许以周期上限结束的测试。
