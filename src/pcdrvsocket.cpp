#include "pcdrvsocket.hh"

namespace psxsplash {

namespace {
// Chunk size for one PCread / PCwrite. Each call is a round trip to the host, so
// fewer, larger calls are cheaper; the stack is not.
constexpr uint32_t c_chunk = 256;
}  // namespace

bool PcdrvSocket::open(const char* server) {
    if (m_fd >= 0) return true;
    if (!server) return false;

    // "tcp://" + host:port, refused rather than truncated if it does not fit:
    // a truncated port is a different server.
    char name[6 + c_maxServerLen + 1] = {'t', 'c', 'p', ':', '/', '/'};
    uint32_t n = 0;
    while (server[n]) {
        if (n == c_maxServerLen) return false;
        name[6 + n] = server[n];
        n++;
    }
    if (n == 0) return false;
    name[6 + n] = 0;

    // The probe first: a host without networking may treat "tcp://..." as a
    // file name, and the probe is the contract's way of asking without that risk.
    const int probe = m_ops.open("probe:net");
    if (probe < 0) return false;
    m_ops.close(probe);

    const int fd = m_ops.open(name);
    if (fd < 0) return false;
    m_fd = fd;
    m_rx.clear();
    m_tx.clear();
    return true;
}

void PcdrvSocket::close() {
    if (m_fd >= 0) m_ops.close(m_fd);
    m_fd = -1;
    // Bytes queued for a socket that is gone must not reach the next one: they
    // belong to the old session and would be parsed as the new one's.
    m_rx.clear();
    m_tx.clear();
}

void PcdrvSocket::poll() {
    if (m_fd < 0) return;
    uint8_t chunk[c_chunk];

    // RX: take what has arrived, without ever waiting for more. A short read
    // means the host has nothing further right now.
    while (m_rx.space() > 0) {
        const uint32_t want = m_rx.space() < c_chunk ? m_rx.space() : c_chunk;
        const int got = m_ops.read(m_fd, chunk, static_cast<int>(want));
        if (got < 0) {  // EOF or peer gone
            m_failedCall = 'r';
            m_failedResult = got;
            close();
            return;
        }
        for (int i = 0; i < got; i++) m_rx.push(chunk[i]);
        m_bytesRx += static_cast<uint32_t>(got);
        if (static_cast<uint32_t>(got) < want) break;
    }

    // TX: PCwrite blocks until written, so everything queued leaves now. A short
    // or failed write has lost part of a frame and the stream cannot recover.
    while (!m_tx.empty()) {
        uint32_t len = 0;
        while (len < c_chunk && m_tx.pop(chunk[len])) len++;
        const int put = m_ops.write(m_fd, chunk, static_cast<int>(len));
        if (put != static_cast<int>(len)) {
            m_failedCall = 'w';
            m_failedResult = put;
            close();
            return;
        }
        m_bytesTx += len;
    }
}

uint32_t PcdrvSocket::read(uint8_t* dst, uint32_t max) {
    uint32_t n = 0;
    while (n < max && m_rx.pop(dst[n])) n++;
    return n;
}

uint32_t PcdrvSocket::write(const uint8_t* src, uint32_t len) {
    if (m_fd < 0) return 0;
    uint32_t n = 0;
    while (n < len && m_tx.push(src[n])) n++;
    return n;
}

bool PcdrvSocket::writeAll(const uint8_t* src, uint32_t len) {
    // All or nothing, for the reason given on INetTransport::writeAll.
    if (m_fd < 0 || m_tx.space() < len) return false;
    for (uint32_t i = 0; i < len; i++) m_tx.push(src[i]);
    return true;
}

}  // namespace psxsplash
