#pragma once

#include <stdint.h>

/**
 * PSXSplash serial network protocol - wire definitions.
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
 * simply fans out to N slots - the PlayStation code is identical either way.
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
 * AppData - the game's own reliable channel.
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
 * Payload budget is c_maxEventPayload (256B) - it shares the reliable channel's
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
    Pong = 10,        // reply to Ping, echoing its payload - see PingPayload
};

// PacketHeader.flags bits.
//
// Hello only: "I already have a slot - I have merely changed scene, so keep it."
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

/**
 * Snapshot payload formats.
 *
 * Header is always [u8 avatarCount][u8 objectCount][u8 format][u8 posShift].
 * The third byte used to be reserved, which is what makes this extension free.
 *
 *   Legacy (0)  20-byte records, absolute fp12 positions. Exactly the bytes this
 *               protocol has always emitted, so an old peer is unaffected.
 *
 *   Compact (1) The header is followed by an absolute fp12 ORIGIN
 *               [i32 x][i32 y][i32 z], and every record stores its position as a
 *               16-bit unsigned offset from that origin, shifted right by
 *               posShift. Decode is `origin + (offset << posShift)`.
 *
 * WHY SELF-DESCRIBING RATHER THAN NEGOTIATED. The obvious design is to agree a
 * precision profile at handshake time. This deliberately does not, because the
 * origin and shift travel WITH the data they describe, so there is no state that
 * can be missed, lost or arrive stale - and a negotiated profile is exactly the
 * kind of thing that silently fails on a link whose handshake extras get dropped.
 * This session has already watched a Ping/Pong exchange never complete on a
 * saturated console; a format that depended on a similar exchange would have
 * decoded every position wrongly instead of merely failing to measure.
 *
 * The sender picks posShift from the actual spread of the positions it is about
 * to send: the smallest shift for which every offset fits in 16 bits. Nothing is
 * assumed about world size. If even a shift of 15 cannot cover the spread, the
 * sender falls back to Legacy rather than emitting something lossy enough to be
 * wrong, so the failure mode is "bigger packet", never "teleporting player".
 *
 * Precision cost is 2^posShift in fp12 raw units. A room spanning 512 world units
 * needs shift 5, i.e. 1/128 of a world unit - far below what a 320x240 screen can
 * show, and these are remote positions that are interpolated on arrival anyway.
 *
 * Saving at ten players: 4 + 12 + 10*10 = 116 bytes against 4 + 20*10 = 204. That
 * is 43% off the largest and most frequent message on the wire, and on a real
 * console it is also 43% less pressure on an 8-byte hardware RX FIFO that has been
 * measured overrunning.
 */
enum : uint8_t {
    c_snapFormatLegacy = 0,
    c_snapFormatCompact = 1,
};

/// Bytes of origin that follow the header in the compact format.
constexpr uint32_t c_snapOriginBytes = 12;
/// Compact record sizes. Avatars are keyed by a 1-byte slot, objects by a 2-byte
/// actorId, so the two differ by exactly that byte.
constexpr uint32_t c_snapCompactAvatarSize = 10;
constexpr uint32_t c_snapCompactObjectSize = 11;
/// Legacy record size, both kinds.
constexpr uint32_t c_snapLegacyRecordSize = 20;

/// One full turn in 4.12 fixed point: round(2*pi * 4096).
///
/// Yaw is normalised into [0, c_fp12TwoPi) before it is quantised, which is what
/// makes 16 bits safe. Clamping instead would be wrong for an angle - a game that
/// lets rotation accumulate would have its players stick at the clamp - and
/// truncating to 16 bits would wrap at 1.27 turns, which is not a rotation.
constexpr int32_t c_fp12TwoPi = 25736;

/// Normalise an fp12 angle into [0, c_fp12TwoPi). Exact, and 32-bit only: there
/// is no 64-bit divide on this target.
constexpr int32_t normalizeFp12Angle(int32_t yaw) {
    int32_t a = yaw % c_fp12TwoPi;
    if (a < 0) a += c_fp12TwoPi;
    return a;
}

/// Largest offset a 16-bit field can carry, and the largest shift worth trying.
constexpr uint32_t c_snapCompactMaxOffset = 0xFFFF;
constexpr uint32_t c_snapCompactMaxShift = 15;

/// Smallest shift for which `spread >> shift` fits in 16 bits, or -1 if none.
///
/// -1 means the positions are too far apart to compact at any precision worth
/// having, and the sender must emit Legacy instead. That fallback is the reason
/// this format is safe to enable unconditionally: the worst case is a bigger
/// packet, never a wrong position.
///
/// MUST match choose_compact_shift() in server/psnl_server/rooms.py. The two
/// implementations quantise the same numbers on opposite sides of the wire, and a
/// disagreement would not fail loudly - it would place every remote player
/// somewhere slightly wrong, which is indistinguishable from a physics bug.
constexpr int32_t chooseCompactShift(uint32_t spread) {
    for (uint32_t s = 0; s <= c_snapCompactMaxShift; ++s) {
        if ((spread >> s) <= c_snapCompactMaxOffset) return static_cast<int32_t>(s);
    }
    return -1;
}

/// Quantise an absolute fp12 coordinate to its 16-bit offset, and back.
/// `decodeCompactOffset(encodeCompactOffset(v)) <= v`, with error < (1 << shift).
constexpr uint16_t encodeCompactOffset(int32_t value, int32_t origin, uint32_t shift) {
    return static_cast<uint16_t>(static_cast<uint32_t>(value - origin) >> shift);
}
constexpr int32_t decodeCompactOffset(uint16_t offset, int32_t origin, uint32_t shift) {
    return origin + (static_cast<int32_t>(offset) << shift);
}

/**
 * Ping/Pong: the link's only source of ground truth about itself.
 *
 * Ping used to be a bare keepalive whose arrival was the entire signal ("liveness
 * only; presence of a valid frame is the signal"). That left the stack with no
 * measurement of any kind, so every rate, timeout and threshold in it was a
 * constant derived from an ASSUMPTION about the link - and when an assumption was
 * wrong the failure was invisible, because each layer still behaved correctly. Two
 * separate week-long hunts ended at exactly that: a receive path capped at 480 B/s
 * and a transmit path whose real cost nobody had measured.
 *
 * So Ping now carries measurements and the peer must echo it back as a Pong. The
 * responder copies `echo` verbatim and fills in its OWN goodput and rtt, so a
 * single exchange tells each end what both ends are seeing.
 *
 * These are reports, not commands: a peer is free to ignore them. But a peer that
 * paces itself by a number the other end actually measured cannot be defeated by
 * being wrong about the wire, which is the property the previous design lacked.
 */
struct PingPayload {
    uint32_t echo;      // opaque token; the responder returns it unmodified
    uint32_t goodput;   // sender's measured outbound bytes per second
    uint16_t rttMillis; // sender's smoothed round trip, 0 when not yet known
    uint16_t reserved;
};
static_assert(sizeof(PingPayload) == 12, "PingPayload must be 12 bytes");

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
