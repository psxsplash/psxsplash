// Host tests for PcdrvSocket (src/pcdrvsocket.cpp), compiled natively.
//
// The PCDRV host is faked here with the contract psxmon and PCSX-Redux share:
// "probe:net" opens only with networking on, "tcp://host:port" opens a stream,
// PCread never blocks (0 = nothing yet, < 0 = peer gone), PCwrite writes all.

#include <string.h>

#include "pcdrvsocket.hh"
#include "mocktransport.hh"
#include "netlink.hh"
#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::net;
using namespace psxsplash::test;

namespace {

// Two stream fds whose ends are crossed, so two sockets form one connection.
constexpr int c_fdA = 3, c_fdB = 4, c_fdProbe = 7;

struct FakeHost {
    bool netEnabled = true;
    bool connectOk = true;
    int opens = 0, probeCloses = 0, closes = 0;
    char lastName[96] = {};
    int nextStream = c_fdA;
    // in[i] holds the bytes waiting to be read on stream fd c_fdA + i.
    RingBuffer<8192> in[2];
    int readLimit = 1 << 30;  // most bytes one PCread returns
    bool readFails = false;
    int writeShort = -1;  // >= 0: the next PCwrite writes only this many
};
FakeHost g;

int fakeOpen(const char* name) {
    g.opens++;
    strncpy(g.lastName, name, sizeof(g.lastName) - 1);
    if (strcmp(name, "probe:net") == 0) return g.netEnabled ? c_fdProbe : -1;
    if (!g.netEnabled || strncmp(name, "tcp://", 6) != 0 || !g.connectOk) return -1;
    return g.nextStream++;
}
int fakeClose(int fd) {
    if (fd == c_fdProbe) g.probeCloses++;
    else g.closes++;
    return 0;
}
int fakeRead(int fd, void* buf, int len) {
    if (g.readFails) return -1;
    auto& ring = g.in[fd - c_fdA];
    int n = 0;
    uint8_t* dst = static_cast<uint8_t*>(buf);
    while (n < len && n < g.readLimit && ring.pop(dst[n])) n++;
    return n;
}
int fakeWrite(int fd, const void* buf, int len) {
    if (g.writeShort >= 0) {
        len = g.writeShort;
        g.writeShort = -1;
    }
    auto& peer = g.in[(fd == c_fdA ? c_fdB : c_fdA) - c_fdA];
    const uint8_t* src = static_cast<const uint8_t*>(buf);
    for (int i = 0; i < len; i++) peer.push(src[i]);
    return len;
}
const PcdrvNetOps c_ops = {fakeOpen, fakeClose, fakeRead, fakeWrite};

void resetHost() { g = FakeHost{}; }

void feed(int fd, const uint8_t* p, int n) {
    for (int i = 0; i < n; i++) g.in[fd - c_fdA].push(p[i]);
}

}  // namespace

TEST(no_host_networking_means_no_socket) {
    resetHost();
    g.netEnabled = false;
    PcdrvSocket s(c_ops);
    CHECK(!s.open("127.0.0.1:6699"));
    CHECK(!s.isOpen());
    CHECK_EQ(g.opens, 1);  // the probe only; tcp:// is never tried
}

TEST(probe_then_connect_with_the_tcp_name) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("10.0.0.2:7000"));
    CHECK(s.isOpen());
    CHECK_EQ(g.probeCloses, 1);
    CHECK(strcmp(g.lastName, "tcp://10.0.0.2:7000") == 0);
}

TEST(refused_connect_leaves_the_socket_closed) {
    resetHost();
    g.connectOk = false;
    PcdrvSocket s(c_ops);
    CHECK(!s.open(PcdrvSocket::c_defaultServer));
    CHECK(!s.isOpen());
    CHECK_EQ(g.probeCloses, 1);
    CHECK(strcmp(g.lastName, "tcp://127.0.0.1:6699") == 0);
}

TEST(overlong_or_empty_server_is_refused_before_any_call) {
    resetHost();
    PcdrvSocket s(c_ops);
    char longName[PcdrvSocket::c_maxServerLen + 2];
    memset(longName, 'a', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = 0;
    CHECK(!s.open(longName));
    CHECK(!s.open(""));
    CHECK(!s.open(nullptr));
    CHECK_EQ(g.opens, 0);
}

TEST(open_again_keeps_the_connection) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("127.0.0.1:6699"));
    CHECK(s.open("127.0.0.1:6699"));
    CHECK_EQ(g.opens, 2);  // one probe + one connect, not four
}

