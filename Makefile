WORK_DIR = $(abspath .)
VERILOG_DIR = $(WORK_DIR)/../verilog
CC_DIR = $(WORK_DIR)/src
BUILD_DIR = $(WORK_DIR)/build
TAR_DIR = $(BUILD_DIR)/obj

# C++编译器（如果没有定义则使用默认值）
CXX ?= g++

# 纯Simulator模式开关：设置为1启用纯Simulator模式（跳过Verilog编译）
# 使用方法：make USE_SIMULATOR_ONLY_MODE=1
USE_SIMULATOR_ONLY_MODE ?= 0

VERILOG_TOP 		= $(VERILOG_DIR)/CPU.sv
VFLAGS 				= --trace --cc --exe -O3 -I$(VERILOG_DIR) -Mdir $(BUILD_DIR) --no-MMD
VFLAGS 				+= -Wno-UNOPTFLAT -Wno-WIDTHEXPAND --verilate-jobs 8 
CINC_PATH 			= -CFLAGS -I$(WORK_DIR)/include

# 如果启用纯Simulator模式，添加编译宏
ifeq ($(USE_SIMULATOR_ONLY_MODE),1)
CINC_PATH 			+= -CFLAGS -DUSE_SIMULATOR_ONLY
endif

REWRITE = $(WORK_DIR)/script/rewrite.mk

# 根据模式选择要编译的C++源文件
ifeq ($(USE_SIMULATOR_ONLY_MODE),1)
# 纯Simulator模式：排除Emulator.cc
CSRCS = $(shell find $(CC_DIR) -name "*.cc" ! -name "Emulator.cc")
else
# 对比模式：编译所有源文件
CSRCS = $(shell find $(CC_DIR) -name "*.cc")
endif
VSRCS = $(shell find $(VERILOG_DIR) -name "*.sv")
BINARY = $(BUILD_DIR)/VCPU

IMG = 

COLOR_RED   		= \033[31m
COLOR_GREEN 		= \033[32m
COLOR_YELLOW 		= \033[33m
COLOR_BLUE  		= \033[34m
COLOR_PURPLE 		= \033[35m
COLOR_DBLUE 		= \033[36m
COLOR_NONE  		= \033[0m


SCALA_DIR = $(WORK_DIR)/../src/main/scala
SCALA_SRCS := $(shell find $(SCALA_DIR) -name "*.scala")


all: $(BINARY) 

# 纯Simulator模式：直接编译C++代码，不需要Verilog
ifeq ($(USE_SIMULATOR_ONLY_MODE),1)
$(BINARY): $(CSRCS)
	@printf "$(COLOR_YELLOW)[SIMULATOR-ONLY MODE]$(COLOR_NONE) Building without Verilog\n"
	@mkdir -p $(BUILD_DIR)
	@$(CXX) -std=c++17 -O3 -I$(WORK_DIR)/include -DUSE_SIMULATOR_ONLY \
		-Wall -Wno-unused-result -pipe \
		$(CSRCS) -o $(BINARY) -lpthread
	@printf "$(COLOR_DBLUE)[BUILD]$(COLOR_NONE) $(notdir $(BINARY))\n"
else
# 对比模式：需要编译Verilog和CPU代码
# 依赖于生成的 Verilog 文件和 C++ 源文件
$(VERILOG_TOP): $(SCALA_SRCS)
	@printf "$(COLOR_YELLOW)[SCALA]$(COLOR_NONE) Zircon\n"
	@$(MAKE) -s -j32 -C ../ sim-verilog

$(BINARY): $(CSRCS) $(VERILOG_TOP) $(VSRCS)
	@printf "$(COLOR_DBLUE)[VERILATE]$(COLOR_NONE) $(notdir $(BUILD_DIR))/VCPU\n"
	@mkdir -p $(BUILD_DIR)
	@verilator $(VFLAGS) $(CSRCS) $(CINC_PATH) $(VERILOG_TOP)
	@printf "$(COLOR_DBLUE)[MAKE]$(COLOR_NONE) $(notdir $(BUILD_DIR))/VCPU\n"
	@$(MAKE) -s -j32 -C $(BUILD_DIR) -f $(REWRITE)
endif 


run: $(BINARY) 
	@printf "$(COLOR_YELLOW)[RUN]$(COLOR_NONE) build/$(notdir $<)\n"
	@$(BINARY) $(IMG) $(ARGS)


clean:
	rm -rf $(BUILD_DIR)