#include "pcdrvsocket.hh"

namespace psxsplash {

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
    // No calls for the first frame or two, so that a quiet frame has been seen
    // before the first turn is judged against one.
    if (m_ops.now) {
        m_lastPoll = m_turnEnd = now();
        m_rest = c_firstRestLines;
        m_turned = false;
        m_quietFresh = true;
    }
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
    const uint16_t start = now();
    const uint16_t frame = static_cast<uint16_t>(start - m_lastPoll);
    m_lastPoll = start;

    // THE PACING RULE (why: "WHY poll() PACES ITSELF" in the header). `frame` is the
    // time since the last poll, one frame of the game.
    //   - A frame with no calls in it is quiet. Keep the shortest since the last turn.
    //   - A frame that carried a turn and came out no longer than a quiet one, within
    //     c_marginLines, cost the game nothing: take another turn now.
    //   - A frame that came out longer lost the game (frame - quiet) lines. Rest for
    //     c_restPerBusy times that, so the socket has at most a quarter of the time.
    //     It is charged on what the game lost and not on the calls: a 7 ms call that
    //     pushes a frame past the video's deadline costs the whole of the next frame.
    //   - A turn can't cost more than its calls and that one video frame, so a longer
    //     loss is the scene's own and is not charged. A scene that stalls does not
    //     make the socket go quiet.
    //   - A read that came back full means the server is ahead of the socket. Until
    //     it has caught up the rest is what was lost and no more, so the socket can
    //     have half the time, and however much the server sends the game has the rest.
    //   - No rest is longer than c_maxRestLines, for a host that stalls: the session
    //     gets a turn at least every half second past the end of a frame.
    //   - A fast host gets both directions in a turn. A slow one gets one call,
    //     reads and writes taking turns, so that a frame carries one stall and not
    //     both. With nothing to send, every turn is a read.
    if (m_turned) {
        m_turned = false;
        m_quietFresh = true;
        m_rest = 0;
        if (frame > m_quietFrame + c_marginLines) {
            const uint32_t lost = static_cast<uint32_t>(frame - m_quietFrame);
            const uint32_t most = static_cast<uint32_t>(m_spent) + c_videoFrameLines;
            const uint32_t rest = (lost < most ? lost : most) * (m_behind ? 1 : c_restPerBusy);
            m_rest = static_cast<uint16_t>(rest < c_maxRestLines ? rest : c_maxRestLines);
            m_turnEnd = start;
        }
    } else {
        m_quietFrame = (m_quietFresh || frame < m_quietFrame) ? frame : m_quietFrame;
        m_quietFresh = false;
    }
    if (static_cast<uint16_t>(start - m_turnEnd) < m_rest) return;

    const bool both = !m_slow;
    const bool writing = !both && m_writeTurn && !m_tx.empty();
    if ((both || !writing) && !pollRx(both)) return;
    if ((both || writing) && !pollTx(both)) return;
    m_writeTurn = !writing;

    m_spent = static_cast<uint16_t>(now() - start);
    m_slow = m_spent > c_slowLines;
    m_turned = true;
}

bool PcdrvSocket::pollRx(bool drain) {
    // RX: take what has arrived, without ever waiting for more. A short read
    // means the host has nothing further right now.
    while (m_rx.space() > 0) {
        const uint32_t want = m_rx.space() < c_chunk ? m_rx.space() : c_chunk;
        const int got = m_ops.read(m_fd, m_chunk, static_cast<int>(want));
        if (got < 0) {  // EOF or peer gone
            m_failedCall = 'r';
            m_failedResult = got;
            close();
            return false;
        }
        for (int i = 0; i < got; i++) m_rx.push(m_chunk[i]);
        m_bytesRx += static_cast<uint32_t>(got);
        m_behind = static_cast<uint32_t>(got) == want;
        if (!m_behind || !drain) break;
    }
    return true;
}

bool PcdrvSocket::pollTx(bool drain) {
    // TX: PCwrite blocks until written. A short or failed write has lost part of
    // a frame and the stream cannot recover.
    while (!m_tx.empty()) {
        uint32_t len = 0;
        while (len < c_chunk && m_tx.pop(m_chunk[len])) len++;
        const int put = m_ops.write(m_fd, m_chunk, static_cast<int>(len));
        if (put != static_cast<int>(len)) {
            m_failedCall = 'w';
            m_failedResult = put;
            close();
            return false;
        }
        m_bytesTx += len;
        if (!drain) break;
    }
    return true;
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
