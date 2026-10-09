// LAN multiplayer session using ENet.
//
// Broadcasts player position and race state across a LAN so instances of the
// port can synchronize. Driven once per game frame from the osViSwapBuffer hook
// (the same place haptics.cpp reads RDRAM), and polled once per main-thread
// iteration.
//
// ENet (libenet-dev 1.3.18) provides reliable UDP with channels, connection
// management, and host/client roles. We use two channels:
//   - channel 0: reliable, ordered  -- player state snapshots
//   - channel 1: unreliable          -- for future high-frequency input broadcast
//
// Memory addresses come from docs/GAME-INTERNALS.md, verified in haptics.cpp.

#include "wr64/lan_session.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>

#include <enet/enet.h>

namespace wr64 {

// ---- RDRAM reading helpers (mirrors haptics.cpp) ----

namespace {

int32_t read_word(const uint8_t* ram, uint32_t address) {
    int32_t v = 0;
    std::memcpy(&v, ram + (address & 0x7FFFFFu), sizeof(v));
    return v;
}

uint16_t read_half(const uint8_t* ram, uint32_t address) {
    uint16_t v = 0;
    std::memcpy(&v, ram + ((address & 0x7FFFFFu) ^ 2u), sizeof(v));
    return v;
}

float read_float(const uint8_t* ram, uint32_t address) {
    float v = 0.0f;
    std::memcpy(&v, ram + (address & 0x7FFFFFu), sizeof(v));
    return std::isfinite(v) ? v : 0.0f;
}

// Read the craft position for a given slot.
PlayerState read_player_state(const uint8_t* rdram, int slot) {
    PlayerState s;
    if (rdram == nullptr || slot < 0 || slot > 3) return s;

    s.slot   = slot;
    s.tick   = static_cast<uint32_t>(read_word(rdram, rdram::kTick));
    s.course = static_cast<uint32_t>(read_word(rdram, rdram::kCourse));

    const uint32_t craft_base = rdram::kCraftBase + static_cast<uint32_t>(slot) * rdram::kCraftStride;
    s.x      = read_float(rdram, craft_base + rdram::kCraftPosOffsetX);
    s.y      = read_float(rdram, craft_base + rdram::kCraftPosOffsetY);
    s.z      = read_float(rdram, craft_base + rdram::kCraftPosOffsetZ);
    s.speed  = read_float(rdram, craft_base + rdram::kCraftSpeed);

    // Heading: at offset 0x50 from craft struct (unverified against GAME-INTERNALS.md).
    // Falls back to 0.0f if not finite.
    s.heading = read_float(rdram, craft_base + rdram::kCraftHeading);

    const uint32_t race_base = rdram::kRaceBase + static_cast<uint32_t>(slot) * rdram::kRaceStride;
    s.lap    = read_word(rdram, race_base + rdram::kRaceLap);
    s.buoys  = read_word(rdram, race_base + rdram::kRaceBuoy);
    s.misses = read_word(rdram, race_base + rdram::kRaceMisses);
    s.power  = std::clamp(read_word(rdram, race_base + rdram::kRacePower), 0, 5);
    s.finished = read_word(rdram, race_base + rdram::kRaceFinished) != 0;
    s.retired  = read_word(rdram, race_base + rdram::kRaceRetired) != 0;

    return s;
}

// Convert a PlayerState to a packed network packet.
NetworkPacket state_to_packet(const PlayerState& s) {
    NetworkPacket pkt{};
    pkt.type       = MessageType::PlayerState;
    pkt.sender_id  = static_cast<uint8_t>(s.slot);
    pkt.tick       = s.tick;
    pkt.course     = s.course;
    pkt.x          = s.x;
    pkt.y          = s.y;
    pkt.z          = s.z;
    pkt.heading    = s.heading;
    pkt.speed      = s.speed;
    pkt.lap        = s.lap;
    pkt.buoys      = s.buoys;
    pkt.misses     = s.misses;
    pkt.power      = s.power;
    pkt.flags      = (s.finished ? 1 : 0) | (s.retired ? 2 : 0);
    return pkt;
}

// Convert a network packet back to a PlayerState.
PlayerState packet_to_state(const NetworkPacket& pkt) {
    PlayerState s;
    s.slot     = pkt.sender_id;
    s.tick     = pkt.tick;
    s.course   = pkt.course;
    s.x        = pkt.x;
    s.y        = pkt.y;
    s.z        = pkt.z;
    s.heading  = pkt.heading;
    s.speed    = pkt.speed;
    s.lap      = pkt.lap;
    s.buoys    = pkt.buoys;
    s.misses   = pkt.misses;
    s.power    = pkt.power;
    s.finished = (pkt.flags & 1) != 0;
    s.retired  = (pkt.flags & 2) != 0;
    return s;
}

}  // namespace

// ---- LANSession implementation ----

struct LANSession::Impl {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;  // when in client mode, the server peer
    bool      is_host = false;
    int       local_slot_val = -1;
    int       max_players = 4;
    bool      connected = false;  // set true once ENET_EVENT_TYPE_CONNECTED fires (client)

