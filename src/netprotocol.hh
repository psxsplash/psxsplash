#pragma once

#include <stdint.h>

/**
 * PSXSplash serial network protocol — wire definitions.
 *
 * This header is the single source of truth for the on-wire format. It is pure
 * data + a CRC helper (no engine or hardware dependencies) so the exact same
 * definitions can be mirrored by a future PC-side central server. Everything is
 * fixed-layout little-endian with static_assert size contracts, matching the
 * project's existing "hand-rolled binary + static_assert" convention.
 *
 * Framing on the wire, per packet:
 *   [ sync=0xA5 ][ PacketHeader (8 bytes) ][ payload (length bytes) ][ crc16 ]
 * The CRC covers the header + payload (not the sync byte). A corrupt or
 * mis-synchronised frame fails the CRC and the receiver resynchronises by
 * hunting for the next sync byte.
 *
 * The protocol is slot-addressed from day one: every packet carries the
 * sender's slot id. In a two-console link the slots are {0,1}; a PC server
 * simply fans out to N slots — the PlayStation code is identical either way.
 */
namespace psxsplash::net {

// Wire protocol version. Bump on any incompatible framing/payload change.
constexpr uint16_t c_protoVersion = 1;

// Protocol magic, bytes 'P','S','N','L' (little-endian u32). Used in the
// handshake to reject foreign/garbage data.
constexpr uint32_t c_magic = 0x4C4E5350u;

// Frame delimiter used to (re)synchronise the byte stream.
constexpr uint8_t c_syncByte = 0xA5;

// Slot ids. Slot 0 is always the host/authority. 0xFF = not yet assigned.
constexpr uint8_t c_hostSlot = 0;
constexpr uint8_t c_noSlot = 0xFF;

// Maximum payload carried by a single frame.
constexpr uint16_t c_maxPayload = 512;

// Maximum payload on the reliable channel (Event / ObjectState / AppData).
// Smaller than c_maxPayload because every reliable packet is buffered whole in
// the retransmit queue, so this multiplies by c_eventQueueSize in RAM.
constexpr uint16_t c_maxEventPayload = 256;

/**
 * AppData — the game's own reliable channel.
 *
 * Everything else in this enum is engine machinery with fixed meaning. AppData
 * is the opposite: an opaque blob the engine never interprets, delivered
 * reliably and in order, surfaced to Lua as a binary string via Net.SendData()
 * and the scene's onNetData(). Lobbies, room lists, roles, votes and task state
 * are all built on top of it *by the game*.
 *
 *
 * It is also the only way to move strings: Net.Send() carries a fixed
 * (int32 id, int32 arg), which cannot express a room name.
 *
 * Payload budget is c_maxEventPayload (256B) — it shares the reliable channel's
 * queue and stop-and-wait sequencing with Event and ObjectState.
 */
enum class PacketType : uint8_t {
    Hello = 1,     // handshake: proto version + scene hash + host tiebreak
    HelloAck = 2,  // handshake reply: assigns slots + confirms host
    Snapshot = 3,  // unreliable object-state snapshot (self-healing)
    Event = 4,     // reliable discrete event (stop-and-wait acked)
    EventAck = 5,  // acknowledgement of an Event's / ObjectState's seq
    Ping = 6,      // keepalive / liveness
    Bye = 7,       // graceful disconnect
    ObjectState = 8,  // reliable per-object Lua state ([actorId:u16][serialized self.sync])
    AppData = 9,      // reliable opaque application payload (see below)
};

// PacketHeader.flags bits.
//
// Hello only: "I already have a slot — I have merely changed scene, so keep it."
// Sent by a console that called Net.SetPersistent(true) before loading a new
// scene. Without this a lobby -> game transition looks like a brand new player
// and the server would hand out a different slot (or refuse, the room being
// full of the very players who are trying to move).
constexpr uint8_t c_flagRebind = 0x01;

// On-wire frame header, immediately after the sync byte. Fixed 8 bytes, no
// implicit padding (the u16 fields are naturally aligned at offsets 4 and 6),
// so it round-trips byte-for-byte via memcpy.
struct PacketHeader {
    uint8_t type;      // PacketType
    uint8_t srcSlot;   // sender slot (c_noSlot during handshake)
    uint8_t flags;     // reserved for channel flags
    uint8_t reserved;
    uint16_t seq;      // sequence / generation number (per packet type)
    uint16_t length;   // payload byte count
};
static_assert(sizeof(PacketHeader) == 8, "PacketHeader must be 8 bytes");

// Handshake: sent by both ends on connect (symmetric). The higher `tiebreak`
// becomes the host in a peer-to-peer link; a server ignores it and assigns.
struct HelloPayload {
    uint32_t magic;         // c_magic
    uint16_t protoVersion;  // c_protoVersion
    uint16_t reserved;
    uint32_t sceneHash;     // hash of the loaded splashpack (identity agreement)
    uint32_t tiebreak;      // random; higher wins the host role
};
static_assert(sizeof(HelloPayload) == 16, "HelloPayload must be 16 bytes");

// Handshake reply from the (elected/authoritative) host.
struct HelloAckPayload {
    uint32_t magic;         // c_magic
    uint16_t protoVersion;  // c_protoVersion
    uint8_t yourSlot;       // slot assigned to the receiver of this ack
    uint8_t hostSlot;       // slot holding authority (normally c_hostSlot)
    uint32_t sceneHash;     // echoed for cross-check
    uint8_t playerCount;    // number of connected slots
    uint8_t reserved[3];
};
static_assert(sizeof(HelloAckPayload) == 16, "HelloAckPayload must be 16 bytes");

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). Table-free; chainable across
// buffers by passing the running value back in as `crc`.
inline uint16_t crc16(const uint8_t* data, uint32_t len, uint16_t crc = 0xFFFF) {
    for (uint32_t i = 0; i < len; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

}  // namespace psxsplash::net
