#pragma once

// LAN multiplayer session using ENet.
//
// Broadcasts player position and race state across a LAN so two or more
// instances of the port can see each other's craft. Designed to be driven
// once per game frame from the same hook that haptics.cpp uses
// (osViSwapBuffer), mirroring its threading model: capture() on the game
// thread, poll() on the main thread.
//
// Uses ENet over UDP. ENet was installed as libenet-dev (1.3.18) and the
// pkg-config module is "libenet". A host creates a session and other clients
// join by broadcast discovery or by direct address.

#include <cstdint>
#include <string>
#include <functional>
#include <array>

namespace wr64 {

// Wave Race 64 memory addresses for reading player/craft state from RDRAM.
// These come from docs/GAME-INTERNALS.md and are verified against haptics.cpp.
namespace rdram {

// Game globals
constexpr uint32_t kTick        = 0x80151960;  // frame counter
constexpr uint32_t kCourse      = 0x800D8170;  // course id, 0-8
constexpr uint32_t kGameState   = 0x800DAB24;  // 0x28 is a race
constexpr uint32_t kPlayers     = 0x800DAB28;  // 1 or 2
constexpr uint32_t kRiders      = 0x801982F0;  // riders on the course, 1-4
constexpr uint32_t kHumanSlot   = 0x800D48DC;  // which race slot the human holds
constexpr uint32_t kPauseFlag   = 0x801CE624;  // half; 0xFFFF while not paused
constexpr uint32_t kRacePhase   = 0x801CE650;  // 2 and 3 are the race proper
constexpr uint32_t kRaceMode    = 0x801CE648;  // 3 while a race is under way

// Per-craft arrays (indexed by race slot)
constexpr uint32_t kCraftBase   = 0x80192690;
constexpr uint32_t kCraftStride = 0x1718;

// Offsets within a craft struct
constexpr uint32_t kCraftPosOffsetX = 0x44;  // float, world x
constexpr uint32_t kCraftPosOffsetY = 0x48;  // float, world y (height)
constexpr uint32_t kCraftPosOffsetZ = 0x4C;  // float, world z
constexpr uint32_t kCraftHeading    = 0x0050; // float, heading (UNVERIFIED - set to 0 if not found)
constexpr uint32_t kCraftSpeed      = 0x0B90; // float, speed (HUD shows x1.8)

// Per-race-standing arrays (indexed by race slot)
constexpr uint32_t kRaceBase    = 0x801C2938;
constexpr uint32_t kRaceStride  = 0x0378;
constexpr uint32_t kRaceLap       = 0x0000;
constexpr uint32_t kRaceBuoy      = 0x000C;
constexpr uint32_t kRacePower     = 0x012C; // 0-5
constexpr uint32_t kRaceMisses    = 0x0134;
constexpr uint32_t kRaceRetired   = 0x02EC;
constexpr uint32_t kRaceFinished  = 0x02F4;

}  // namespace rdram

// A snapshot of one player's state, sent over the wire.
struct PlayerState {
    int32_t  slot       = -1;     // race slot (0-3)
    uint32_t tick       = 0;      // game tick this was sampled at
    uint32_t course     = 0;      // course id (0-8)
    float    x          = 0.0f;   // world position x
    float    y          = 0.0f;   // world position y (height)
    float    z          = 0.0f;   // world position z
    float    heading    = 0.0f;   // facing direction
    float    speed      = 0.0f;   // craft speed (game units)
    int32_t  lap        = 0;
    int32_t  buoys      = 0;
    int32_t  misses     = 0;
    int32_t  power      = 0;
    bool     finished   = false;
    bool     retired    = false;
};

// Network message types exchanged over ENet. The layout is fixed and small
// (48 bytes) to stay well within a single UDP packet.
enum class MessageType : uint8_t {
    Connect     = 0,
    Disconnect  = 1,
    PlayerState = 2,
    Discovery   = 3,
};

// Packed network message. Trivially copyable for direct send/receive.
struct NetworkPacket {
    MessageType type;
    uint8_t     sender_id;    // player slot or client index
    uint8_t     padding[2];   // for alignment
    uint32_t    tick;
    uint32_t    course;
    float       x, y, z;
    float       heading;
    float       speed;
    int32_t     lap;
    int32_t     buoys;
    int32_t     misses;
    int32_t     power;
    uint8_t     flags;        // bit 0 = finished, bit 1 = retired
    uint8_t     pad[3];
};

static_assert(sizeof(NetworkPacket) <= 64, "packet must fit in a single UDP frame");

// LANSession: ENet-based LAN session for synchronizing player positions.
//
// Threading: capture() is called on the game thread (once per game frame,
// after the game's update), and poll() is called on the main thread.
// Remote states are available through remote_state() and the on_state_received
// callback, both safe to call from any thread.
class LANSession {
public:
    LANSession();
    ~LANSession();

    // Non-copyable, non-movable (ENet host owns OS-level resources).
    LANSession(const LANSession&) = delete;
    LANSession& operator=(const LANSession&) = delete;

    // --- host mode ---

    // Create a LAN session as host on the given port. Returns false on error.
    bool create_host(uint16_t port = 14000, int max_players = 4);

    // --- client mode ---

    // Join a session at host_address (e.g. "192.168.1.10" or "127.0.0.1").
    // Uses the same default port as create_host.
    bool join(const std::string& host_address, uint16_t port = 14000,
              int player_slot = 0);

    // --- common ---

    // Disconnect and tear down ENet. Safe to call when not connected.
    void close();

    // Returns true if currently connected (host or client).
    bool is_connected() const;

    // Returns true if this instance is the host.
    bool is_host() const;

    // Returns this client's slot on the remote side, or -1 if not connected.
    int local_slot() const;

    // --- per-frame API ---

    // Called once per game frame on the game thread. Reads the player's craft
    // state from RDRAM and broadcasts it to all peers.
    void capture(const uint8_t* rdram);

    // Called once per main-thread poll. Services ENet's event queue and invokes
    // callbacks. Returns the number of network events processed.
    int poll();

    // --- state access ---

    // Get the latest known state of a remote player by slot.
    PlayerState remote_state(int slot) const;

    // Returns the number of connected peers (including self as host).
    int peer_count() const;

    // --- callbacks ---

    // Fired when a new player joins the session.
    std::function<void(int slot, const PlayerState&)> on_player_joined;

    // Fired when a player's state is received.
    std::function<void(int slot, const PlayerState&)> on_state_received;

    // Fired when a player leaves.
    std::function<void(int slot)> on_player_left;

private:
    struct Impl;
    Impl* m_impl;
};

}  // namespace wr64
