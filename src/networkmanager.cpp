#include "networkmanager.hh"

#include "netinterp.hh"

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
    // sendHello() raise c_flagRebind - clear it and the rebind silently
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
    m_helloTimer = 0;  // send a Hello promptly on the next preTick
    m_snapshotTimer = c_snapshotIntervalDt;
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
    for (uint8_t s = 0; s < c_maxSlots; ++s) {
        m_remoteAvatarActor[s] = 0xFFFF;
        // The targets name positions in the scene that is going away, so the next
        // scene's first snapshot must apply outright rather than easing from a
        // stale one.
        m_avatarTarget[s].active = false;
    }
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
        m_helloTimer = 0;
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

uint32_t NetworkManager::lerpIntervalMillis() const {
    // The first slot actually being interpolated. One number is enough: every
    // remote rides the same server cadence, so they agree to within a frame.
    for (uint8_t slot = 0; slot < c_maxSlots; ++slot) {
        const AvatarTarget& t = m_avatarTarget[slot];
        if (!t.active || t.intervalSamples == 0) continue;
        return (static_cast<uint32_t>(t.intervalDt) * 1000u) / (30u * 4096u);
    }
    return 0;
}

void NetworkManager::interpolateRemotes(SceneManager& sm, int32_t dt12) {
    // Once per frame, regardless of whether a snapshot arrived: that is the whole
    // point - between (and through) snapshots the avatar keeps moving smoothly.
    //
    // INTERPOLATE BETWEEN TWO KNOWN POSITIONS, rather than easing toward one.
    //
    // The previous version applied exponential smoothing toward the newest
    // received position. That converges on the target and then does NOTHING until
    // the next snapshot lands - so at the 15Hz a serial peer receives, a remote
    // avatar moved for a few frames and stood still for the rest of each 67ms
    // window. On hardware that read as "other players' movement is choppy" while
    // the local player looked perfect, and the asymmetry was the whole clue: the
    // local avatar is driven by input every frame and never runs out of work.
    //
    // Walking from the previous position to the current one across the MEASURED
    // interval keeps it moving for the entire gap. The cost is one interval of
    // latency, which is the trade every networked game makes and is invisible
    // beside the stutter it removes.
    for (uint8_t slot = 0; slot < c_maxSlots; ++slot) {
        if (slot == m_localSlot) continue;  // our own avatar is ours to move
        AvatarTarget& t = m_avatarTarget[slot];
        if (!t.active) continue;

        const uint16_t proxy = remoteAvatarActor(slot);
        if (proxy == 0xFFFF || !sm.isValidActor(proxy)) continue;

        if (dt12 > 0) t.elapsedDt += dt12;

        int32_t interval = t.intervalDt;
        if (interval < c_lerpMinIntervalDt) interval = c_lerpDefaultIntervalDt;

        // Progress through the interval, 4.12. Clamped at 1: when a snapshot is
        // late the avatar waits at the last known position rather than
        // extrapolating into a wall. Guessing forward is what makes a player slide
        // through geometry on a hiccup, and this link hiccups.
        const int32_t alpha = lerpAlpha(t.elapsedDt, interval);

        const int32_t dx = t.px - t.prevPx;
        const int32_t dy = t.py - t.prevPy;
        const int32_t dz = t.pz - t.prevPz;

        // Nothing to do: this avatar has arrived and is standing still. Do NOT
        // write the position back - setActorPosition shifts the AABB and flags the
        // object dynamic-moved, dropping it out of the BVH-accelerated render pass
        // into the linear one. A stationary player would otherwise pay that every
        // frame, for nothing.
        if (dx == 0 && dy == 0 && dz == 0) continue;
        if (alpha == 4096) {
            psyqo::Vec3 cur;
            if (sm.getActorPosition(proxy, cur) && cur.x.value == t.px && cur.y.value == t.py &&
                cur.z.value == t.pz) {
                continue;
            }
        }

        // A teleport is not a walk. Anything beyond the snap distance is applied
        // whole, so vents and respawns land instantly instead of sliding.
        const int32_t adx = dx < 0 ? -dx : dx;
        const int32_t adz = dz < 0 ? -dz : dz;
        if (adx > c_lerpSnapDistance || adz > c_lerpSnapDistance) {
            psyqo::Vec3 snap;
            snap.x.value = t.px;
            snap.y.value = t.py;
            snap.z.value = t.pz;
            sm.setActorPosition(proxy, snap);
            t.prevPx = t.px;
            t.prevPy = t.py;
            t.prevPz = t.pz;
            continue;
        }

        // Multiply THEN divide so truncation rounds toward zero and a negative
        // delta cannot round away from the target. The snap check above bounds
        // |d| to c_lerpSnapDistance (48<<12), so d * 4096 stays inside int32.
        psyqo::Vec3 out;
        out.x.value = lerpComponent(t.prevPx, t.px, alpha);
        out.y.value = lerpComponent(t.prevPy, t.py, alpha);
        out.z.value = lerpComponent(t.prevPz, t.pz, alpha);
        sm.setActorPosition(proxy, out);
    }
}

