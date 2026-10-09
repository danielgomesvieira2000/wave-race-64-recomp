// Standalone test: verifies that two ENet endpoints can exchange PlayerState
// packets with position data, simulating the LANSession sync protocol.
//
// Build: see tools/test_lan_sync.sh
// Run:   ./build/test_lan_sync
//
// This test does not require the full game runtime. It creates a fake RDRAM
// buffer with craft position data, then:
//   - Client A acts as host, writes position data and broadcasts
//   - Client B connects, receives position, writes its own, broadcasts
//   - Both read each other's positions and verify they match

#include "wr64/lan_session.hpp"

#include <enet/enet.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <thread>
#include <chrono>

// ENet needs global init/shutdown. The real port calls these once at startup.
static bool init_enet() {
    if (enet_initialize() != 0) {
        std::fprintf(stderr, "Failed to initialize ENet\n");
        return false;
    }
    std::atexit(enet_deinitialize);
    return true;
}

// Build a fake RDRAM buffer with craft data at the known addresses.
static constexpr size_t kRdramSize = 0x800000; // 8 MB
static std::unique_ptr<uint8_t[]> make_fake_rdram(int slot, float x, float y, float z, float speed) {
    auto ram = std::make_unique<uint8_t[]>(kRdramSize);
    std::memset(ram.get(), 0, kRdramSize);

    // Write the game tick (incrementing).
    uint32_t tick = 42;
    std::memcpy(ram.get() + (wr64::rdram::kTick & 0x7FFFFFu), &tick, 4);

    // Write player count = 2.
    uint32_t players = 2;
    std::memcpy(ram.get() + (wr64::rdram::kPlayers & 0x7FFFFFu), &players, 4);

    // Write game state = 0x28 (race).
    uint32_t state = 0x28;
    std::memcpy(ram.get() + (wr64::rdram::kGameState & 0x7FFFFFu), &state, 4);

    // Write course id = 0 (Dolphin Park).
    uint32_t course = 0;
    std::memcpy(ram.get() + (wr64::rdram::kCourse & 0x7FFFFFu), &course, 4);

    // Write craft position for the given slot.
    uint32_t craft_base = wr64::rdram::kCraftBase + slot * wr64::rdram::kCraftStride;
    craft_base &= 0x7FFFFFu;
    std::memcpy(ram.get() + craft_base + wr64::rdram::kCraftPosOffsetX, &x, 4);
    std::memcpy(ram.get() + craft_base + wr64::rdram::kCraftPosOffsetY, &y, 4);
    std::memcpy(ram.get() + craft_base + wr64::rdram::kCraftPosOffsetZ, &z, 4);
    std::memcpy(ram.get() + craft_base + wr64::rdram::kCraftSpeed, &speed, 4);

    // Write race standing: lap 1, 3 buoys, 0 misses, power 3.
    uint32_t race_base = wr64::rdram::kRaceBase + slot * wr64::rdram::kRaceStride;
    race_base &= 0x7FFFFFu;
    int32_t lap = 1, buoys = 3, misses = 0, power = 3;
    std::memcpy(ram.get() + race_base + wr64::rdram::kRaceLap, &lap, 4);
    std::memcpy(ram.get() + race_base + wr64::rdram::kRaceBuoy, &buoys, 4);
    std::memcpy(ram.get() + race_base + wr64::rdram::kRaceMisses, &misses, 4);
    std::memcpy(ram.get() + race_base + wr64::rdram::kRacePower, &power, 4);

    return ram;
}

using wr64::PlayerState;

