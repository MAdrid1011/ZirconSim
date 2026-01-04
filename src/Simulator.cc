#include "Simulator.h"
#include <cmath>

// ==================== VLIW 写回缓冲区辅助函数 ====================

void Simulator::clearWriteBackBuffer() {
    for (int i = 0; i < VLIW_PACK_SIZE; i++) {
        writeBackBuffer[i].valid = false;
        writeBackBuffer[i].isFloat = false;
        writeBackBuffer[i].rd = 0;
        writeBackBuffer[i].intValue = 0;
        writeBackBuffer[i].floatValue = 0.0f;
    }
    vliwInstIndex = 0;
}

void Simulator::stageIntWrite(uint8_t rd, uint32_t value) {
    if (vliwInstIndex < VLIW_PACK_SIZE) {
        writeBackBuffer[vliwInstIndex].valid = true;
        writeBackBuffer[vliwInstIndex].isFloat = false;
        writeBackBuffer[vliwInstIndex].rd = rd;
        writeBackBuffer[vliwInstIndex].intValue = value;
    }
}

void Simulator::stageFloatWrite(uint8_t rd, float value) {
    if (vliwInstIndex < VLIW_PACK_SIZE) {
        writeBackBuffer[vliwInstIndex].valid = true;
        writeBackBuffer[vliwInstIndex].isFloat = true;
        writeBackBuffer[vliwInstIndex].rd = rd;
        writeBackBuffer[vliwInstIndex].floatValue = value;
    }
}

void Simulator::commitWriteBack() {
    // 按顺序写回所有暂存的结果
    for (int i = 0; i < VLIW_PACK_SIZE; i++) {
        if (writeBackBuffer[i].valid) {
            if (writeBackBuffer[i].isFloat) {
                fpr[writeBackBuffer[i].rd] = writeBackBuffer[i].floatValue;
            } else {
                rf[writeBackBuffer[i].rd] = writeBackBuffer[i].intValue;
            }
        }
    }
    // 确保 x0 和 f0 始终为0
    rf[0] = 0;
    // 清空缓冲区
    clearWriteBackBuffer();
}

// ==================== VLIW 包执行 ====================

void Simulator::stepVLIWPack() {
    clearWriteBackBuffer();
    
    // 执行8条指令，暂存写回结果
    for (vliwInstIndex = 0; vliwInstIndex < VLIW_PACK_SIZE; vliwInstIndex++) {
        uint32_t inst = memory->refMemoryRead(pc);
        uint8_t opcode = bits(inst, 6, 0);
        
        // 检查是否是分支/跳转指令
        bool isBranchOrJump = (opcode == 0x63 ||  // B-type: beq, bne, blt, bge, bltu, bgeu
                               opcode == 0x67 ||  // JALR
                               opcode == 0x6F);   // JAL
        
        switch(opcode){
            case 0x37: executeUType(inst); break;
            case 0x17: executeUType(inst); break;
            case 0x6F: executeJType(inst); break;
            case 0x67: executeIType(inst); break;
            case 0x63: executeBType(inst); break;
            case 0x03: executeIType(inst); break;
            case 0x07: executeIType(inst); break;  // FLW
            case 0x23: executeSType(inst); break;
            case 0x27: executeSType(inst); break;  // FSW
            case 0x13: executeIType(inst); break;
            case 0x33: executeRType(inst); break;
            case 0x53: executeFType(inst); break;  // FP operations
            case 0x43: executeR4Type(inst); break; // FMADD.S
            case 0x47: executeR4Type(inst); break; // FMSUB.S
            case 0x4B: executeR4Type(inst); break; // FNMSUB.S
            case 0x4F: executeR4Type(inst); break; // FNMADD.S
            default: break;
        }
        
        // 如果遇到分支/跳转指令，执行完后立即退出循环
        if (isBranchOrJump) {
            vliwInstIndex++;  // 增加索引以便正确提交已执行的指令
            break;
        }
    }
    
    // 提交已执行指令的写回结果
    commitWriteBack();
}

// ==================== 单条指令执行（向后兼容） ====================

