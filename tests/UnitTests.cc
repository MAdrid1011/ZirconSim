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
