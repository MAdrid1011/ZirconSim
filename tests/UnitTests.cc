#include <cassert>
#include <iostream>
#include <stdexcept>

#include "DeterministicRng.h"
#include "ElfImage.h"
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
    assert(zircon::sim::SpikeReference::requiresSynchronization(0xb0002573u));
    assert(zircon::sim::SpikeReference::requiresSynchronization(0xc0102573u));
    assert(!zircon::sim::SpikeReference::requiresSynchronization(0x34102573u));

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

    zircon::sim::Statistic statistic;
    statistic.observeInstruction(0x02000033u);
    statistic.observeInstruction(0x02004033u);
    statistic.observeInstruction(0x00000063u);
    statistic.observeInstruction(0x00008067u);
    assert(statistic.instructions().multiply == 1);
    assert(statistic.instructions().divide == 1);
    assert(statistic.instructions().branch == 1);
    assert(statistic.instructions().ret == 1);

    std::cout << "unit-tests: RNG, ELF32, tohost, Spike parsing, and statistics passed" << std::endl;
    return 0;
}