void Simulator::step(uint32_t num) {
    // 重置索引以便暂存
    vliwInstIndex = 0;
    
    uint32_t inst = memory->refMemoryRead(pc);
    uint8_t opcode = bits(inst, 6, 0);
    
    switch(opcode){
        case 0x37: executeUType(inst); break;
        case 0x17: executeUType(inst); break;
        case 0x6F: executeJType(inst); break;
        case 0x67: executeIType(inst); break;
        case 0x63: executeBType(inst); break;
        case 0x03: executeIType(inst); break;
        case 0x07: executeIType(inst); break;  // FLW
        case 0x23: executeSType(inst); break;
        case 0x27: executeSType(inst); break;  // FSW
        case 0x13: executeIType(inst); break;
        case 0x33: executeRType(inst); break;
        case 0x53: executeFType(inst); break;  // FP operations
        case 0x43: executeR4Type(inst); break; // FMADD.S
        case 0x47: executeR4Type(inst); break; // FMSUB.S
        case 0x4B: executeR4Type(inst); break; // FNMSUB.S
        case 0x4F: executeR4Type(inst); break; // FNMADD.S
        default: break;
    }
    
    // 单条指令模式下立即写回
    commitWriteBack();
}

void Simulator::executeRType(uint32_t inst) {
    uint8_t opcode  = bits(inst, 6, 0);
    uint8_t rd      = bits(inst, 11, 7);
    uint8_t rs1     = bits(inst, 19, 15);
    uint8_t rs2     = bits(inst, 24, 20);
    uint8_t funct7  = bits(inst, 31, 25);
    uint8_t funct3  = bits(inst, 14, 12);
    uint32_t value1 = rf[rs1];
    uint32_t value2 = rf[rs2];
    uint32_t result = 0;
    bool hasResult = false;
    
    switch (opcode) {
        case 0x33: {
            switch (funct7) {
                case 0x0: {
                    instStat.aluInsts++;
                    hasResult = true;
                    switch (funct3) {
                        case 0x0: result = value1 + value2; break;
                        case 0x1: result = value1 << value2; break;
                        case 0x2: result = (int32_t)value1 < (int32_t)value2; break;
                        case 0x3: result = value1 < value2; break;
                        case 0x4: result = value1 ^ value2; break;
                        case 0x5: result = value1 >> value2; break;
                        case 0x6: result = value1 | value2; break;
                        case 0x7: result = value1 & value2; break;
                        default: hasResult = false; break;
                    }
                    break;
                }
                case 0x20: {
                    instStat.aluInsts++;
                    hasResult = true;
                    switch (funct3) {
                        case 0x0: result = value1 - value2; break;
                        case 0x5: result = (int32_t)value1 >> value2; break;
                        default: hasResult = false; break;
                    }
                    break;
                }
                case 0x01: {
                    hasResult = true;
                    switch (funct3) {
                        case 0x0: instStat.mulInsts++; result = value1 * value2; break;
                        case 0x1: instStat.mulInsts++; result = ((int64_t)(int32_t)value1 * (int64_t)(int32_t)value2) >> 32; break;
                        case 0x2: instStat.mulInsts++; result = ((int64_t)(int32_t)value1 * (uint64_t)value2) >> 32; break;
                        case 0x3: instStat.mulInsts++; result = ((uint64_t)value1 * (uint64_t)value2) >> 32; break;
                        case 0x4: instStat.divInsts++; result = value2 == 0 ? -1 : (int32_t)value1 / (int32_t)value2; break;
                        case 0x5: instStat.divInsts++; result = value2 == 0 ? -1 : value1 / value2; break;
                        case 0x6: instStat.divInsts++; result = value2 == 0 ? value1 : (int32_t)value1 % (int32_t)value2; break;
                        case 0x7: instStat.divInsts++; result = value2 == 0 ? value1 : value1 % value2; break;
                        default: hasResult = false; break;
                    }
                    break;
                }
                default: break;
            }
            break;
        }
        default: break;
    }
    
    if (hasResult) {
        stageIntWrite(rd, result);
    }
    pc += 4;
}

