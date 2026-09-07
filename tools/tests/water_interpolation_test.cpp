#include "../../lib/RT64/src/hle/rt64_water_interpolation.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace RT64::WaterInterpolation;

struct Mesh {
    std::vector<float> positions, texcoords;
    std::vector<uint32_t> indices;
    uint32_t count() const { return uint32_t(positions.size() / 3); }
};

// An oblique lattice equivalent to the game's staggered rows, with its actual
// integer Z quantization. Reanchor offsets deliberately need not coincide.
static Mesh grid(uint32_t width, float originX, float originZ, float time, bool nonlinear = false) {
    Mesh mesh;
    for (uint32_t row = 0; row < width; row++) {
        for (uint32_t col = 0; col < width; col++) {
            const float x = originX + col * 64.0f - row * 32.0f;
            const float z = originZ + std::round(row * 64.0f * std::sqrt(3.0f) / 2.0f);
            const float y = nonlinear ? 12.0f * std::sin(x / 100.0f + time) : x * 0.1f + z * 0.2f + time;
            mesh.positions.insert(mesh.positions.end(), {x, y, z});
            mesh.texcoords.insert(mesh.texcoords.end(), {x * 0.02f + z * 0.03f + time * 2, x * 0.01f - z * 0.01f + time * 3});
            if (row + 1 < width && col + 1 < width) {
                const uint32_t a = row * width + col, b = a + 1, c = a + width, d = c + 1;
                mesh.indices.insert(mesh.indices.end(), {a, b, d, a, d, c});
            }
        }
    }
    return mesh;
}

static void close(float a, float b, float tolerance = 0.0002f) {
    if (std::abs(a - b) > tolerance) {
        std::fprintf(stderr, "Expected %.8f, got %.8f\n", b, a);
        std::abort();
    }
}

int main() {
    const Mesh previous = grid(12, 0, 0, 0);
    const PreviousMesh history(previous.positions.data(), previous.texcoords.data(), previous.count(), previous.indices);

    // A 64-unit reanchor must sample the previous world point, not its old
    // array slot (which would introduce a spurious 6.4-unit height change).
    for (const auto origin : {std::pair<float, float>{64, 0}, {-32, 55}, {-32, 56}, {0.5f, 0.5f}}) {
        const Mesh current = grid(12, origin.first, origin.second, 2);
        std::vector<float> velocity(current.count() * 3, 999), tcVelocity(current.count() * 2, 999);
        const uint32_t matched = history.velocities(current.positions.data(), current.texcoords.data(), current.count(), velocity.data(), tcVelocity.data());
        assert(matched > current.count() / 2 && matched < current.count());
        for (uint32_t i = 0; i < current.count(); i++) {
            close(velocity[i * 3], 0); close(velocity[i * 3 + 2], 0);
            Sample sample;
            if (history.sample(current.positions[i * 3], current.positions[i * 3 + 2], sample)) {
                close(velocity[i * 3 + 1], 2);
                close(tcVelocity[i * 2], 4); close(tcVelocity[i * 2 + 1], 6);
                // At presentation alpha 0.5, the height is halfway through
                // time at the same world position, irrespective of reanchor.
                close(current.positions[i * 3 + 1] - velocity[i * 3 + 1] * 0.5f, sample.y + 1);
            }
            else {
                close(velocity[i * 3 + 1], 0);
                close(tcVelocity[i * 2], 0); close(tcVelocity[i * 2 + 1], 0);
            }
        }
        const float naive = current.positions[1] - previous.positions[1];
        if (origin.first == 64) assert(std::abs(naive - 2) > 6);
    }

    // Stationary deformations reproduce ordinary interpolation exactly.
    const Mesh still0 = grid(9, 100, -200, 0, true), still1 = grid(9, 100, -200, 0.05f, true);
    const PreviousMesh stillHistory(still0.positions.data(), still0.texcoords.data(), still0.count(), still0.indices);
    std::vector<float> velocity(still1.count() * 3), tcVelocity(still1.count() * 2);
    assert(stillHistory.velocities(still1.positions.data(), still1.texcoords.data(), still1.count(), velocity.data(), tcVelocity.data()) == still1.count());
    for (uint32_t i = 0; i < still1.count(); i++) close(velocity[i * 3 + 1], still1.positions[i * 3 + 1] - still0.positions[i * 3 + 1]);

    // Repeated RSP loads may duplicate a vertex. Equal copies preserve the
    // sample; conflicting surfaces reset it instead of guessing an average.
    Mesh duplicates = previous;
    duplicates.positions.insert(duplicates.positions.end(), previous.positions.begin(), previous.positions.begin() + 3);
    duplicates.texcoords.insert(duplicates.texcoords.end(), previous.texcoords.begin(), previous.texcoords.begin() + 2);
    Sample sample;
    PreviousMesh duplicateHistory(duplicates.positions.data(), duplicates.texcoords.data(), duplicates.count(), duplicates.indices);
    assert(duplicateHistory.sample(0, 0, sample)); close(sample.y, 0);
    duplicates.positions[duplicates.positions.size() - 2] = 10;
    PreviousMesh conflictHistory(duplicates.positions.data(), duplicates.texcoords.data(), duplicates.count(), duplicates.indices);
    assert(!conflictHistory.sample(0, 0, sample));
    assert(!history.sample(-10000, 10000, sample));
    assert(!history.sample(NAN, 0, sample));

    // Do not bridge resets, camera cuts, split-screen view swaps, or skipped
    // history. Paused, consecutive, and stationary frames remain eligible.
    History before;
    before.valid = true; before.course = 2; before.generation = 4;
    before.viewport = {0, 0, 1280, 480}; before.forward = {0, 0, 1};
    before.previousTime = 1; before.currentTime = 1.05f;
    History after = before;
    after.previousTime = before.currentTime; after.currentTime = 1.1f;
    after.camera = {64, 0, 0};
    assert(compatible(after, before));
    History changed = after; changed.generation++; assert(!compatible(changed, before));
    changed = after; changed.course++; assert(!compatible(changed, before));
    changed = after; changed.viewport = {0, 480, 1280, 960}; assert(!compatible(changed, before));
    changed = after; changed.camera[0] = 1000; assert(!compatible(changed, before));
    changed = after; changed.forward = {0, 0, -1}; assert(!compatible(changed, before));
    changed = after; changed.previousTime += 0.05f; assert(!compatible(changed, before));
    changed = after; changed.valid = false; assert(!compatible(changed, before));
    changed = after; changed.currentTime = changed.previousTime; assert(compatible(changed, before));

    // Time a grid larger than the original patch, including duplicate-free
    // spatial-index construction, to make accidental quadratic searches clear.
    const Mesh large0 = grid(40, 0, 0, 0), large1 = grid(40, -32, 55, 0.05f);
    velocity.resize(large1.count() * 3); tcVelocity.resize(large1.count() * 2);
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < 30; i++) {
        const PreviousMesh largeHistory(large0.positions.data(), large0.texcoords.data(), large0.count(), large0.indices);
        assert(largeHistory.velocities(large1.positions.data(), large1.texcoords.data(), large1.count(), velocity.data(), tcVelocity.data()) > 1400);
    }
    const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 30;
    std::printf("Water interpolation tests passed (1600 vertices + 3042 triangles: %.3f ms per source frame).\n", milliseconds);
}
