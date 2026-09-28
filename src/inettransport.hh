#pragma once

#include <stdint.h>

namespace psxsplash {

/**
 * Abstract, non-blocking byte-stream transport.
 *
 * Everything above the hardware driver - framing, session, object replication -
 * only ever sees this interface. That is the seam that lets the exact same
 * networking code run whether the bytes travel to a peer PlayStation over a link
 * cable or to a PC hosting a central server: both are just bytes on the serial
 * port. The current implementation is Sio1; a loopback/mock transport can
 * implement the same interface for self-testing.
 *
 * No method ever blocks. `poll()` services the hardware; the read/write calls
 * move data to/from internal buffers and report how much they actually moved.
 */
class INetTransport {
  public:
    // Service the underlying hardware once: move received bytes into the RX
    // buffer and push queued bytes out of the TX buffer. Call every frame
    // (or from an interrupt handler).
    virtual void poll() = 0;

    // Number of bytes available to read right now.
    virtual uint32_t available() const = 0;

    // Copy up to `max` received bytes into `dst`. Returns the count copied
    // (0..max), never more than are available.
    virtual uint32_t read(uint8_t* dst, uint32_t max) = 0;

    // Queue up to `len` bytes for transmission. Returns the count actually
    // queued, which may be less than `len` if the outgoing buffer is full.
    // Never blocks.
    //
    // Prefer writeAll() for anything framed - see below.
    virtual uint32_t write(const uint8_t* src, uint32_t len) = 0;

    // Queue `len` bytes or none at all. Returns false if they did not fit.
    //
    // Pure virtual on purpose: a default implementation in terms of write()
    // could not be atomic, and silently inheriting a non-atomic "atomic" write
    // is exactly the bug this exists to prevent. A partial write of a
    // length-prefixed frame corrupts the frame AFTER it too, because the peer
    // reads a length that runs past the truncation and swallows the next frame's
    // opening bytes as payload. Every transport must answer this deliberately.
    virtual bool writeAll(const uint8_t* src, uint32_t len) = 0;

    // How congested the outgoing side is, 0..255 (0 = idle, 255 = full).
    //
    // Pure virtual, like writeAll, and for the same kind of reason: a transport
    // that cannot answer this honestly will be asked to carry more than it can,
    // and the caller has no other way to find out. A default of "never congested"
    // is a lie that costs a whole game.
    //
    // The failure it prevents: a transport whose real throughput is far below what
    // the sender assumes. Position snapshots offered at a fixed cadence into a ring
    // that drains slower will fill it, and acknowledgements then queue behind
    // minutes of stale positions until the peer abandons the message it is
    // retransmitting. Every layer behaves correctly and the game does not work.
    //
    // So anything DROPPABLE must consult this before queueing. Then, when the wire
    // is slower than the sender expects - for any reason, including ones nobody has
    // anticipated - what gets dropped is the data designed to be dropped.
    virtual uint8_t txCongestion() const = 0;

    // Bytes writeAll() would accept right now.
    //
    // txCongestion() above is a RATIO, which is enough to answer "is the wire
    // busy" and useless for answering "will this specific frame fit, and will
    // anything fit after it". Reserving room requires the second question, and
    // reserving room turns out to be the difference between a link that can be
    // measured and one that cannot.
    //
    // The case that needs it: a 30Hz snapshot cadence offering ~56 bytes per frame
    // into a drain of 48 keeps the ring permanently full. Position data wins the
    // race for space simply by being generated constantly, so control frames are
    // refused, no round trip is ever measured, and the link cannot report on itself
    // precisely when that would be most useful.
    //
    // Priority ordering alone does not fix it. Sending control traffic first is
    // worthless if droppable data has already taken every byte by the time the
    // control frame is built. Room has to be RESERVED, which requires knowing how
    // much there is.
    virtual uint32_t txSpace() const = 0;

    // Total capacity of the outgoing buffer. With txSpace() this gives OCCUPANCY,
    // which is the quantity that actually governs latency: a queue holding N bytes
    // delays everything behind it by N/rate, whatever N is made of.
    //
    // That is why a reserve is not sufficient on its own. Keeping room free
    // guarantees a control frame can be ACCEPTED; it does nothing about how long
    // it then waits. A 1024-byte ring draining at ~900 B/s
    // is 1.1 SECONDS of head-of-line delay, so an acknowledgement queued behind a
    // full ring of position updates arrives long after the sender has given up on
    // the message it acknowledges.
    virtual uint32_t txCapacity() const = 0;

  protected:
    // Transports are long-lived singletons, never deleted through this base,
    // so the destructor is non-virtual and protected (avoids pulling in
    // operator delete under -nostdlib).
    ~INetTransport() = default;
};

}  // namespace psxsplash