int main() {
    if (!init_enet()) {
        return 1;
    }

    std::printf("=== LAN Session Sync Test (ENet) ===\n");

    // ---- Host (Player A, slot 0) ----
    wr64::LANSession host;
    if (!host.create_host(14099, 4)) {
        std::fprintf(stderr, "FAIL: host create_host failed\n");
        return 1;
    }
    std::printf("Host created on port 14099\n");

    // Prepare host's position data: Player A at (100.0, 0.5, 200.0), speed 45.0
    auto host_rdram = make_fake_rdram(0, 100.0f, 0.5f, 200.0f, 45.0f);

    // ---- Client (Player B, slot 1) ----
    wr64::LANSession client;
    if (!client.join("127.0.0.1", 14099, 1)) {
        std::fprintf(stderr, "FAIL: client join failed\n");
        return 1;
    }
    std::printf("Client joined 127.0.0.1:14099 as slot 1\n");

    // Poll both host and client in a loop to allow ENet to complete the
    // connection handshake (it needs multiple round-trips with timeouts).
    int attempts = 0;
    while (attempts < 20 && !client.is_connected()) {
        host.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ++attempts;
    }

    int host_poll = host.poll();
    int client_poll = client.poll();
    std::printf("After connect: host polled %d events, client polled %d events (attempt %d)\n",
                host_poll, client_poll, attempts);

    if (host_poll == 0) {
        std::fprintf(stderr, "FAIL: host did not receive connect event\n");
        return 1;
    }
    if (host.peer_count() < 2) {
        std::fprintf(stderr, "FAIL: host peer_count = %d (expected >= 2)\n", host.peer_count());
        return 1;
    }
    std::printf("PASS: host sees %d peers (1 + client)\n", host.peer_count());

    // ---- Host broadcasts position ----
    host.capture(host_rdram.get());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Client polls to receive the host's position.
    client.poll();

    PlayerState host_state = client.remote_state(0);
    std::printf("Client received host state: slot=%d x=%.1f y=%.1f z=%.1f speed=%.1f lap=%d buoys=%d\n",
                host_state.slot, host_state.x, host_state.y, host_state.z,
                host_state.speed, host_state.lap, host_state.buoys);

    if (host_state.slot != 0) {
        std::fprintf(stderr, "FAIL: expected host slot 0, got %d\n", host_state.slot);
        return 1;
    }
    if (std::abs(host_state.x - 100.0f) > 0.01f) {
        std::fprintf(stderr, "FAIL: host x = %.1f, expected 100.0\n", host_state.x);
        return 1;
    }
    if (std::abs(host_state.z - 200.0f) > 0.01f) {
        std::fprintf(stderr, "FAIL: host z = %.1f, expected 200.0\n", host_state.z);
        return 1;
    }
    if (std::abs(host_state.speed - 45.0f) > 0.01f) {
        std::fprintf(stderr, "FAIL: host speed = %.1f, expected 45.0\n", host_state.speed);
        return 1;
    }
    std::printf("PASS: host position (x=100, z=200, speed=45) received by client\n");

    // ---- Client broadcasts its position ----
    auto client_rdram = make_fake_rdram(1, -50.0f, 0.0f, 500.0f, 38.5f);
    client.capture(client_rdram.get());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Host polls to receive the client's position.
    host.poll();

    PlayerState client_state = host.remote_state(1);
    std::printf("Host received client state: slot=%d x=%.1f y=%.1f z=%.1f speed=%.1f\n",
                client_state.slot, client_state.x, client_state.y, client_state.z,
                client_state.speed);

    if (client_state.slot != 1) {
        std::fprintf(stderr, "FAIL: expected client slot 1, got %d\n", client_state.slot);
        return 1;
    }
    if (std::abs(client_state.x - (-50.0f)) > 0.01f) {
        std::fprintf(stderr, "FAIL: client x = %.1f, expected -50.0\n", client_state.x);
        return 1;
    }
    if (std::abs(client_state.z - 500.0f) > 0.01f) {
        std::fprintf(stderr, "FAIL: client z = %.1f, expected 500.0\n", client_state.z);
        return 1;
    }
    if (std::abs(client_state.speed - 38.5f) > 0.01f) {
        std::fprintf(stderr, "FAIL: client speed = %.1f, expected 38.5\n", client_state.speed);
        return 1;
    }
    std::printf("PASS: client position (x=-50, z=500, speed=38.5) received by host\n");

    // ---- Bidirectional: both move and sync again ----
    std::printf("\n--- Bidirectional sync iteration 2 ---\n");

    // Host moves to new position.
    host_rdram = make_fake_rdram(0, 150.0f, 1.0f, 250.0f, 60.0f);
    host.capture(host_rdram.get());

    // Client moves to new position.
    client_rdram = make_fake_rdram(1, -100.0f, 0.5f, 600.0f, 55.0f);
    client.capture(client_rdram.get());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    host.poll();
    client.poll();

    host_state = client.remote_state(0);
    client_state = host.remote_state(1);

    std::printf("Client sees host at: x=%.1f z=%.1f speed=%.1f\n",
                host_state.x, host_state.z, host_state.speed);
    std::printf("Host sees client at: x=%.1f z=%.1f speed=%.1f\n",
                client_state.x, client_state.z, client_state.speed);

    // Verify sync.
    bool host_ok = std::abs(host_state.x - 150.0f) < 0.01f &&
                   std::abs(host_state.z - 250.0f) < 0.01f &&
                   std::abs(host_state.speed - 60.0f) < 0.01f;
    bool client_ok = std::abs(client_state.x - (-100.0f)) < 0.01f &&
                     std::abs(client_state.z - 600.0f) < 0.01f &&
                     std::abs(client_state.speed - 55.0f) < 0.01f;

    if (!host_ok) {
        std::fprintf(stderr, "FAIL: host state not synced on iteration 2\n");
        return 1;
    }
    if (!client_ok) {
        std::fprintf(stderr, "FAIL: client state not synced on iteration 2\n");
        return 1;
    }
    std::printf("PASS: bidirectional sync iteration 2 verified\n");

    // ---- Cleanup ----
    host.close();
    client.close();

    std::printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
