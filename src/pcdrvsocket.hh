#pragma once

#include <stdint.h>

#include "inettransport.hh"
#include "ringbuffer.hh"

namespace psxsplash {

/// The four PCDRV calls a socket needs, behind pointers so this file stays
/// hardware-free: the engine passes the real break calls (networkmanager.cpp),
/// the host tests pass fakes.
struct PcdrvNetOps {
    int (*open)(const char* name);  // PCopen(name, 0, 0): fd, or < 0
    int (*close)(int fd);
    int (*read)(int fd, void* buf, int len);         // never blocks: 0 = nothing yet, < 0 = gone
    int (*write)(int fd, const void* buf, int len);  // blocks until written (host timeout 5 s)
};

/**
 * A TCP stream opened through the PCDRV socket contract, as an INetTransport.
 *
 * psxmon (on hardware) and PCSX-Redux (`-pcdrv -pcdrvnet`) both answer the same
 * guest calls:
 *   - PCopen("probe:net") succeeds only when host networking is enabled;
 *   - PCopen("tcp://host:port") connects (waiting up to 5 s) and returns an fd in
 *     the same space as files;
 *   - PCread never blocks: it returns what has arrived, 0 for nothing yet, and an
 *     error once the peer is gone;
 *   - PCwrite blocks until everything is written (5 s timeout).
 *
 * The bytes on the socket are exactly the bytes that would cross SIO1, so NetLink
 * and the PSNL frames above it do not change. A PC server reads them on its raw
 * port (psnl_server: 6699), the one the serial bridge uses.
 *
 * Both rings are drained in poll(), once a frame: one PCread loop until the host
 * has nothing more, then PCwrite of everything queued. A failed call closes the
 * socket; isOpen() then reports false and the session above treats it as a lost
 * link.
 */
class PcdrvSocket final : public INetTransport {
  public:
    /// Used when the game names no server: psnl_server's raw port on this host.
    static constexpr const char* c_defaultServer = "127.0.0.1:6699";
    /// Longest "host:port" accepted (the name passed to PCopen adds "tcp://").
    static constexpr uint32_t c_maxServerLen = 57;

    explicit PcdrvSocket(const PcdrvNetOps& ops) : m_ops(ops) {}

    /// Probe for host networking and connect to `server` ("host:port"). Returns
    /// false, leaving the socket closed, if the host has no networking, refuses
    /// the name or cannot connect. Already open: returns true without reconnecting,
    /// so a scene change keeps the server-side session.
    bool open(const char* server);
    void close();
    bool isOpen() const { return m_fd >= 0; }

    // --- INetTransport ---
    void poll() override;
    uint32_t available() const override { return m_rx.size(); }
    uint32_t read(uint8_t* dst, uint32_t max) override;
    uint32_t write(const uint8_t* src, uint32_t len) override;
    bool writeAll(const uint8_t* src, uint32_t len) override;
    uint8_t txCongestion() const override {
        return static_cast<uint8_t>((m_tx.size() * 255u) / m_tx.capacity());
    }
    uint32_t txSpace() const override { return m_tx.space(); }
    uint32_t txCapacity() const override { return m_tx.capacity(); }

    uint32_t bytesReceived() const { return m_bytesRx; }
    uint32_t bytesSent() const { return m_bytesTx; }

    /// Why the socket last closed itself: the call ('r' PCread, 'w' PCwrite) and
    /// what it returned. 0 / 0 until a call has failed.
    char lastFailedCall() const { return m_failedCall; }
    int lastFailedResult() const { return m_failedResult; }

  private:
    // Same sizes as Sio1, for the same reasons; see c_rxRingSize there.
    static constexpr uint32_t c_rxRingSize = 4096;
    static constexpr uint32_t c_txRingSize = 1024;

    PcdrvNetOps m_ops;
    int m_fd = -1;
    RingBuffer<c_rxRingSize> m_rx;
    RingBuffer<c_txRingSize> m_tx;
    uint32_t m_bytesRx = 0;
    uint32_t m_bytesTx = 0;
    char m_failedCall = 0;
    int m_failedResult = 0;
};

}  // namespace psxsplash
