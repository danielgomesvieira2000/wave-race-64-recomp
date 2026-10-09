# Task 7 Results: LAN Multiplayer Prototype with ENet

## Summary
Successfully implemented a LAN multiplayer prototype using ENet for player position synchronization.

## ENet Availability
- **Status**: INSTALLED
- **Package**: `libenet-dev 1.3.18+ds-1build1` (arm64)
- **Headers**: `/usr/include/enet/enet.h`
- **Library**: `/usr/lib/aarch64-linux-gnu/libenet.so`
- **pkg-config**: `libenet`

## Files Created

### 1. `include/wr64/lan_session.hpp`
C++ header defining the `LANSession` class for LAN multiplayer synchronization.

**Key components:**
- `PlayerState` struct: Contains player slot, position (x, y, z), heading, speed, lap, buoys, misses, power, finished, retired
- `NetworkPacket` struct: Packed 64-byte UDP message for transmission
- `LANSession` class: Manages ENet host/client connections, broadcasts/read player state
- Memory address constants in `rdram::` namespace:
  - Craft base: `0x80192690`, stride `0x1718`
  - Position offsets: x at `+0x44`, y at `+0x48`, z at `+0x4C`
  - Player count: `0x800DAB28`
  - Game state: `0x800DAB24` (0x28 = race)
  - Race standing: base `0x801C2938`, stride `0x378`

### 2. `src/network/lan_session.cpp`
Implementation of the LANSession class using ENet.

**Features:**
- `create_host(port, max_players)` - Create a LAN session as host
- `join(host_address, port, player_slot)` - Join an existing session
- `capture(rdram)` - Read player state from RDRAM and broadcast (called once per game frame)
- `poll()` - Service ENet events (called once per main-thread poll)
- Thread-safe access to remote player states via mutex
- Automatic slot assignment on connection
- Event callbacks: `on_player_joined`, `on_state_received`, `on_player_left`

### 3. `tools/test_lan_sync.cpp`
Standalone test verifying two ENet clients can synchronize position.

### 4. `tools/test_lan_sync.sh`
Build script for the standalone test.

## Test Results

```
=== LAN Session Sync Test (ENet) ===
Host created on port 14099
Client joined 127.0.0.1:14099 as slot 1
After connect: host polled 1 events, client polled 0 events (attempt 2)
PASS: host sees 2 peers (1 + client)
Client received host state: slot=0 x=100.0 y=0.5 z=200.0 speed=45.0 lap=1 buoys=3
PASS: host position (x=100, z=200, speed=45) received by client
Host received client state: slot=1 x=-50.0 y=0.0 z=500.0 speed=38.5
PASS: client position (x=-50, z=500, speed=38.5) received by host

--- Bidirectional sync iteration 2 ---
Client sees host at: x=150.0 z=250.0 speed=60.0
Host sees client at: x=-100.0 z=600.0 speed=55.0
PASS: bidirectional sync iteration 2 verified

=== ALL TESTS PASSED ===
```

## Integration with CMakeLists.txt

Added to `CMakeLists.txt`:
- New option: `WR64_WITH_NETWORK` (default OFF)
- Adds `src/network/lan_session.cpp` to build via `file(GLOB)`
- Links `libenet` via pkg-config (`PkgConfig::ENET`)

Usage:
```bash
cmake -DWR64_WITH_NETWORK=ON -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON ..
```

## Architecture Notes

The implementation follows the existing pattern from `src/haptics.cpp`:
1. **Game thread**: `capture()` is called once per game frame (hooked at `osViSwapBuffer`)
2. **Main thread**: `poll()` is called once per SDL event/poll cycle
3. **RDRAM reading**: Uses the same helper functions (`read_word`, `read_float`) as haptics

The ENet usage:
- 2 channels: 0 = reliable (state), 1 = unreliable (future input)
- Player state packets are ~48 bytes, well under UDP MTU
- Connections are by direct address (no automatic discovery broadcast)

## Limitations & Next Steps

1. **Automatic discovery**: Clients must know the host's IP. Could add UDP broadcast discovery later.
2. **Input synchronization**: Currently only position is synced. For real-time gameplay, would need to sync input commands.
3. **Prediction**: Clients would need to predict movement between state updates to mask latency.
4. **The header-only integration**: For a game port that actually uses this, need to hook into `callbacks.cpp` or similar where RDRAM is accessed.