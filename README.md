# ZirconSim

ZirconSim 是 Zircon-2026 的 Verilator 仿真与 Spike 差分环境。它加载 RV32 ELF，解析
入口和 `tohost` 等符号，在处理器退休点逐条比较 PC、指令与整数/浮点写回结果。
系统配置提供 Linux 平台，性能配置提供 VLEN=256 的 Zve32f 裸机计算环境。

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
- `--instruction-profile <文件.csv>` 按 PC 导出本次运行的实际退休指令数量，字段为
  `pc,instruction,retired`。统计从本次启动或检查点恢复开始，适合结合 ELF 符号核对循环中的
  算术指令数；默认关闭，不修改硬件计数器。

性能计数器在程序结束时汇总为 Markdown 报告。`zircon-sim-unit` 验证随机数生成与 RV32
ELF 装载，包括 `PT_LOAD`、入口地址和 `tohost/fromhost` 符号。功能测试目标使用
`RV-Software` 中的裸机程序；`--allow-timeout` 显式允许以周期上限结束的测试。

## SIMD8 性能核心

在主仓库根目录使用独立输出目录：

```sh
cmake -S . -B build/performance -DCMAKE_CXX_COMPILER=clang++ \
  -DZIRCON_CORE_PROFILE=performance
cmake --build build/performance --target zircon-sim -j4
cmake --build build/performance --target coremark
```

构建配置同时选择 RTL、256 bit AXI 模型和 Spike ISA。性能核心的向量退休差分比较目的
寄存器组、`vl/vtype/vstart`、浮点异常与整数饱和标志以及 Store 地址、字节 mask 和数据。
标准允许的 agnostic 元素采用 DUT 的全一值同步参考状态，undisturbed 元素仍逐元素校验。
默认保留差分与动态进度；`time` 使用核心周期时基。
`coremark` 构建目标按核心配置选择对应软件。Verilator 线程池使用已编译模型的线程数；
同时运行多个仿真时，为各进程分配互不重叠的 CPU，避免运行线程争用。
模型热路径与 Verilator 公共运行库使用 `-O3`，初始化代码单独编译。
`ZIRCON_SIM_ENABLE_THIN_LTO` 和 `ZIRCON_SIM_PGO_MODE` 分别控制 ThinLTO 与 PGO；
LLVM profile 工具和链接器应与宿主 Clang 使用相同主版本。

SIMD 五项计算程序、SGEMM 与吞吐测试位于 `RV-Software/simd/`；官方单精度负载位于
`RV-Software/coremark-pro/`。完整系统与性能核心的 RTL、仿真器和软件使用独立生成目录。
性能报告按核心配置、VLEN、AXI 宽度和宿主编译器版本保存在 `reports/` 的对应子目录。
