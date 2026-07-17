#include "networkmanager.hh"

#include "scenemanager.hh"
#include "sio1.hh"

namespace psxsplash {

using namespace psxsplash::net;

// Snapshot payload layout (unreliable; only the newest matters):
//   [ uint8 avatarCount ][ uint8 objectCount ][ uint16 reserved ]
//   avatar record (20B): [u8 slot][u8 flags][u16 pad][i32 px][i32 py][i32 pz][i32 yaw]
//   object record (20B): [u16 actorId][u8 flags][u8 pad][i32 px][i32 py][i32 pz][i32 yaw]
namespace {
constexpr uint8_t SNAP_FLAG_ACTIVE = 0x01;
constexpr uint32_t c_recordSize = 20;

struct RecordWire {
    int32_t px, py, pz, yaw;
};
}  // namespace

NetworkManager NetworkManager::s_instance;
NetworkManager& NetworkManager::Get() { return s_instance; }

NetworkManager::NetworkManager() : m_link(Sio1::Get(), *this) {
    for (uint8_t s = 0; s < c_maxSlots; ++s) m_remoteAvatarActor[s] = 0xFFFF;
}

void NetworkManager::begin(uint32_t sceneHash, uint32_t seed, uint32_t baud, Sio1::RxMode rxMode) {
    // A persistent reconnect is a REBIND, not a fresh join: we already hold a
    // slot and only changed scene. Keeping m_localSlot here is what lets
    // sendHello() raise c_flagRebind — clear it and the rebind silently
    // degrades into "new player", which a full room would then refuse.
    const bool rebinding = m_persistent && m_localSlot != c_noSlot;

    Sio1::Get().init(baud, rxMode);
    // Always clear the link: the reliable queue may still hold payloads for the
    // scene we just left, and delivering those into the new scene would apply
    // them to whatever actorIds happen to occupy the same indices.
    m_link.reset();

    m_sceneHash = sceneHash;
    // Derive a well-spread tiebreak from the seed (host = higher tiebreak).
    m_tiebreak = (seed * 2654435761u) ^ (seed >> 15) ^ 0x9E3779B9u;
    if (m_tiebreak == 0) m_tiebreak = 1;  // never 0, keeps comparisons meaningful
    m_state = State::Connecting;  // re-handshake either way, to agree the new scene
    m_hostSlot = c_hostSlot;
    if (!rebinding) {
        m_isHost = false;
        m_localSlot = c_noSlot;
        m_playerCount = 1;
    }
    m_link.setLocalSlot(m_localSlot);
    m_helloTimer = 1;  // send a Hello promptly on the next preTick
    m_snapshotTimer = c_snapshotIntervalFrames;
    m_hasPendingSnapshot = false;
}

void NetworkManager::end() {
    if (m_state == State::Connected) m_link.send(PacketType::Bye, nullptr, 0);
    m_state = State::Disconnected;
}

void NetworkManager::resetSceneBindings() {
    // Everything here names an actorId, and actorIds are indices into the scene
    // that is going away. The session (slot, link, handshake) is deliberately
    // untouched.
    m_netActorCount = 0;
    m_hasPendingSnapshot = false;
    m_recvEventHead = 0;
    m_recvEventCount = 0;
    m_objStateHead = 0;
    m_objStateCount = 0;
    m_appDataHead = 0;
    m_appDataCount = 0;
    m_localAvatarActor = 0;  // PLAYER_ACTOR_ID
    for (uint8_t s = 0; s < c_maxSlots; ++s) m_remoteAvatarActor[s] = 0xFFFF;
}

void NetworkManager::reset() {
    resetSceneBindings();
    m_state = State::Disconnected;
    m_isHost = false;
    m_localSlot = c_noSlot;
    m_playerCount = 1;
    m_persistent = false;
    m_link.reset();
}

void NetworkManager::setRemoteAvatarActor(uint8_t slot, uint16_t actorId) {
    if (slot < c_maxSlots) m_remoteAvatarActor[slot] = actorId;
}
uint16_t NetworkManager::remoteAvatarActor(uint8_t slot) const {
    return slot < c_maxSlots ? m_remoteAvatarActor[slot] : 0xFFFF;
}

bool NetworkManager::registerNetworkedActor(uint16_t actorId) {
    if (isNetworkedActor(actorId)) return true;
    if (m_netActorCount >= c_maxNetActors) return false;
    m_netActors[m_netActorCount++] = actorId;
    return true;
}
void NetworkManager::unregisterNetworkedActor(uint16_t actorId) {
    for (uint32_t i = 0; i < m_netActorCount; ++i) {
        if (m_netActors[i] == actorId) {
            m_netActors[i] = m_netActors[--m_netActorCount];
            return;
        }
    }
}
bool NetworkManager::isNetworkedActor(uint16_t actorId) const {
    for (uint32_t i = 0; i < m_netActorCount; ++i)
        if (m_netActors[i] == actorId) return true;
    return false;
}

bool NetworkManager::sendEvent(const uint8_t* data, uint16_t len) {
    if (m_state != State::Connected) return false;
    return m_link.sendReliable(PacketType::Event, data, len);
}

bool NetworkManager::sendGameEvent(int32_t eventId, int32_t arg) {
    if (m_state != State::Connected) return false;
    uint8_t buf[8];
    __builtin_memcpy(buf + 0, &eventId, 4);
    __builtin_memcpy(buf + 4, &arg, 4);
    return m_link.sendReliable(PacketType::Event, buf, 8);
}

bool NetworkManager::sendObjectState(const uint8_t* payload, uint16_t len) {
    if (m_state != State::Connected) return false;
    return m_link.sendReliable(PacketType::ObjectState, payload, len);
}

bool NetworkManager::sendAppData(const uint8_t* payload, uint16_t len) {
    if (m_state != State::Connected) return false;
    return m_link.sendReliable(PacketType::AppData, payload, len);
}

// --- handshake -----------------------------------------------------------

void NetworkManager::sendHello() {
    HelloPayload h{};
    h.magic = c_magic;
    h.protoVersion = c_protoVersion;
    h.sceneHash = m_sceneHash;
    h.tiebreak = m_tiebreak;
    // If we kept our session across a scene load, say so: the authority should
    // give us the SAME slot back rather than treat us as a new arrival.
    const uint8_t flags = (m_persistent && m_localSlot != c_noSlot) ? c_flagRebind : 0;
    m_link.send(PacketType::Hello, reinterpret_cast<const uint8_t*>(&h), sizeof(h), flags);
}

void NetworkManager::onHello(const HelloPayload& hello, uint8_t /*srcSlot*/) {
    if (hello.magic != c_magic) return;  // foreign data, ignore
    if (hello.protoVersion != c_protoVersion) {
        m_state = State::VersionMismatch;
        return;
    }
    if (hello.sceneHash != m_sceneHash) {
        m_state = State::SceneMismatch;
        return;
    }
    if (m_state == State::Connected) {
        // Already connected; re-ack in case our HelloAck was lost.
        if (m_isHost) {
            HelloAckPayload ack{};
            ack.magic = c_magic;
            ack.protoVersion = c_protoVersion;
            ack.yourSlot = 1;
            ack.hostSlot = c_hostSlot;
            ack.sceneHash = m_sceneHash;
            ack.playerCount = m_playerCount;
            m_link.send(PacketType::HelloAck, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
        }
        return;
    }

    if (m_tiebreak == hello.tiebreak) {
        // Extremely rare tie: re-roll and try again next Hello.
        m_tiebreak ^= 0x9E3779B9u;
        if (m_tiebreak == 0) m_tiebreak = 1;
        m_helloTimer = 1;
        return;
    }

    if (m_tiebreak > hello.tiebreak) {
        // We win: become host, assign the peer slot 1, confirm.
        m_isHost = true;
        m_localSlot = c_hostSlot;
        m_hostSlot = c_hostSlot;
        m_playerCount = 2;
        m_state = State::Connected;
        m_link.setLocalSlot(m_localSlot);

        HelloAckPayload ack{};
        ack.magic = c_magic;
        ack.protoVersion = c_protoVersion;
        ack.yourSlot = 1;
        ack.hostSlot = c_hostSlot;
        ack.sceneHash = m_sceneHash;
        ack.playerCount = m_playerCount;
        m_link.send(PacketType::HelloAck, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
    }
    // Else: peer wins the election; we wait for their HelloAck.
}

void NetworkManager::onHelloAck(const HelloAckPayload& ack) {
    if (ack.magic != c_magic) return;
    if (ack.protoVersion != c_protoVersion) {
        m_state = State::VersionMismatch;
        return;
    }
    if (ack.sceneHash != m_sceneHash) {
        m_state = State::SceneMismatch;
        return;
    }
    m_localSlot = ack.yourSlot;
    m_hostSlot = ack.hostSlot;
    m_isHost = (ack.yourSlot == ack.hostSlot);
    m_playerCount = ack.playerCount;
    m_state = State::Connected;
    m_link.setLocalSlot(m_localSlot);
}

void NetworkManager::onAppData(const uint8_t* payload, uint16_t len, uint8_t /*srcSlot*/, uint16_t /*seq*/) {
    // Opaque application payload. Queued here; handed to the scene script as
    // onNetData(str) from preTick, where Lua is reachable.
    if (len > c_maxEventPayload) return;
    if (m_appDataCount >= c_appDataQueueLen) return;  // drop under saturation
    uint32_t tail = (m_appDataHead + m_appDataCount) % c_appDataQueueLen;
    PendingAppData& d = m_recvAppData[tail];
    d.len = len;
    if (len) __builtin_memcpy(d.data, payload, len);
    m_appDataCount++;
}

void NetworkManager::onPeerBye(uint8_t /*srcSlot*/) {
    // Peer left. Drop to Disconnected; the game keeps running normally.
    m_state = State::Disconnected;
}

// --- replication ---------------------------------------------------------

void NetworkManager::onSnapshot(const uint8_t* payload, uint16_t len, uint8_t srcSlot, uint16_t /*seq*/) {
    if (len > c_maxPayload) return;
    // Stage the newest snapshot; applied in preTick against the SceneManager.
    __builtin_memcpy(m_pendingSnapshot, payload, len);
    m_pendingLen = len;
    m_pendingSlot = srcSlot;
    m_hasPendingSnapshot = true;
}

void NetworkManager::onEvent(const uint8_t* payload, uint16_t len, uint8_t /*srcSlot*/, uint16_t /*seq*/) {
    // Game event payload: [int32 id][int32 arg]. Queued here; delivered to the
    // scene script as onNetEvent(id, arg) from preTick (where Lua is reachable).
    if (len < 8) return;
    if (m_recvEventCount >= c_eventQueueLen) return;  // drop if the queue is saturated
    int32_t id, arg;
    __builtin_memcpy(&id, payload + 0, 4);
    __builtin_memcpy(&arg, payload + 4, 4);
    uint32_t tail = (m_recvEventHead + m_recvEventCount) % c_eventQueueLen;
    m_recvEvents[tail].id = id;
    m_recvEvents[tail].arg = arg;
    m_recvEventCount++;
}

void NetworkManager::onObjectState(const uint8_t* payload, uint16_t len, uint8_t /*srcSlot*/, uint16_t /*seq*/) {
    // Payload: [actorId:u16][serialized self.sync]. Queued; applied to Lua in
    // preTick (where the SceneManager/Lua state is reachable).
    if (len < 2) return;
    uint16_t blobLen = static_cast<uint16_t>(len - 2);
    if (blobLen > c_maxObjStateBlob) return;
    if (m_objStateCount >= c_objStateQueueLen) return;  // drop under saturation
    uint32_t tail = (m_objStateHead + m_objStateCount) % c_objStateQueueLen;
    PendingObjState& s = m_recvObjStates[tail];
    s.actorId = static_cast<uint16_t>(payload[0] | (payload[1] << 8));
    s.len = blobLen;
    __builtin_memcpy(s.data, payload + 2, blobLen);
    m_objStateCount++;
}

void NetworkManager::applyPendingSnapshot(SceneManager& sm) {
    if (!m_hasPendingSnapshot) return;
    m_hasPendingSnapshot = false;

    const uint8_t* p = m_pendingSnapshot;
    uint16_t len = m_pendingLen;
    if (len < 4) return;

    uint8_t avatarCount = p[0];
    uint8_t objectCount = p[1];
    uint32_t off = 4;

    auto readRecord = [&](RecordWire& r) -> bool {
        if (off + c_recordSize > len) return false;
        __builtin_memcpy(&r, m_pendingSnapshot + off + 4, sizeof(RecordWire));
        return true;
    };

    // Avatars: apply each remote slot's avatar to its local proxy actor.
    for (uint8_t i = 0; i < avatarCount; ++i) {
        if (off + c_recordSize > len) break;
        uint8_t slot = m_pendingSnapshot[off];
        // uint8_t flags = m_pendingSnapshot[off + 1];
        RecordWire r;
        readRecord(r);
        off += c_recordSize;

        if (slot == m_localSlot) continue;  // never overwrite our own avatar
        uint16_t proxy = remoteAvatarActor(slot);
        if (proxy == 0xFFFF || !sm.isValidActor(proxy)) continue;

        psyqo::Vec3 pos;
        pos.x.value = r.px;
        pos.y.value = r.py;
        pos.z.value = r.pz;
        sm.setActorPosition(proxy, pos);
        psyqo::Vec3 rot;
        rot.x.value = 0;
        rot.y.value = r.yaw;
        rot.z.value = 0;
        sm.setActorRotation(proxy, rot);
    }

    // World objects: clients apply the host's authoritative object state.
    for (uint8_t i = 0; i < objectCount; ++i) {
        if (off + c_recordSize > len) break;
        uint16_t actorId = static_cast<uint16_t>(m_pendingSnapshot[off] | (m_pendingSnapshot[off + 1] << 8));
        RecordWire r;
        readRecord(r);
        off += c_recordSize;

        if (m_isHost) continue;  // host is authoritative; ignore inbound objects
        if (!isNetworkedActor(actorId) || !sm.isValidActor(actorId)) continue;

        psyqo::Vec3 pos;
        pos.x.value = r.px;
        pos.y.value = r.py;
        pos.z.value = r.pz;
        sm.setActorPosition(actorId, pos);
        psyqo::Vec3 rot;
        rot.x.value = 0;
        rot.y.value = r.yaw;
        rot.z.value = 0;
        sm.setActorRotation(actorId, rot);
    }
}

void NetworkManager::buildAndSendSnapshot(SceneManager& sm) {
    uint8_t buf[c_maxPayload];
    uint32_t off = 4;  // header filled in last

    // Our own avatar (always sent).
    uint8_t avatarCount = 0;
    if (sm.isValidActor(m_localAvatarActor)) {
        psyqo::Vec3 pos, rot;
        sm.getActorPosition(m_localAvatarActor, pos);
        sm.getActorRotation(m_localAvatarActor, rot);
        buf[off + 0] = m_localSlot;
        buf[off + 1] = SNAP_FLAG_ACTIVE;
        buf[off + 2] = 0;
        buf[off + 3] = 0;
        RecordWire r{pos.x.value, pos.y.value, pos.z.value, rot.y.value};
        __builtin_memcpy(buf + off + 4, &r, sizeof(r));
        off += c_recordSize;
        avatarCount = 1;
    }

    // Host-authoritative world objects.
    uint8_t objectCount = 0;
    if (m_isHost) {
        for (uint32_t i = 0; i < m_netActorCount; ++i) {
            uint16_t actorId = m_netActors[i];
            if (!sm.isValidActor(actorId)) continue;
            if (off + c_recordSize > c_maxPayload) break;  // stay within one frame
            psyqo::Vec3 pos, rot;
            sm.getActorPosition(actorId, pos);
            sm.getActorRotation(actorId, rot);
            buf[off + 0] = static_cast<uint8_t>(actorId & 0xFF);
            buf[off + 1] = static_cast<uint8_t>(actorId >> 8);
            buf[off + 2] = SNAP_FLAG_ACTIVE;
            buf[off + 3] = 0;
            RecordWire r{pos.x.value, pos.y.value, pos.z.value, rot.y.value};
            __builtin_memcpy(buf + off + 4, &r, sizeof(r));
            off += c_recordSize;
            ++objectCount;
        }
    }

    buf[0] = avatarCount;
    buf[1] = objectCount;
    buf[2] = 0;
    buf[3] = 0;
    m_link.send(PacketType::Snapshot, buf, static_cast<uint16_t>(off));
}

// --- per-frame -----------------------------------------------------------

void NetworkManager::preTick(SceneManager& sm) {
    if (m_state == State::Disconnected || m_state == State::VersionMismatch || m_state == State::SceneMismatch) {
        return;
    }

    m_link.poll();  // parses frames -> onHello/onHelloAck/onSnapshot

    if (m_state == State::Connecting) {
        if (m_helloTimer > 0) m_helloTimer--;
        if (m_helloTimer == 0) {
            sendHello();
            m_helloTimer = c_helloIntervalFrames;
        }
        return;  // no replication until connected
    }

    applyPendingSnapshot(sm);

    // Deliver any received reliable game events to the scene script.
    while (m_recvEventCount > 0) {
        PendingGameEvent e = m_recvEvents[m_recvEventHead];
        m_recvEventHead = (m_recvEventHead + 1) % c_eventQueueLen;
        m_recvEventCount--;
        sm.getLua().OnNetEvent(e.id, e.arg);
    }

    // Apply any received per-object state to the matching actor's Lua self.sync.
    while (m_objStateCount > 0) {
        PendingObjState& s = m_recvObjStates[m_objStateHead];
        GameObject* go = sm.getActorGameObject(s.actorId);
        if (go) sm.getLua().ApplyObjectSync(go, s.data, s.len);
        m_objStateHead = (m_objStateHead + 1) % c_objStateQueueLen;
        m_objStateCount--;
    }

    // Hand any received application payloads to the scene script.
    while (m_appDataCount > 0) {
        PendingAppData& d = m_recvAppData[m_appDataHead];
        sm.getLua().OnNetData(d.data, d.len);
        m_appDataHead = (m_appDataHead + 1) % c_appDataQueueLen;
        m_appDataCount--;
    }
}

void NetworkManager::postTick(SceneManager& sm) {
    if (m_state != State::Connected) return;
    if (m_snapshotTimer > 0) m_snapshotTimer--;
    if (m_snapshotTimer == 0) {
        buildAndSendSnapshot(sm);
        m_snapshotTimer = c_snapshotIntervalFrames;
    }
}

}  // namespace psxsplash
