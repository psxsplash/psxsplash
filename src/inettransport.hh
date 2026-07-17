#pragma once

#include <stdint.h>

namespace psxsplash {

/**
 * Abstract, non-blocking byte-stream transport.
 *
 * Everything above the hardware driver — framing, session, object replication —
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
    virtual uint32_t write(const uint8_t* src, uint32_t len) = 0;

  protected:
    // Transports are long-lived singletons, never deleted through this base,
    // so the destructor is non-virtual and protected (avoids pulling in
    // operator delete under -nostdlib).
    ~INetTransport() = default;
};

}  // namespace psxsplash
