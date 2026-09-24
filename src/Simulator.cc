#include "Simulator.h"

void Simulator::step(uint32_t num) {
    uint32_t inst = memory->refMemoryRead(pc);
    uint8_t opcode = bits(inst, 6, 0);
    // std::cout << std::hex <<  <<  std::dec << std::endl;
    switch(opcode){
        case 0x37: executeUType(inst); break;
        case 0x17: executeUType(inst); break;
        case 0x6F: executeJType(inst); break;
        case 0x67: executeIType(inst); break;
        case 0x63: executeBType(inst); break;
        case 0x03: executeIType(inst); break;
        case 0x23: executeSType(inst); break;
        case 0x13: executeIType(inst); break;
        case 0x33: executeRType(inst); break;
        default: break;
    }
    rf[0] = 0;
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
    switch (opcode) {
        case 0x33: {
            switch (funct7) {
                case 0x0: {
                    instStat.aluInsts++;
                    switch (funct3) {
                        case 0x0: rf[rd] = value1 + value2; break;
                        case 0x1: rf[rd] = value1 << value2; break;
                        case 0x2: rf[rd] = (int32_t)value1 < (int32_t)value2; break;
                        case 0x3: rf[rd] = value1 < value2; break;
                        case 0x4: rf[rd] = value1 ^ value2; break;
                        case 0x5: rf[rd] = value1 >> value2; break;
                        case 0x6: rf[rd] = value1 | value2; break;
                        case 0x7: rf[rd] = value1 & value2; break;
                        default: break;
                    }
                    break;
                }
                case 0x20: {
                    instStat.aluInsts++;
                    switch (funct3) {
                        case 0x0: rf[rd] = value1 - value2; break;
                        case 0x5: rf[rd] = (int32_t)value1 >> value2; break;
                        default: break;
                    }
                    break;
                }
                case 0x01: {
                    switch (funct3) {
                        case 0x0: instStat.mulInsts++; rf[rd] = value1 * value2; break;
                        case 0x1: instStat.mulInsts++; rf[rd] = ((int64_t)(int32_t)value1 * (int64_t)(int32_t)value2) >> 32; break;
                        case 0x2: instStat.mulInsts++; rf[rd] = ((int64_t)(int32_t)value1 * (uint64_t)value2) >> 32; break;
                        case 0x3: instStat.mulInsts++; rf[rd] = ((uint64_t)value1 * (uint64_t)value2) >> 32; break;
                        case 0x4: instStat.divInsts++; rf[rd] = value2 == 0 ? -1 : (int32_t)value1 / (int32_t)value2; break;
                        case 0x5: instStat.divInsts++; rf[rd] = value2 == 0 ? -1 : value1 / value2; break;
                        case 0x6: instStat.divInsts++; rf[rd] = value2 == 0 ? value1 : (int32_t)value1 % (int32_t)value2; break;
                        case 0x7: instStat.divInsts++; rf[rd] = value2 == 0 ? value1 : value1 % value2; break;
                        default: break;
                    }
                }
                default: break;
            }
            break;
        }
        default: break;
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
    switch(opcode){
        case 0x13: {
            instStat.aluInsts++;
            switch(funct3){
                case 0x0: rf[rd] = value1 + imm; break;
                case 0x1: rf[rd] = value1 << (imm & 0x1F); break;
                case 0x2: rf[rd] = (int32_t)value1 < (int32_t)imm; break;
                case 0x3: rf[rd] = value1 < imm; break;
                case 0x4: rf[rd] = value1 ^ imm; break;
                case 0x5: rf[rd] = inst & 0x40000000 ? (int32_t)value1 >> (imm & 0x1F) : value1 >> (imm & 0x1F); break;
                case 0x6: rf[rd] = value1 | imm; break;
                case 0x7: rf[rd] = value1 & imm; break;
                default: break;
            }
            pc += 4;
            break;
        }
        case 0x03: {
            instStat.loadInsts++;
            switch(funct3){
                case 0x0: rf[rd] = signExtend(memory->refMemoryRead(value1 + imm), 8); break;
                case 0x1: rf[rd] = signExtend(memory->refMemoryRead(value1 + imm), 16); break;
                case 0x2: rf[rd] = memory->refMemoryRead(value1 + imm); break;
                case 0x4: rf[rd] = zeroExtend(memory->refMemoryRead(value1 + imm), 8); break;
                case 0x5: rf[rd] = zeroExtend(memory->refMemoryRead(value1 + imm), 16); break;
                default: break;
            }
            pc += 4;
            break;
        }
        case 0x67: {
            instStat.branchInsts++;
            rf[rd] = pc + 4;
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
        }
        default: break;
    }
    pc += 4;
}

void Simulator::executeUType(uint32_t inst) {

    uint8_t opcode = bits(inst, 6, 0);
    uint8_t rd = bits(inst, 11, 7);
    uint32_t imm = bits(inst, 31, 12) << 12;
    switch(opcode){
        case 0x37: instStat.aluInsts++; rf[rd] = imm; break;
        case 0x17: instStat.aluInsts++; rf[rd] = pc + imm; break;
        default: break;
    }
    pc += 4;
}

void Simulator::executeJType(uint32_t inst) {
    uint8_t opcode = bits(inst, 6, 0);
    uint8_t rd = bits(inst, 11, 7);
    uint32_t imm = signExtend(bits(inst, 31, 31) << 20 | (bits(inst, 19, 12) << 12) | (bits(inst, 20, 20) << 11) | (bits(inst, 30, 21) << 1), 21);
    switch(opcode){
        case 0x6F: {
            instStat.branchInsts++;
            rf[rd] = pc + 4;
            pc += imm;
            break;
        }
        default: break;
    }
}