    // Remote states, protected by mutex (poll() writes, remote_state() reads).
    mutable std::mutex state_mutex;
    std::array<PlayerState, 4> remote_states{};

    // Track which slots are active.
    std::array<bool, 4> slot_active{};
};

LANSession::LANSession() : m_impl(new Impl()) {}

LANSession::~LANSession() {
    close();
    delete m_impl;
}

bool LANSession::create_host(uint16_t port, int max_players) {
    if (m_impl->host != nullptr) {
        std::fprintf(stderr, "[lan] create_host: already initialized\n");
        return false;
    }

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    // Two channels: 0 = reliable (state), 1 = unreliable (future input).
    // Bandwidth limits of 0 mean "unlimited", which is correct for LAN.
    m_impl->host = enet_host_create(&address, static_cast<size_t>(max_players),
        2,  /* channel count: 0 = reliable, 1 = unreliable */
        0,  /* incoming bandwidth: 0 = unlimited */
        0); /* outgoing bandwidth: 0 = unlimited */

    if (m_impl->host == nullptr) {
        std::fprintf(stderr, "[lan] create_host: enet_host_create failed\n");
        return false;
    }

    m_impl->is_host = true;
    m_impl->local_slot_val = 0;  // host is slot 0
    m_impl->max_players = max_players;
    m_impl->slot_active[0] = true;

    std::fprintf(stderr, "[lan] hosting on port %u, max %d players\n",
                 port, max_players);
    return true;
}

bool LANSession::join(const std::string& host_address, uint16_t port, int player_slot) {
    if (m_impl->host != nullptr) {
        std::fprintf(stderr, "[lan] join: already initialized\n");
        return false;
    }

    // Create a client host. Two channels: 0 = reliable, 1 = unreliable.
    m_impl->host = enet_host_create(nullptr, 1, 2, 0, 0);

    if (m_impl->host == nullptr) {
        std::fprintf(stderr, "[lan] join: enet_host_create failed\n");
        return false;
    }

    ENetAddress address{};
    if (enet_address_set_host(&address, host_address.c_str()) != 0) {
        std::fprintf(stderr, "[lan] join: could not resolve \"%s\"\n", host_address.c_str());
        enet_host_destroy(m_impl->host);
        m_impl->host = nullptr;
        return false;
    }
    address.port = port;

    m_impl->peer = enet_host_connect(m_impl->host, &address, 2, 0);
    if (m_impl->peer == nullptr) {
        std::fprintf(stderr, "[lan] join: enet_host_connect failed\n");
        enet_host_destroy(m_impl->host);
        m_impl->host = nullptr;
        return false;
    }

    m_impl->local_slot_val = player_slot;
    m_impl->is_host = false;

    std::fprintf(stderr, "[lan] joining %s:%u as slot %d\n",
                 host_address.c_str(), port, player_slot);
    return true;
}

void LANSession::close() {
    if (m_impl->host == nullptr) return;

    if (m_impl->peer != nullptr) {
        enet_peer_disconnect_now(m_impl->peer, 0);
        m_impl->peer = nullptr;
    }

    enet_host_destroy(m_impl->host);
    m_impl->host = nullptr;
    m_impl->is_host = false;
    m_impl->local_slot_val = -1;
    m_impl->connected = false;

    std::lock_guard<std::mutex> lock(m_impl->state_mutex);
    m_impl->slot_active.fill(false);
    m_impl->remote_states.fill(PlayerState{});
}

bool LANSession::is_connected() const {
    if (m_impl->host == nullptr) return false;
    if (m_impl->is_host) return true;
    return m_impl->peer != nullptr && m_impl->connected;
}

bool LANSession::is_host() const {
    return m_impl->is_host;
}

int LANSession::local_slot() const {
    return m_impl->local_slot_val;
}

int LANSession::peer_count() const {
    if (!m_impl->host) return 0;
    int count = 0;
    for (int i = 0; i < 4; ++i) {
        if (m_impl->slot_active[i]) ++count;
    }
    return count;
}

void LANSession::capture(const uint8_t* rdram) {
    if (!is_connected() || rdram == nullptr) return;

    // Read our own state from RDRAM.
    int slot = m_impl->local_slot_val;
    if (slot < 0) slot = 0;
    const PlayerState state = read_player_state(rdram, slot);

    // Broadcast to all connected peers.
    const NetworkPacket pkt = state_to_packet(state);
    ENetPacket* packet = enet_packet_create(
        &pkt, sizeof(pkt),
        ENET_PACKET_FLAG_RELIABLE);

    if (m_impl->is_host) {
        // Host sends to all connected peers.
        enet_host_broadcast(m_impl->host, 0, packet);
    } else if (m_impl->peer != nullptr) {
        enet_peer_send(m_impl->peer, 0, packet);
    }
    // enet_host_flush sends immediately.
    enet_host_flush(m_impl->host);
}

int LANSession::poll() {
    if (m_impl->host == nullptr) return 0;

    int events = 0;
    ENetEvent event;

    // Non-blocking poll: process all pending events immediately.
    // A small timeout (1ms) allows ENet to process connection establishment
    // packets that arrive via internal timeouts -- a pure 0 would skip them.
    while (enet_host_service(m_impl->host, &event, 1) > 0) {
        ++events;

        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            m_impl->connected = true;
            // Assign the new peer the next available slot.
            std::lock_guard<std::mutex> lock(m_impl->state_mutex);
            int assigned = -1;
            for (int i = 0; i < m_impl->max_players; ++i) {
                if (!m_impl->slot_active[i]) {
                    // Slot 0 is reserved for the host.
                    if (i == 0 && m_impl->is_host) continue;
                    assigned = i;
                    m_impl->slot_active[i] = true;
                    break;
                }
            }
            event.peer->data = reinterpret_cast<void*>(static_cast<intptr_t>(assigned));

            if (assigned >= 0 && on_player_joined) {
                PlayerState ps;
                ps.slot = assigned;
                on_player_joined(assigned, ps);
            }
            std::fprintf(stderr, "[lan] client connected, assigned slot %d\n", assigned);
            break;
        }

        case ENET_EVENT_TYPE_RECEIVE: {
            if (event.packet->dataLength < sizeof(NetworkPacket)) {
                enet_packet_destroy(event.packet);
                break;
            }

            const NetworkPacket* pkt =
                reinterpret_cast<const NetworkPacket*>(event.packet->data);
            const PlayerState state = packet_to_state(*pkt);

            {
                std::lock_guard<std::mutex> lock(m_impl->state_mutex);
                if (state.slot >= 0 && state.slot < 4) {
                    m_impl->remote_states[state.slot] = state;
                    m_impl->slot_active[state.slot] = true;
                }
            }

            if (on_state_received) {
                on_state_received(state.slot, state);
            }

            enet_packet_destroy(event.packet);
            break;
        }

        case ENET_EVENT_TYPE_DISCONNECT: {
            int slot = static_cast<int>(reinterpret_cast<intptr_t>(event.peer->data));
            event.peer->data = nullptr;

            std::lock_guard<std::mutex> lock(m_impl->state_mutex);
            if (slot >= 0 && slot < 4) {
                m_impl->slot_active[slot] = false;
                m_impl->remote_states[slot] = PlayerState{};
            }

            if (on_player_left) {
                on_player_left(slot);
            }
            std::fprintf(stderr, "[lan] client in slot %d disconnected\n", slot);
            break;
        }

        default:
            break;
        }
    }

    if (events > 0) {
        enet_host_flush(m_impl->host);
    }

    return events;
}

PlayerState LANSession::remote_state(int slot) const {
    std::lock_guard<std::mutex> lock(m_impl->state_mutex);
    if (slot < 0 || slot >= 4) return PlayerState{};
    return m_impl->remote_states[slot];
}

}  // namespace wr64