void NetworkManager::applyPendingSnapshot(SceneManager& sm) {
    if (!m_hasPendingSnapshot) return;
    m_hasPendingSnapshot = false;

    const uint8_t* p = m_pendingSnapshot;
    uint16_t len = m_pendingLen;
    if (len < 4) return;

    uint8_t avatarCount = p[0];
    uint8_t objectCount = p[1];
    const uint8_t format = p[2];
    const uint8_t posShift = p[3];
    uint32_t off = 4;

    // Compact snapshots carry one absolute origin, then 16-bit offsets from it.
    // See netprotocol.hh: the origin and shift travel WITH the data so there is no
    // negotiated state to lose.
    const bool compact = (format == c_snapFormatCompact);
    int32_t originX = 0, originY = 0, originZ = 0;
    if (compact) {
        if (len < off + c_snapOriginBytes) return;
        __builtin_memcpy(&originX, p + off + 0, 4);
        __builtin_memcpy(&originY, p + off + 4, 4);
        __builtin_memcpy(&originZ, p + off + 8, 4);
        off += c_snapOriginBytes;
    } else if (format != c_snapFormatLegacy) {
        // A format this build does not know. Discarding is the only safe answer:
        // guessing at the layout would drive every actor to a wrong position,
        // which is far worse than a missed frame of interpolation.
        m_snapshotFormatUnknown++;
        return;
    }

    const uint32_t avatarSize = compact ? c_snapCompactAvatarSize : c_snapLegacyRecordSize;
    const uint32_t objectSize = compact ? c_snapCompactObjectSize : c_snapLegacyRecordSize;

    // Read a record body (position + yaw) that starts `bodyOff` bytes into the
    // record, in whichever format this snapshot uses.
    auto readBody = [&](uint32_t bodyOff, RecordWire& r) {
        if (compact) {
            uint16_t dx, dy, dz, yaw;
            __builtin_memcpy(&dx, p + bodyOff + 0, 2);
            __builtin_memcpy(&dy, p + bodyOff + 2, 2);
            __builtin_memcpy(&dz, p + bodyOff + 4, 2);
            __builtin_memcpy(&yaw, p + bodyOff + 6, 2);
            r.px = originX + (static_cast<int32_t>(dx) << posShift);
            r.py = originY + (static_cast<int32_t>(dy) << posShift);
            r.pz = originZ + (static_cast<int32_t>(dz) << posShift);
            r.yaw = static_cast<int32_t>(yaw);
        } else {
            __builtin_memcpy(&r, p + bodyOff, sizeof(RecordWire));
        }
    };

    // Avatars: apply each remote slot's avatar to its local proxy actor.
    for (uint8_t i = 0; i < avatarCount; ++i) {
        if (off + avatarSize > len) break;
        uint8_t slot = p[off];
        // uint8_t flags = p[off + 1];
        RecordWire r;
        readBody(off + 2 + (compact ? 0 : 2), r);  // legacy has a 2-byte pad
        off += avatarSize;

        if (slot == m_localSlot) continue;  // never overwrite our own avatar
        uint16_t proxy = remoteAvatarActor(slot);
        if (proxy == 0xFFFF || !sm.isValidActor(proxy)) continue;

        psyqo::Vec3 pos;
        pos.x.value = r.px;
        pos.y.value = r.py;
        pos.z.value = r.pz;

        // Start a new interpolation leg. interpolateRemotes() walks the actor from
        // prev to the new target across the measured interval.
        //
        // `prev` is where the avatar ACTUALLY IS on screen, not the previous
        // target. Those differ whenever the last leg had not finished - a late
        // snapshot, a dropped one, a frame-rate dip - and starting from the stale
        // target instead would teleport the avatar backwards by the unfinished
        // remainder on every such packet. Reading the rendered position makes
        // every leg continuous with what the player is looking at.
        AvatarTarget& t = m_avatarTarget[slot];
        if (!t.active) {
            // First snapshot for this slot: place it outright, or it would glide
            // in from wherever the scene happened to put the proxy.
            t.active = true;
            sm.setActorPosition(proxy, pos);
            t.prevPx = r.px;
            t.prevPy = r.py;
            t.prevPz = r.pz;
            t.intervalDt = c_lerpDefaultIntervalDt;
            t.intervalSamples = 0;
        } else {
            psyqo::Vec3 cur;
            if (sm.getActorPosition(proxy, cur)) {
                t.prevPx = cur.x.value;
                t.prevPy = cur.y.value;
                t.prevPz = cur.z.value;
            } else {
                t.prevPx = t.px;
                t.prevPy = t.py;
                t.prevPz = t.pz;
            }
            // MEASURE the cadence rather than assume it, then SMOOTH it.
            //
            // Measuring matters because the server sends 30Hz to an emulator and
            // 15Hz to a serial peer, and Phase 4 may make it adaptive; playing back
            // over an assumed interval would run visibly fast or slow the moment
            // that assumption moved.
            //
            // Smoothing matters more, and is the thing whose absence kept remote
            // players choppy on hardware after interpolation was added. A single
            // sample is quantised to whole frames, and at ~19fps against 15Hz
            // snapshots it can only read 52ms or 105ms for a true 67ms gap - so the
            // playback speed alternated 1.6x fast and 0.6x slow. The error
            // alternates sign, so an average converges on the truth where any one
            // reading cannot. See c_lerpIntervalSmoothing.
            int32_t measured = t.elapsedDt;
            if (measured < c_lerpMinIntervalDt) measured = c_lerpMinIntervalDt;
            if (measured > c_lerpMaxIntervalDt) measured = c_lerpMaxIntervalDt;
            if (t.intervalSamples == 0) {
                t.intervalDt = measured;  // nothing to average against yet
                t.intervalSamples = 1;
            } else {
                t.intervalDt += (measured - t.intervalDt) / c_lerpIntervalSmoothing;
            }
        }
        t.elapsedDt = 0;
        t.px = r.px;
        t.py = r.py;
        t.pz = r.pz;

        // Yaw is snapped, not eased: it wraps at a full turn, and easing it
        // without handling that wrap makes a player crossing the seam spin the
        // long way round.
        psyqo::Vec3 rot;
        rot.x.value = 0;
        rot.y.value = r.yaw;
        rot.z.value = 0;
        sm.setActorRotation(proxy, rot);
    }

    // World objects: clients apply the host's authoritative object state.
    for (uint8_t i = 0; i < objectCount; ++i) {
        if (off + objectSize > len) break;
        uint16_t actorId = static_cast<uint16_t>(p[off] | (p[off + 1] << 8));
        RecordWire r;
        readBody(off + (compact ? 3 : 4), r);  // legacy: id,id,flags,pad
        off += objectSize;

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
    // LATEST-WINS, not queued. A snapshot is a statement about the world right
    // now; one still waiting when the next is built was already wrong. See
    // NetLink::sendLatest for why this is a slot rather than a queue.
    m_link.sendLatest(PacketType::Snapshot, buf, static_cast<uint16_t>(off));
}

// --- per-frame -----------------------------------------------------------

void NetworkManager::preTick(SceneManager& sm, int32_t dt12) {
    if (m_state == State::Disconnected || m_state == State::VersionMismatch || m_state == State::SceneMismatch) {
        return;
    }

    m_link.poll(dt12);  // parses frames -> onHello/onHelloAck/onSnapshot

    if (m_state == State::Connecting) {
        m_helloTimer -= dt12;
        if (m_helloTimer <= 0) {
            sendHello();
            m_helloTimer = c_helloIntervalDt;
        }
        return;  // no replication until connected
    }

    // Publish the applied-snapshot rate once a second. See snapshotsPerSecond().
    m_snapWindowDt += dt12 > 0 ? dt12 : 0;
    if (m_snapWindowDt >= 30 * 4096) {
        m_snapsPerSec = (m_snapsThisWindow * 30u * 4096u) / static_cast<uint32_t>(m_snapWindowDt);
        m_snapsThisWindow = 0;
        m_snapWindowDt = 0;
    }
    if (m_hasPendingSnapshot) m_snapsThisWindow++;

    applyPendingSnapshot(sm);
    // Every frame, not just the frames a snapshot happened to land on - that is
    // what turns the 30Hz replication cadence (and any packet it loses) into
    // continuous motion instead of visible steps.
    interpolateRemotes(sm, dt12);

    // Deliver any received reliable game events to the scene script.
    while (m_recvEventCount > 0) {
        PendingGameEvent e = m_recvEvents[m_recvEventHead];
        m_recvEventHead = (m_recvEventHead + 1) % c_eventQueueLen;
        m_recvEventCount--;
        sm.getLua().OnNetEvent(e.id, e.arg);
    }

    // Apply any received per-object state to the matching actor's Lua self.sync.
    // Same authority rule as the snapshot's world objects: the host owns them,
    // and only actors registered with Net.RegisterActor are replicated.
    while (m_objStateCount > 0) {
        PendingObjState& s = m_recvObjStates[m_objStateHead];
        if (!m_isHost && isNetworkedActor(s.actorId)) {
            GameObject* go = sm.getActorGameObject(s.actorId);
            if (go) sm.getLua().ApplyObjectSync(go, s.data, s.len);
        }
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

void NetworkManager::postTick(SceneManager& sm, int32_t dt12) {
    if (m_state != State::Connected) return;
    // A scene with nothing to replicate must not pay for replication. MEASURED on
    // hardware: a console sitting in the lobby spent 894 B/s - essentially its
    // whole outbound budget - broadcasting the position of a player that did not
    // exist, while the reliable message it was waiting for queued behind that traffic.
    // Nothing consumed those snapshots at either end.
    if (!m_replicationEnabled && m_netActorCount == 0) return;
    m_snapshotTimer -= dt12;
    if (m_snapshotTimer <= 0) {
        // No congestion check any more, because there is nothing left to protect
        // against. This used to read `if (txCongestion() < c_snapshotCongestionLimit)`
        // and drop the snapshot when the wire looked busy - a threshold guarding a
        // queue that snapshots should never have been in.
        //
        // NetLink::sendLatest holds exactly one, overwriting rather than queueing,
        // and emits it only after control and reliable traffic. Stale positions
        // therefore cannot accumulate ahead of an acknowledgement no matter how
        // slow the link turns out to be, so there is no threshold to tune and no
        // value of one that reintroduces the bug. Watch link().latestSuperseded()
        // instead: it counts snapshots replaced before reaching the wire, which is
        // a direct measure of how far behind the link is running.
        buildAndSendSnapshot(sm);
        // Assign rather than accumulate: after a long stall the right answer is
        // one fresh snapshot, not a burst of backdated ones the peer would only
        // discard.
        m_snapshotTimer = c_snapshotIntervalDt;
    }
}

}  // namespace psxsplash
