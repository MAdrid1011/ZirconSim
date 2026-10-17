#include <cassert>
#include <filesystem>
#include <iostream>

#include "Checkpoint.h"
#include "SpikeReference.h"

int main() {
    zircon::sim::SparseMemory program;
    program.write32(0x80000000u, 0xc102f2d7u); // vsetivli x5, 5, e32, m1, tu, mu
    program.write32(0x80000004u, 0x0090d073u); // csrwi vxsat, 1
    program.write32(0x80000008u, 0xb2a49457u); // vfmacc.vv v8, v9, v10
    program.write32(0x8000000cu, 0x0200e427u); // vse32.v v8, (x1)
    zircon::sim::SpikeReference reference(program, 0x80000000u);
    zircon::sim::SpikeArchitecturalState initial;
    initial.integer[1] = 0x80001000u;
    reference.synchronize(initial);
    std::array<uint32_t, 8> values;
    values.fill(0x3f800000u); // 1.0f
    reference.vectorRegister(8, values);
    values.fill(0x40000000u); // 2.0f
    reference.vectorRegister(9, values);
    values.fill(0x40400000u); // 3.0f
    reference.vectorRegister(10, values);
    assert(reference.next()->pc == 0x80000000u);
    assert(reference.next()->pc == 0x80000004u);
    assert(reference.vectorState().vl == 5 && reference.vectorState().vxsat);

    const auto checkpoint = std::filesystem::temp_directory_path() / "zircon-spike-vector-unit.bin";
    {
        zircon::sim::CheckpointWriter writer(checkpoint);
        reference.save(writer);
        writer.close();
    }
    zircon::sim::SpikeReference restored(program, 0x80000000u);
    {
        zircon::sim::CheckpointReader reader(checkpoint);
        restored.restore(reader);
        reader.requireEnd();
    }
    std::filesystem::remove(checkpoint);
    assert(restored.vectorState().vl == 5);
    assert(restored.vectorState().vtype == reference.vectorState().vtype);
    assert(restored.vectorState().vxsat);
    for (uint32_t index = 0; index < 32; ++index) {
        assert(reference.vectorRegister(index) == restored.vectorRegister(index));
    }
    const auto expectedFma = reference.next();
    const auto restoredFma = restored.next();
    assert(expectedFma->vector.has_value() && restoredFma->vector.has_value());
    assert(expectedFma->pc == restoredFma->pc && expectedFma->instruction == restoredFma->instruction);
    const auto result = restored.vectorRegister(8);
    assert(result == reference.vectorRegister(8));
    for (size_t index = 0; index < result.size(); ++index) {
        assert(result[index] == (index < 5 ? 0x40e00000u : 0x3f800000u)); // Active 7.0f, tail 1.0f.
    }
    const auto expectedStore = reference.next();
    const auto restoredStore = restored.next();
    assert(expectedStore->vector.has_value() && restoredStore->vector.has_value());
    assert(expectedStore->vector->stores == restoredStore->vector->stores);
    assert(restoredStore->vector->stores.size() == 5 * sizeof(float));
    for (uint32_t offset = 0; offset < 8 * sizeof(float); ++offset) {
        assert(reference.memoryByte(0x80001000u + offset) == restored.memoryByte(0x80001000u + offset));
    }
    std::cout << "vector-reference: Zve32f FMA, tail preservation, stores, and checkpoint restore passed\n";
}