void Simulator::executeIType(uint32_t inst) {
    uint8_t opcode = bits(inst, 6, 0);
    uint8_t rd = bits(inst, 11, 7);
    uint8_t rs1 = bits(inst, 19, 15);
    uint8_t funct3 = bits(inst, 14, 12);
    uint32_t imm = signExtend(bits(inst, 31, 20), 12);
    uint32_t value1 = rf[rs1];
    uint32_t result = 0;
    
    switch(opcode){
        case 0x13: {
            instStat.aluInsts++;
            switch(funct3){
                case 0x0: result = value1 + imm; break;
                case 0x1: result = value1 << (imm & 0x1F); break;
                case 0x2: result = (int32_t)value1 < (int32_t)imm; break;
                case 0x3: result = value1 < imm; break;
                case 0x4: result = value1 ^ imm; break;
                case 0x5: result = inst & 0x40000000 ? (int32_t)value1 >> (imm & 0x1F) : value1 >> (imm & 0x1F); break;
                case 0x6: result = value1 | imm; break;
                case 0x7: result = value1 & imm; break;
                default: break;
            }
            stageIntWrite(rd, result);
            pc += 4;
            break;
        }
        case 0x03: {
            instStat.loadInsts++;
            switch(funct3){
                case 0x0: result = signExtend(memory->refMemoryRead(value1 + imm), 8); break;
                case 0x1: result = signExtend(memory->refMemoryRead(value1 + imm), 16); break;
                case 0x2: result = memory->refMemoryRead(value1 + imm); break;
                case 0x4: result = zeroExtend(memory->refMemoryRead(value1 + imm), 8); break;
                case 0x5: result = zeroExtend(memory->refMemoryRead(value1 + imm), 16); break;
                default: break;
            }
            stageIntWrite(rd, result);
            pc += 4;
            break;
        }
        case 0x07: {  // FLW
            instStat.fLoadInsts++;
            if(funct3 == 0x2) {
                uint32_t data = memory->refMemoryRead(value1 + imm);
                float floatResult;
                memcpy(&floatResult, &data, sizeof(float));
                stageFloatWrite(rd, floatResult);
            }
            pc += 4;
            break;
        }
        case 0x67: {  // JALR
            instStat.branchInsts++;
            stageIntWrite(rd, pc + 4);
            pc = value1 + imm;
            break;
        }
        default: break;
    }
}

void Simulator::executeBType(uint32_t inst) {
    uint8_t opcode = bits(inst, 6, 0);
    uint8_t rs1 = bits(inst, 19, 15);
    uint8_t rs2 = bits(inst, 24, 20);
    uint8_t funct3 = bits(inst, 14, 12);
    uint32_t imm = signExtend(bits(inst, 31, 31) << 12 | (bits(inst, 7, 7) << 11) | (bits(inst, 30, 25) << 5) | (bits(inst, 11, 8) << 1), 13);
    uint32_t value1 = rf[rs1];
    uint32_t value2 = rf[rs2];
    switch(opcode){
        case 0x63: {
            instStat.branchInsts++;
            switch(funct3){
                case 0x0: value1 == value2 ? pc += imm : pc += 4; break;
                case 0x1: value1 != value2 ? pc += imm : pc += 4; break;
                case 0x4: (int32_t)value1 < (int32_t)value2 ? pc += imm : pc += 4; break;
                case 0x5: (int32_t)value1 >= (int32_t)value2 ? pc += imm : pc += 4; break;
                case 0x6: value1 < value2 ? pc += imm : pc += 4; break;
                case 0x7: value1 >= value2 ? pc += imm : pc += 4; break;
                default: break;
            }
        }
        default: break;
    }
}

void Simulator::executeSType(uint32_t inst) {
    uint8_t opcode = bits(inst, 6, 0);
    uint8_t rs1 = bits(inst, 19, 15);
    uint8_t rs2 = bits(inst, 24, 20);
    uint8_t funct3 = bits(inst, 14, 12);
    uint32_t imm = signExtend(bits(inst, 31, 25) << 5 | bits(inst, 11, 7), 12);
    uint32_t value1 = rf[rs1];
    uint32_t value2 = rf[rs2];
    switch(opcode){
        case 0x23: {
            instStat.storeInsts++;
            switch(funct3){
                case 0x0: memory->refMemoryWrite(value1 + imm, value2, 0x1); break;
                case 0x1: memory->refMemoryWrite(value1 + imm, value2, 0x3); break;
                case 0x2: memory->refMemoryWrite(value1 + imm, value2, 0xf); break;
                default: break;
            }
            pc += 4;
            break;
        }
        case 0x27: {  // FSW
            instStat.fStoreInsts++;
            if(funct3 == 0x2) {
                uint32_t data;
                memcpy(&data, &fpr[rs2], sizeof(float));
                memory->refMemoryWrite(value1 + imm, data, 0xf);
            }
            pc += 4;
            break;
        }
        default: break;
    }
}

void Simulator::executeUType(uint32_t inst) {
    uint8_t opcode = bits(inst, 6, 0);
    uint8_t rd = bits(inst, 11, 7);
    uint32_t imm = bits(inst, 31, 12) << 12;
    uint32_t result = 0;
    
    switch(opcode){
        case 0x37: instStat.aluInsts++; result = imm; stageIntWrite(rd, result); break;
        case 0x17: instStat.aluInsts++; result = pc + imm; stageIntWrite(rd, result); break;
        default: break;
    }
    pc += 4;
}