TEST(nothing_moves_until_poll_and_poll_never_waits) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("h:1"));
    const uint8_t msg[3] = {1, 2, 3};
    CHECK(s.writeAll(msg, 3));
    CHECK_EQ(g.in[1].size(), 0);  // queued, not yet written
    s.poll();                     // nothing has arrived: returns at once
    CHECK_EQ(g.in[1].size(), 3);
    CHECK_EQ(s.available(), 0);
    CHECK_EQ(s.bytesSent(), 3);
}

TEST(poll_takes_everything_that_has_arrived_across_short_reads) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("h:1"));
    uint8_t data[700];
    for (int i = 0; i < 700; i++) data[i] = static_cast<uint8_t>(i * 7);
    feed(c_fdA, data, 700);
    s.poll();  // 256 + 256 + 188: the short read ends the loop
    CHECK_EQ(s.available(), 700);
    uint8_t out[700];
    CHECK_EQ(s.read(out, sizeof(out)), 700);
    CHECK_MEM(out, data, 700);
    CHECK_EQ(s.bytesReceived(), 700);
}

TEST(write_all_is_all_or_nothing) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("h:1"));
    uint8_t big[1024] = {};
    CHECK(s.writeAll(big, 1000));
    CHECK(!s.writeAll(big, 100));  // 24 left
    CHECK_EQ(s.txSpace(), 24);
    CHECK(s.txCongestion() > 240);
    s.poll();
    CHECK_EQ(s.txSpace(), s.txCapacity());
    CHECK_EQ(g.in[1].size(), 1000);
}

TEST(closed_socket_refuses_writes) {
    resetHost();
    PcdrvSocket s(c_ops);
    const uint8_t b = 1;
    CHECK(!s.writeAll(&b, 1));
    CHECK_EQ(s.write(&b, 1), 0);
}

TEST(peer_gone_on_read_closes_and_drops_queues) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("h:1"));
    const uint8_t msg[4] = {9, 9, 9, 9};
    CHECK(s.writeAll(msg, 4));
    g.readFails = true;
    s.poll();
    CHECK(!s.isOpen());
    CHECK_EQ(g.closes, 1);
    CHECK_EQ(s.txSpace(), s.txCapacity());
    CHECK_EQ(g.in[1].size(), 0);  // the queued bytes never went out
}

TEST(short_write_closes_rather_than_corrupting_the_stream) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("h:1"));
    const uint8_t msg[10] = {};
    CHECK(s.writeAll(msg, 10));
    g.writeShort = 4;
    s.poll();
    CHECK(!s.isOpen());
    // And it can be reopened afterwards.
    CHECK(s.open("h:1"));
    CHECK(s.isOpen());
}

TEST(netlink_runs_unchanged_over_two_sockets) {
    resetHost();
    g.readLimit = 5;  // bytes dribble in, as they would over a real socket
    PcdrvSocket sa(c_ops), sb(c_ops);
    CHECK(sa.open("h:1"));
    CHECK(sb.open("h:1"));
    RecordingHandler ha, hb;
    NetLink la(sa, ha), lb(sb, hb);
    la.setLocalSlot(1);
    lb.setLocalSlot(0);
    const uint8_t ev[6] = {'P', 'C', 'D', 'R', 'V', '!'};
    CHECK(la.sendReliable(PacketType::Event, ev, 6));
    const uint8_t app[3] = {7, 8, 9};
    CHECK(lb.sendReliable(PacketType::AppData, app, 3));
    for (int i = 0; i < 40; i++) {
        la.poll(4096);
        lb.poll(4096);
    }
    CHECK_EQ(hb.eventCount, 1);
    CHECK_MEM(hb.lastEvent, ev, 6);
    CHECK_EQ(hb.lastEventSlot, 1);
    CHECK_EQ(ha.appDataCount, 1);
    CHECK_MEM(ha.lastAppData, app, 3);
    CHECK(la.reliableIdle());
    CHECK(lb.reliableIdle());
}

TEST(netlink_switches_transport) {
    resetHost();
    MockTransport::Ring r1, r2;
    MockTransport mock(r1, r2);
    PcdrvSocket s(c_ops);
    CHECK(s.open("h:1"));
    RecordingHandler h;
    NetLink l(mock, h);
    CHECK(&l.transport() == &mock);
    l.setTransport(s);
    l.reset();
    CHECK(&l.transport() == &s);
    CHECK(l.send(PacketType::Ping, nullptr, 0));
    CHECK_EQ(r1.size(), 0);   // nothing went to the old transport
    CHECK(s.txSpace() < s.txCapacity());
}

int main() { return psxsplash::test::runAll(); }
