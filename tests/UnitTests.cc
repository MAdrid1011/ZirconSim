#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "Checkpoint.h"
#include "DeterministicRng.h"
#include "ElfImage.h"
#include "PlatformDevices.h"
#include "SpikeReference.h"
#include "Statistic.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: unit-tests <rv32-elf>" << std::endl;
        return 2;
    }

    zircon::sim::DeterministicRng first(1);
    zircon::sim::DeterministicRng second(1);
    for (int sample = 0; sample < 1024; ++sample) {
        assert(first.next64() == second.next64());
    }
    bool rejected_zero_seed = false;
    try {
        zircon::sim::DeterministicRng invalid(0);
    } catch (const std::invalid_argument &) {
        rejected_zero_seed = true;
    }
    assert(rejected_zero_seed);

    auto image = zircon::sim::ElfImage::load(argv[1]);
    assert(image.isElf());
    assert(image.entry() == 0x80000000u);
    assert(image.memory().read32(image.entry()) != 0u);
    const auto tohost = image.symbol("tohost");
    const auto fromhost = image.symbol("fromhost");
    assert(tohost.has_value());
    assert(fromhost.has_value());
    assert(*fromhost == *tohost + 4u);

    zircon::sim::SpikeReference reference(image.memory(), image.entry());
    const auto first_reference_commit = reference.next();
    assert(first_reference_commit.has_value());
    assert(first_reference_commit->pc == image.entry());
    assert(first_reference_commit->instruction == image.memory().read32(image.entry()));

    zircon::sim::TestExitMonitor monitor(*tohost);
    assert(!monitor.observeWrite(*tohost, 0, 0xf).has_value());
    assert(monitor.observeWrite(*tohost, 1, 0xf).value() == 0);

    zircon::sim::TestExitMonitor failure_monitor(*tohost);
    assert(failure_monitor.observeWrite(*tohost, 7, 0xf).value() == 3);

    zircon::sim::PlatformDevices platform(false, "A");
    uint32_t bus_data = 0;
    assert(platform.readBus(zircon::sim::PlatformDevices::kUartBase + 5, 1, bus_data));
    assert(bus_data == 0x6100u);
    assert(platform.readBus(zircon::sim::PlatformDevices::kUartBase, 1, bus_data));
    assert(bus_data == static_cast<uint32_t>('A'));
    assert(platform.writeBus(zircon::sim::PlatformDevices::kUartBase, static_cast<uint32_t>('Z'), 1));
    assert(platform.uartOutput() == "Z");
    const uint32_t timer_compare = zircon::sim::PlatformDevices::kClintBase + 0x4000u;
    assert(platform.writeBus(timer_compare, 2, 0xf));
    assert(platform.writeBus(timer_compare + 4, 0, 0xf));
    assert(!platform.timerInterrupt());
    for (uint32_t cycle = 0; cycle < zircon::sim::PlatformDevices::kTimerDivider - 1; ++cycle) {
        platform.tick();
    }
    assert(platform.time() == 0);
    platform.tick();
    assert(platform.time() == 1);
    assert(!platform.timerInterrupt());
    for (uint32_t cycle = 0; cycle < zircon::sim::PlatformDevices::kTimerDivider; ++cycle) {
        platform.tick();
    }
    assert(platform.time() == 2);
    assert(platform.timerInterrupt());

    const auto integer_commit = zircon::sim::parseSpikeCommitLine("core   0: 3 0x80000004 (0x00011117) x2 0x80011004");
    assert(integer_commit.has_value());
    assert(integer_commit->pc == 0x80000004u);
    assert(integer_commit->instruction == 0x00011117u);
    assert(integer_commit->write_valid && !integer_commit->is_fp);
    assert(integer_commit->rd == 2 && integer_commit->value == 0x80011004u);
    const auto floating_commit =
        zircon::sim::parseSpikeCommitLine("core   0: 3 0x80000020 (0x00000053) f10 0x3f800000");
    assert(floating_commit.has_value());
    assert(floating_commit->write_valid && floating_commit->is_fp);
    assert(floating_commit->rd == 10 && floating_commit->value == 0x3f800000u);

    const auto store_commit =
        zircon::sim::parseSpikeCommitLine("core   0: 3 0x8000003c (0x00112623) mem 0x80010ffc 0x80000010");
    assert(store_commit.has_value() && !store_commit->write_valid);
    assert(zircon::sim::SpikeReference::requiresSynchronization(0xf12027f3u));
    assert(zircon::sim::SpikeReference::requiresSynchronization(0xf14027f3u));
    assert(zircon::sim::SpikeReference::requiresSynchronization(0xb0002573u));
    assert(zircon::sim::SpikeReference::requiresSynchronization(0xc0102573u));
    assert(!zircon::sim::SpikeReference::requiresSynchronization(0x34102573u));

    const std::filesystem::path checkpoint_path =
        std::filesystem::temp_directory_path() / "zircon-spike-checkpoint-unit.bin";
    zircon::sim::SparseMemory checkpoint_program;
    checkpoint_program.write32(0x80000000u, 0x00100093u); // addi x1, x0, 1
    checkpoint_program.write32(0x80000004u, 0x00208113u); // addi x2, x1, 2
    zircon::sim::SpikeReference checkpoint_reference(checkpoint_program, 0x80000000u);
    assert(checkpoint_reference.next()->pc == 0x80000000u);
    {
        zircon::sim::CheckpointWriter writer(checkpoint_path);
        checkpoint_reference.save(writer);
        writer.close();
    }
    const auto expected_checkpoint_commit = checkpoint_reference.next();
    zircon::sim::SpikeReference restored_checkpoint_reference(checkpoint_program, 0x80000000u);
    {
        zircon::sim::CheckpointReader reader(checkpoint_path);
        restored_checkpoint_reference.restore(reader);
        reader.requireEnd();
    }
    std::filesystem::remove(checkpoint_path);
    const auto restored_checkpoint_commit = restored_checkpoint_reference.next();
    assert(expected_checkpoint_commit.has_value() && restored_checkpoint_commit.has_value());
    assert(expected_checkpoint_commit->pc == restored_checkpoint_commit->pc);
    assert(expected_checkpoint_commit->instruction == restored_checkpoint_commit->instruction);
    assert(expected_checkpoint_commit->next_pc == restored_checkpoint_commit->next_pc);
    assert(expected_checkpoint_commit->write_valid == restored_checkpoint_commit->write_valid);
    assert(expected_checkpoint_commit->rd == restored_checkpoint_commit->rd);
    assert(expected_checkpoint_commit->value == restored_checkpoint_commit->value);

    zircon::sim::SparseMemory counter_program;
    counter_program.write32(0x80000000u, 0xb00022f3u); // csrrs x5, mcycle, x0
    counter_program.write32(0x80000004u, 0x00128313u); // addi x6, x5, 1
    zircon::sim::SpikeReference counter_reference(counter_program, 0x80000000u);
    const auto counter_commit = counter_reference.next();
    assert(counter_commit->pc == 0x80000000u);
    zircon::sim::SpikeArchitecturalState synchronized_state;
    synchronized_state.integer[5] = 0x1234u;
    counter_reference.synchronize(synchronized_state);
    const auto dependent_commit = counter_reference.next();
    assert(dependent_commit->pc == 0x80000004u);
    assert(dependent_commit->write_valid && dependent_commit->rd == 6);
    assert(dependent_commit->value == 0x1235u);

    zircon::sim::SparseMemory signed_sync_program;
    signed_sync_program.write32(0x80000000u, 0xb00022f3u); // csrrs x5, mcycle, x0
    signed_sync_program.write32(0x80000004u, 0xc1405337u); // lui x6, 0xc1405
    signed_sync_program.write32(0x80000008u, 0x34030313u); // addi x6, x6, 0x340
    signed_sync_program.write32(0x8000000cu, 0x00629463u); // bne x5, x6, +8
    signed_sync_program.write32(0x80000010u, 0x00100393u); // addi x7, x0, 1
    zircon::sim::SpikeReference signed_sync_reference(signed_sync_program, 0x80000000u);
    assert(signed_sync_reference.next()->pc == 0x80000000u);
    zircon::sim::SpikeArchitecturalState signed_sync_state;
    signed_sync_state.integer[5] = 0xc1405340u;
    signed_sync_reference.synchronize(signed_sync_state);
    assert(signed_sync_reference.next()->pc == 0x80000004u);
    assert(signed_sync_reference.next()->pc == 0x80000008u);
    const auto signed_compare = signed_sync_reference.next();
    assert(signed_compare->pc == 0x8000000cu && signed_compare->next_pc == 0x80000010u);

    zircon::sim::SparseMemory trap_program;
    trap_program.write32(0x80000000u, 0x800002b7u); // lui x5, 0x80000
    trap_program.write32(0x80000004u, 0x01028293u); // addi x5, x5, 16
    trap_program.write32(0x80000008u, 0x30529073u); // csrw mtvec, x5
    trap_program.write32(0x8000000cu, 0x00000073u); // ecall
    trap_program.write32(0x80000010u, 0x00700313u); // addi x6, x0, 7
    zircon::sim::SpikeReference trap_reference(trap_program, 0x80000000u);
    assert(trap_reference.next()->pc == 0x80000000u);
    assert(trap_reference.next()->pc == 0x80000004u);
    assert(trap_reference.next()->pc == 0x80000008u);
    const auto handler_commit = trap_reference.next();
    assert(handler_commit->pc == 0x80000010u);
    assert(handler_commit->write_valid && handler_commit->rd == 6 && handler_commit->value == 7u);

    zircon::sim::SparseMemory virtual_program;
    virtual_program.write32(0x80000000u, 0xc00002b7u); // lui x5, 0xc0000
    virtual_program.write32(0x80000004u, 0x34129073u); // csrw mepc, x5
    virtual_program.write32(0x80000008u, 0x000012b7u); // lui x5, 1
    virtual_program.write32(0x8000000cu, 0x80028293u); // addi x5, x5, -2048
    virtual_program.write32(0x80000010u, 0x30029073u); // csrw mstatus, x5
    virtual_program.write32(0x80000014u, 0x800802b7u); // lui x5, 0x80080
    virtual_program.write32(0x80000018u, 0x00128293u); // addi x5, x5, 1
    virtual_program.write32(0x8000001cu, 0x18029073u); // csrw satp, x5
    virtual_program.write32(0x80000020u, 0x30200073u); // mret
    virtual_program.write32(0x80001c00u, 0x20000801u); // Root VPN 0x300 -> 0x80002000
    virtual_program.write32(0x80002000u, 0x20000c4bu); // Leaf VPN 0 -> 0x80003000, R-X-A
    virtual_program.write32(0x80003000u, 0x00900313u); // addi x6, x0, 9
    zircon::sim::SpikeReference virtual_reference(virtual_program, 0x80000000u);
    for (uint32_t pc = 0x80000000u; pc <= 0x80000020u; pc += 4) {
        assert(virtual_reference.next()->pc == pc);
    }
    const auto virtual_commit = virtual_reference.next();
    assert(virtual_commit->pc == 0xc0000000u);
    assert(virtual_commit->instruction == 0x00900313u);
    assert(virtual_commit->write_valid && virtual_commit->rd == 6 && virtual_commit->value == 9u);

    zircon::sim::SparseMemory dirty_fault_program;
    dirty_fault_program.write32(0x80000000u, 0xc00002b7u); // lui x5, 0xc0000
    dirty_fault_program.write32(0x80000004u, 0x04028293u); // addi x5, x5, 64
    dirty_fault_program.write32(0x80000008u, 0x10529073u); // csrw stvec, x5
    dirty_fault_program.write32(0x8000000cu, 0x000082b7u); // lui x5, 8
    dirty_fault_program.write32(0x80000010u, 0x30229073u); // csrw medeleg, x5
    dirty_fault_program.write32(0x80000014u, 0xc00002b7u); // lui x5, 0xc0000
    dirty_fault_program.write32(0x80000018u, 0x34129073u); // csrw mepc, x5
    dirty_fault_program.write32(0x8000001cu, 0x000012b7u); // lui x5, 1
    dirty_fault_program.write32(0x80000020u, 0x80028293u); // addi x5, x5, -2048
    dirty_fault_program.write32(0x80000024u, 0x30029073u); // csrw mstatus, x5
    dirty_fault_program.write32(0x80000028u, 0x800802b7u); // lui x5, 0x80080
    dirty_fault_program.write32(0x8000002cu, 0x00128293u); // addi x5, x5, 1
    dirty_fault_program.write32(0x80000030u, 0x18029073u); // csrw satp, x5
    dirty_fault_program.write32(0x80000034u, 0x30200073u); // mret
    dirty_fault_program.write32(0x80001c00u, 0x20000801u); // Root VPN 0x300 -> 0x80002000
    dirty_fault_program.write32(0x80002000u, 0x20000c4bu); // Code page: R-X-A
    dirty_fault_program.write32(0x80002004u, 0x20001047u); // Data page: RW-A, D=0
    dirty_fault_program.write32(0x80003000u, 0xc0001337u); // lui x6, 0xc0001
    dirty_fault_program.write32(0x80003004u, 0x00900393u); // addi x7, x0, 9
    dirty_fault_program.write32(0x80003008u, 0x00732023u); // sw x7, 0(x6)
    dirty_fault_program.write32(0x80003040u, 0x00800413u); // addi x8, x0, 8
    zircon::sim::SpikeReference dirty_fault_reference(dirty_fault_program, 0x80000000u);
    for (uint32_t pc = 0x80000000u; pc <= 0x80000034u; pc += 4) {
        assert(dirty_fault_reference.next()->pc == pc);
    }
    assert(dirty_fault_reference.next()->pc == 0xc0000000u);
    assert(dirty_fault_reference.next()->pc == 0xc0000004u);
    const auto dirty_fault_handler = dirty_fault_reference.next();
    assert(dirty_fault_handler->pc == 0xc0000040u);
    assert(dirty_fault_handler->write_valid && dirty_fault_handler->rd == 8 && dirty_fault_handler->value == 8u);

    zircon::sim::SparseMemory injected_fault_program;
    injected_fault_program.write32(0x80000000u, 0x800002b7u); // lui x5, 0x80000
    injected_fault_program.write32(0x80000004u, 0x06028293u); // addi x5, x5, 96
    injected_fault_program.write32(0x80000008u, 0x10529073u); // csrw stvec, x5
    injected_fault_program.write32(0x8000000cu, 0x000082b7u); // lui x5, 8
    injected_fault_program.write32(0x80000010u, 0x30229073u); // csrw medeleg, x5
    injected_fault_program.write32(0x80000014u, 0x800002b7u); // lui x5, 0x80000
    injected_fault_program.write32(0x80000018u, 0x04028293u); // addi x5, x5, 64
    injected_fault_program.write32(0x8000001cu, 0x34129073u); // csrw mepc, x5
    injected_fault_program.write32(0x80000020u, 0x000012b7u); // lui x5, 1
    injected_fault_program.write32(0x80000024u, 0x80028293u); // addi x5, x5, -2048
    injected_fault_program.write32(0x80000028u, 0x30029073u); // csrw mstatus, x5
    injected_fault_program.write32(0x8000002cu, 0x30200073u); // mret
    injected_fault_program.write32(0x80000040u, 0x00600313u); // addi x6, x0, 6
    injected_fault_program.write32(0x80000044u, 0x00702023u); // sw x7, 0(x0)
    injected_fault_program.write32(0x80000060u, 0x00800413u); // addi x8, x0, 8
    zircon::sim::SpikeReference injected_fault_reference(injected_fault_program, 0x80000000u);
    for (uint32_t pc = 0x80000000u; pc <= 0x8000002cu; pc += 4) {
        assert(injected_fault_reference.next()->pc == pc);
    }
    assert(injected_fault_reference.next()->pc == 0x80000040u);
    injected_fault_reference.injectPageFault({15, 0x80000044u, 0xa0021014u, 1});
    const auto injected_fault_handler = injected_fault_reference.next();
    assert(injected_fault_handler->pc == 0x80000060u);
    assert(injected_fault_handler->write_valid && injected_fault_handler->rd == 8 &&
           injected_fault_handler->value == 8u);

    zircon::sim::SparseMemory interrupt_program;
    interrupt_program.write32(0x80000000u, 0x800002b7u); // lui x5, 0x80000
    interrupt_program.write32(0x80000004u, 0x02028293u); // addi x5, x5, 32
    interrupt_program.write32(0x80000008u, 0x30529073u); // csrw mtvec, x5
    interrupt_program.write32(0x8000000cu, 0x08000293u); // addi x5, x0, 128
    interrupt_program.write32(0x80000010u, 0x30429073u); // csrw mie, x5
    interrupt_program.write32(0x80000014u, 0x00800293u); // addi x5, x0, 8
    interrupt_program.write32(0x80000018u, 0x3002a073u); // csrs mstatus, x5
    interrupt_program.write32(0x8000001cu, 0x00100093u); // addi x1, x0, 1
    interrupt_program.write32(0x80000020u, 0x00200113u); // addi x2, x0, 2
    zircon::sim::SpikeReference interrupt_reference(interrupt_program, 0x80000000u);
    for (uint32_t pc = 0x80000000u; pc <= 0x80000018u; pc += 4) {
        assert(interrupt_reference.next()->pc == pc);
    }
    interrupt_reference.injectInterrupt({0x80000007u, 0x8000001cu, 0, 3});
    const auto interrupt_handler_commit = interrupt_reference.next();
    assert(interrupt_handler_commit->pc == 0x80000020u);
    assert(interrupt_handler_commit->write_valid && interrupt_handler_commit->rd == 2 &&
           interrupt_handler_commit->value == 2u);

    zircon::sim::SparseMemory pending_interrupt_program;
    pending_interrupt_program.write32(0x80000000u, 0x800002b7u); // lui x5, 0x80000
    pending_interrupt_program.write32(0x80000004u, 0x02428293u); // addi x5, x5, 36
    pending_interrupt_program.write32(0x80000008u, 0x30529073u); // csrw mtvec, x5
    pending_interrupt_program.write32(0x8000000cu, 0x00800293u); // addi x5, x0, 8
    pending_interrupt_program.write32(0x80000010u, 0x30429073u); // csrw mie, x5
    pending_interrupt_program.write32(0x80000014u, 0x34429073u); // csrw mip, x5
    pending_interrupt_program.write32(0x80000018u, 0x3002a073u); // csrs mstatus, x5
    pending_interrupt_program.write32(0x8000001cu, 0x00100093u); // addi x1, x0, 1
    pending_interrupt_program.write32(0x80000020u, 0x00200113u); // addi x2, x0, 2
    pending_interrupt_program.write32(0x80000024u, 0x00300193u); // addi x3, x0, 3
    zircon::sim::SpikeReference pending_interrupt_reference(pending_interrupt_program, 0x80000000u);
    for (uint32_t pc = 0x80000000u; pc <= 0x8000001cu; pc += 4) {
        assert(pending_interrupt_reference.next()->pc == pc);
    }
    pending_interrupt_reference.injectInterrupt({0x80000003u, 0x80000020u, 0, 3});
    const auto pending_interrupt_handler_commit = pending_interrupt_reference.next();
    assert(pending_interrupt_handler_commit->pc == 0x80000024u);
    assert(pending_interrupt_handler_commit->write_valid && pending_interrupt_handler_commit->rd == 3 &&
           pending_interrupt_handler_commit->value == 3u);

    zircon::sim::Statistic statistic;
    statistic.observeInstruction(0x02000033u);
    statistic.observeInstruction(0x02004033u);
    statistic.observeInstruction(0x00000063u);
    statistic.observeInstruction(0x00008067u);
    assert(statistic.instructions().multiply == 1);
    assert(statistic.instructions().divide == 1);
    assert(statistic.instructions().branch == 1);
    assert(statistic.instructions().ret == 1);

    std::cout << "unit-tests: RNG, ELF32, platform devices, Sv32 Spike fetch, parsing, and statistics passed"
              << std::endl;
    return 0;
}