void Simulator::executeJType(uint32_t inst) {
    uint8_t opcode = bits(inst, 6, 0);
    uint8_t rd = bits(inst, 11, 7);
    uint32_t imm = signExtend(bits(inst, 31, 31) << 20 | (bits(inst, 19, 12) << 12) | (bits(inst, 20, 20) << 11) | (bits(inst, 30, 21) << 1), 21);
    switch(opcode){
        case 0x6F: {  // JAL
            instStat.branchInsts++;
            stageIntWrite(rd, pc + 4);
            pc += imm;
            break;
        }
        default: break;
    }
}

void Simulator::executeFType(uint32_t inst) {
    uint8_t opcode = bits(inst, 6, 0);
    uint8_t rd = bits(inst, 11, 7);
    uint8_t rs1 = bits(inst, 19, 15);
    uint8_t rs2 = bits(inst, 24, 20);
    uint8_t funct7 = bits(inst, 31, 25);
    uint8_t funct3 = bits(inst, 14, 12);
    float value1 = fpr[rs1];
    float value2 = fpr[rs2];
    
    if(opcode == 0x53) {
        instStat.fpuInsts++;
        switch(funct7) {
            case 0x00: {  // FADD.S
                stageFloatWrite(rd, value1 + value2);
                break;
            }
            case 0x04: {  // FSUB.S
                stageFloatWrite(rd, value1 - value2);
                break;
            }
            case 0x08: {  // FMUL.S
                stageFloatWrite(rd, value1 * value2);
                break;
            }
            case 0x0C: {  // FDIV.S
                stageFloatWrite(rd, value2 != 0.0f ? value1 / value2 : 0.0f);
                break;
            }
            case 0x2C: {  // FSQRT.S
                if(rs2 == 0) {
                    stageFloatWrite(rd, value1 >= 0.0f ? sqrtf(value1) : 0.0f);
                }
                break;
            }
            case 0x10: {  // FSGNJ.S, FSGNJN.S, FSGNJX.S
                switch(funct3) {
                    case 0x0: {  // FSGNJ.S
                        uint32_t sign2;
                        memcpy(&sign2, &value2, sizeof(uint32_t));
                        uint32_t sign1;
                        memcpy(&sign1, &value1, sizeof(uint32_t));
                        uint32_t resultBits = (sign1 & 0x7FFFFFFF) | (sign2 & 0x80000000);
                        float floatResult;
                        memcpy(&floatResult, &resultBits, sizeof(float));
                        stageFloatWrite(rd, floatResult);
                        break;
                    }
                    case 0x1: {  // FSGNJN.S
                        uint32_t sign2;
                        memcpy(&sign2, &value2, sizeof(uint32_t));
                        uint32_t sign1;
                        memcpy(&sign1, &value1, sizeof(uint32_t));
                        uint32_t resultBits = (sign1 & 0x7FFFFFFF) | ((~sign2) & 0x80000000);
                        float floatResult;
                        memcpy(&floatResult, &resultBits, sizeof(float));
                        stageFloatWrite(rd, floatResult);
                        break;
                    }
                    case 0x2: {  // FSGNJX.S
                        uint32_t sign1, sign2;
                        memcpy(&sign1, &value1, sizeof(uint32_t));
                        memcpy(&sign2, &value2, sizeof(uint32_t));
                        uint32_t resultBits = (sign1 & 0x7FFFFFFF) | ((sign1 ^ sign2) & 0x80000000);
                        float floatResult;
                        memcpy(&floatResult, &resultBits, sizeof(float));
                        stageFloatWrite(rd, floatResult);
                        break;
                    }
                    default: break;
                }
                break;
            }
            case 0x14: {  // FMIN.S, FMAX.S
                switch(funct3) {
                    case 0x0: {  // FMIN.S
                        stageFloatWrite(rd, (value1 < value2) ? value1 : value2);
                        break;
                    }
                    case 0x1: {  // FMAX.S
                        stageFloatWrite(rd, (value1 > value2) ? value1 : value2);
                        break;
                    }
                    default: break;
                }
                break;
            }
            case 0x50: {  // FEQ.S, FLT.S, FLE.S (结果写入整数寄存器)
                switch(funct3) {
                    case 0x2: {  // FEQ.S
                        stageIntWrite(rd, (value1 == value2) ? 1 : 0);
                        break;
                    }
                    case 0x1: {  // FLT.S
                        stageIntWrite(rd, (value1 < value2) ? 1 : 0);
                        break;
                    }
                    case 0x0: {  // FLE.S
                        stageIntWrite(rd, (value1 <= value2) ? 1 : 0);
                        break;
                    }
                    default: break;
                }
                break;
            }
            case 0x60: {  // FCVT.W.S, FCVT.WU.S (结果写入整数寄存器)
                switch(rs2) {
                    case 0x0: {  // FCVT.W.S
                        stageIntWrite(rd, (int32_t)value1);
                        break;
                    }
                    case 0x1: {  // FCVT.WU.S
                        stageIntWrite(rd, value1 >= 0.0f ? (uint32_t)value1 : 0);
                        break;
                    }
                    default: break;
                }
                break;
            }
            case 0x68: {  // FCVT.S.W, FCVT.S.WU (从整数转换为浮点)
                switch(rs2) {
                    case 0x0: {  // FCVT.S.W
                        stageFloatWrite(rd, (float)(int32_t)rf[rs1]);
                        break;
                    }
                    case 0x1: {  // FCVT.S.WU
                        stageFloatWrite(rd, (float)rf[rs1]);
                        break;
                    }
                    default: break;
                }
                break;
            }
            case 0x70: {  // FMV.X.W, FCLASS.S (结果写入整数寄存器)
                switch(funct3) {
                    case 0x0: {  // FMV.X.W
                        uint32_t intResult;
                        memcpy(&intResult, &value1, sizeof(uint32_t));
                        stageIntWrite(rd, intResult);
                        break;
                    }
                    case 0x1: {  // FCLASS.S
                        uint32_t fbits;
                        memcpy(&fbits, &value1, sizeof(uint32_t));
                        uint32_t sign = fbits >> 31;
                        uint32_t exp = (fbits >> 23) & 0xFF;
                        uint32_t mantissa = fbits & 0x7FFFFF;
                        uint32_t result = 0;
                        if(exp == 0xFF && mantissa == 0) {
                            result = sign ? (1 << 0) : (1 << 5);
                        } else if(exp == 0xFF && mantissa != 0) {
                            if(mantissa & 0x400000) {
                                result = (1 << 7);
                            } else {
                                result = (1 << 6);
                            }
                        } else if(exp == 0 && mantissa == 0) {
                            result = sign ? (1 << 2) : (1 << 3);
                        } else {
                            if(exp == 0) {
                                result = sign ? (1 << 1) : (1 << 4);
                            } else {
                                result = sign ? (1 << 1) : (1 << 4);
                            }
                        }
                        stageIntWrite(rd, result);
                        break;
                    }
                    default: break;
                }
                break;
            }
            case 0x78: {  // FMV.W.X (从整数移动到浮点)
                if(funct3 == 0x0) {
                    uint32_t data = rf[rs1];
                    float floatResult;
                    memcpy(&floatResult, &data, sizeof(float));
                    stageFloatWrite(rd, floatResult);
                }
                break;
            }
            default: break;
        }
    }
    pc += 4;
}

void Simulator::executeR4Type(uint32_t inst) {
    uint8_t opcode = bits(inst, 6, 0);
    uint8_t rd = bits(inst, 11, 7);
    uint8_t rs1 = bits(inst, 19, 15);
    uint8_t rs2 = bits(inst, 24, 20);
    uint8_t rs3 = bits(inst, 31, 27);
    float value1 = fpr[rs1];
    float value2 = fpr[rs2];
    float value3 = fpr[rs3];
    
    instStat.fpuInsts++;
    switch(opcode) {
        case 0x43: {  // FMADD.S: rd = rs1 * rs2 + rs3
            stageFloatWrite(rd, value1 * value2 + value3);
            break;
        }
        case 0x47: {  // FMSUB.S: rd = rs1 * rs2 - rs3
            stageFloatWrite(rd, value1 * value2 - value3);
            break;
        }
        case 0x4B: {  // FNMSUB.S: rd = -(rs1 * rs2) + rs3
            stageFloatWrite(rd, -(value1 * value2) + value3);
            break;
        }
        case 0x4F: {  // FNMADD.S: rd = -(rs1 * rs2) - rs3
            stageFloatWrite(rd, -(value1 * value2) - value3);
            break;
        }
        default: break;
    }
    pc += 4;
}