#pragma once

#include <stdint.h>

#include "inettransport.hh"
#include "ringbuffer.hh"

namespace psxsplash {

/// The PCDRV calls a socket needs and a clock, behind pointers so this file stays
/// hardware-free: the engine passes the real break calls (networkmanager.cpp),
/// the host tests pass fakes.
struct PcdrvNetOps {
    int (*open)(const char* name);  // PCopen(name, 0, 0): fd, or < 0
    int (*close)(int fd);
    int (*read)(int fd, void* buf, int len);         // never blocks: 0 = nothing yet, < 0 = gone
    int (*write)(int fd, const void* buf, int len);  // blocks until written (host timeout 5 s)
    uint16_t (*now)();  // scanlines, and it must run during a call; nullptr: every poll runs
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
 * Both rings are drained in poll(): one PCread loop until the host has nothing
 * more, then PCwrite of everything queued. A failed call closes the socket;
 * isOpen() then reports false and the session above treats it as a lost link.
 *
 * WHY poll() PACES ITSELF. A PCDRV call stops the console until the host has
 * answered, and over a monitor's serial link that is a long time: on an SCPH-5501
 * through nugget's monitor an empty PCread took 25.7 ms and a PCwrite of a frame
 * 31.5 ms at 115200 baud, 6.9 and 8.2 ms at 529200, against a 16.7 ms frame. Two
 * calls a frame took a 60 fps scene to 15.3 fps at 115200. So poll() times its own
 * calls and compares the frame that carried them with one that did not. If the
 * game lost nothing the next frame gets a turn too: a game with time to spare, or
 * a fast host, is polled every frame. If the game lost time, the socket rests for
 * c_restPerBusy times what it lost, so it costs at most a quarter of the console's
 * time however slow the link is. There is nothing to configure. A slow
 * host gets one call at a time, in turn, so that a frame carries one stall and not
 * both. Outgoing data waits in the ring meanwhile, where txSpace() lets the
 * droppable data lose out. Incoming data can't be left to wait: psnl_server sends
 * one console 1,068 bytes a second (30 snapshots of 35), and at 256 bytes a read an
 * SCPH-5501 at 115200 was 50 KB behind after 112 s. So a read asks for c_chunk, a
 * server that is ahead shortens the rest, and the two together keep up.
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
    /// The most one PCread or PCwrite moves. A call moves only what there is, and its
    /// round trip is the dear part (25.7 ms at 115200 before the first byte), so a
    /// large one costs nothing when there is little. A member: the stack can't spare it.
    static constexpr uint32_t c_chunk = 1024;
    /// Rest this many times what the game lost: at most 1/(1 + 3) of the time.
    static constexpr uint32_t c_restPerBusy = 3;
    /// Calls that took longer than this (2 ms, in scanlines) mean a slow host.
    static constexpr uint16_t c_slowLines = 32;
    /// The longest rest, half a second in scanlines. NetLink pings once a second,
    /// so a stalling host still gets a read and a write across that.
    static constexpr uint32_t c_maxRestLines = 7864;
    /// One video frame in scanlines: the most a turn can push a frame past its deadline.
    static constexpr uint16_t c_videoFrameLines = 263;
    /// A frame with a turn this much (1 ms) over a quiet one has cost nothing.
    static constexpr uint16_t c_marginLines = 16;
    /// Two video frames, in scanlines.
    static constexpr uint16_t c_firstRestLines = 526;

    uint16_t now() const { return m_ops.now ? m_ops.now() : 0; }
    bool pollRx(bool drain);
    bool pollTx(bool drain);

    PcdrvNetOps m_ops;
    uint16_t m_turnEnd = 0;
    uint16_t m_rest = 0;
    uint16_t m_lastPoll = 0;
    uint16_t m_spent = 0;       // what the last turn's calls took
    uint16_t m_quietFrame = 0;  // the shortest frame since the last turn with no calls in it
    bool m_quietFresh = false;
    bool m_behind = false;      // the last read came back full: the server is ahead
    uint8_t m_chunk[c_chunk];
    bool m_turned = false;
    bool m_slow = false;
    bool m_writeTurn = false;
    int m_fd = -1;
    RingBuffer<c_rxRingSize> m_rx;
    RingBuffer<c_txRingSize> m_tx;
    uint32_t m_bytesRx = 0;
    uint32_t m_bytesTx = 0;
    char m_failedCall = 0;
    int m_failedResult = 0;
};

}  // namespace psxsplash
