#include "nettest.hh"

#include "netlink.hh"
#include "ringbuffer.hh"

// Result mirror in fixed globals (unmangled) so a headless emulator harness can
// read the outcome from RAM by symbol address. 0xC0DE0000 | count encodings.
extern "C" {
volatile uint32_t g_netTestDone = 0;
volatile uint32_t g_netTestPassed = 0;
volatile uint32_t g_netTestFailed = 0;
}

namespace {

using namespace psxsplash;
using namespace psxsplash::net;

// One end of a bidirectional in-RAM pipe: writes go to `out`, reads come from
// `in`. Two of these with the rings crossed form a full-duplex link with no
// hardware involved.
class PipeEnd final : public INetTransport {
  public:
    PipeEnd(RingBuffer<4096>& out, RingBuffer<4096>& in) : m_out(out), m_in(in) {}
    void poll() override {}  // data is already in the rings
    uint32_t available() const override { return m_in.size(); }
    uint32_t read(uint8_t* dst, uint32_t max) override {
        uint32_t n = 0;
        uint8_t b;
        while (n < max && m_in.pop(b)) dst[n++] = b;
        return n;
    }
    uint32_t write(const uint8_t* src, uint32_t len) override {
        uint32_t n = 0;
        while (n < len && m_out.push(src[n])) n++;
        return n;
    }

  private:
    RingBuffer<4096>& m_out;
    RingBuffer<4096>& m_in;
};

// Records everything the NetLink delivers so the test can assert on it.
class TestHandler final : public NetLinkHandler {
  public:
    int helloCount = 0, ackCount = 0, snapshotCount = 0, eventCount = 0, appDataCount = 0, byeCount = 0;
    uint8_t lastHelloSlot = 0xFF;
    HelloPayload lastHello{};
    HelloAckPayload lastAck{};
    uint16_t lastSnapLen = 0;
    uint8_t lastSnap[c_maxPayload]{};
    uint16_t lastEventLen = 0;
    uint8_t lastEvent[128]{};
    uint16_t lastAppDataLen = 0;
    uint8_t lastAppData[128]{};

    void onHello(const HelloPayload& h, uint8_t srcSlot) override {
        helloCount++;
        lastHello = h;
        lastHelloSlot = srcSlot;
    }
    void onHelloAck(const HelloAckPayload& a) override {
        ackCount++;
        lastAck = a;
    }
    void onSnapshot(const uint8_t* payload, uint16_t len, uint8_t, uint16_t) override {
        snapshotCount++;
        lastSnapLen = len;
        if (len <= c_maxPayload) __builtin_memcpy(lastSnap, payload, len);
    }
    void onEvent(const uint8_t* payload, uint16_t len, uint8_t, uint16_t) override {
        eventCount++;
        lastEventLen = len;
        if (len <= sizeof(lastEvent)) __builtin_memcpy(lastEvent, payload, len);
    }
    void onObjectState(const uint8_t*, uint16_t, uint8_t, uint16_t) override {}
    void onAppData(const uint8_t* payload, uint16_t len, uint8_t, uint16_t) override {
        appDataCount++;
        lastAppDataLen = len;
        if (len && len <= sizeof(lastAppData)) __builtin_memcpy(lastAppData, payload, len);
    }
    void onPeerBye(uint8_t) override { byeCount++; }
};

}  // namespace

