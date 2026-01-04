# ZirconSim

本项目是在ChiselTest停止更新后，为Zircon-2024处理器设计的C++仿真环境。其性能相比ChiselTest提升了约50%，并无需复杂的Scala库支持。

## 使用方法

请将本项目放到Zircon-2024的根目录下。执行如下命令可以构建项目：

```bash
make 
```

## 运行模式

ZirconSim支持两种运行模式：

### 1. 对比模式（默认）

默认情况下，ZirconSim使用Emulator进行CPU仿真，并与Simulator进行对比测试（difftest）。这是用于验证CPU实现的模式。

```bash
make run IMG=<程序路径>
```

### 2. 纯Simulator模式

如果启用了纯Simulator模式，ZirconSim将只使用Simulator执行Memory中的指令，不进行CPU仿真。这个模式更快，适合快速测试程序功能，并且会跳过Verilog和Scala编译步骤。

启用方法：在编译时添加 `USE_SIMULATOR_ONLY_MODE=1` 参数：
```bash
make USE_SIMULATOR_ONLY_MODE=1
make run IMG=<程序路径>
```

或者一次性编译并运行：
```bash
make USE_SIMULATOR_ONLY_MODE=1 run IMG=<程序路径>
```

**注意**：纯Simulator模式下不会生成波形文件，也不会进行CPU性能统计，编译速度会显著加快。