namespace psxsplash {

NetTestResult runNetLinkSelfTest() {
    static RingBuffer<4096> a2b;  // host -> client
    static RingBuffer<4096> b2a;  // client -> host
    a2b.clear();
    b2a.clear();

    PipeEnd hostT(a2b, b2a);
    PipeEnd clientT(b2a, a2b);
    TestHandler hostH, clientH;

    NetLink host(hostT, hostH);
    host.setLocalSlot(c_hostSlot);
    NetLink client(clientT, clientH);
    client.setLocalSlot(1);

    int passed = 0, failed = 0;
    auto check = [&](bool c) {
        if (c) passed++;
        else failed++;
    };
    auto pumpN = [&](int n) {
        for (int i = 0; i < n; i++) {
            host.poll();
            client.poll();
        }
    };

    // 1. Handshake: client -> Hello, host receives it intact and slot-tagged.
    HelloPayload hello{};
    hello.magic = c_magic;
    hello.protoVersion = c_protoVersion;
    hello.sceneHash = 0xDEADBEEFu;
    hello.tiebreak = 42;
    client.send(PacketType::Hello, reinterpret_cast<const uint8_t*>(&hello), sizeof(hello));
    pumpN(2);
    check(hostH.helloCount == 1);
    check(hostH.lastHello.sceneHash == 0xDEADBEEFu);
    check(hostH.lastHello.magic == c_magic);
    check(hostH.lastHelloSlot == 1);

    // 2. Host -> HelloAck, client receives assignment.
    HelloAckPayload ack{};
    ack.magic = c_magic;
    ack.protoVersion = c_protoVersion;
    ack.yourSlot = 1;
    ack.hostSlot = c_hostSlot;
    ack.sceneHash = 0xDEADBEEFu;
    ack.playerCount = 2;
    host.send(PacketType::HelloAck, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
    pumpN(2);
    check(clientH.ackCount == 1);
    check(clientH.lastAck.yourSlot == 1);
    check(clientH.lastAck.playerCount == 2);

    // 3. Unreliable snapshot host -> client, bytes intact.
    uint8_t snap[20];
    for (int i = 0; i < 20; i++) snap[i] = static_cast<uint8_t>(i * 7 + 1);
    host.send(PacketType::Snapshot, snap, 20);
    pumpN(2);
    check(clientH.snapshotCount == 1);
    check(clientH.lastSnapLen == 20);
    check(__builtin_memcmp(clientH.lastSnap, snap, 20) == 0);

    // 4. Reliable event client -> host: delivered exactly once, acked, then idle.
    uint8_t ev[5] = {1, 2, 3, 4, 5};
    client.sendReliable(PacketType::Event, ev, 5);
    pumpN(6);
    check(hostH.eventCount == 1);
    check(hostH.lastEventLen == 5);
    check(__builtin_memcmp(hostH.lastEvent, ev, 5) == 0);
    check(client.reliableIdle());

    // 5. Two more reliable events: both delivered once, in order (stop-and-wait).
    uint8_t e1[1] = {0xAA};
    uint8_t e2[1] = {0xBB};
    client.sendReliable(PacketType::Event, e1, 1);
    client.sendReliable(PacketType::Event, e2, 1);
    pumpN(14);
    check(hostH.eventCount == 3);
    check(hostH.lastEvent[0] == 0xBB);
    check(client.reliableIdle());

    // 6. AppData: the game's opaque reliable channel. Must reach onAppData and
    //    NOT leak into onEvent — they share a seq stream, so a routing mistake
    //    would silently deliver room data to the game-event handler.
    const uint8_t app[4] = {'R', 'O', 'O', 'M'};
    client.sendReliable(PacketType::AppData, app, 4);
    pumpN(6);
    check(hostH.appDataCount == 1);
    check(hostH.lastAppDataLen == 4);
    check(__builtin_memcmp(hostH.lastAppData, app, 4) == 0);
    check(hostH.eventCount == 3);  // unchanged by the AppData round trip
    check(client.reliableIdle());

    // 7. Corruption/resync: inject garbage (including a false sync + impossible
    //    length) into the host->client stream, then a valid frame. The garbage
    //    must be rejected and the following valid frame must still parse.
    uint8_t garbage[10] = {0xA5, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xFF, 0xFF, 0x99};
    for (uint8_t g : garbage) a2b.push(g);
    uint32_t errBefore = client.crcErrors() + client.resyncs();
    host.send(PacketType::Snapshot, snap, 20);
    pumpN(3);
    check(clientH.snapshotCount == 2);  // valid frame parsed despite preceding garbage
    check((client.crcErrors() + client.resyncs()) > errBefore);

    return {passed, failed};
}

}  // namespace psxsplash
