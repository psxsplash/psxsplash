#pragma once

#include <stdint.h>

#include "netlink.hh"
#include "netprotocol.hh"
#include "sio1.hh"

namespace psxsplash {

class SceneManager;

/**
 * NetworkManager - session, ownership, and actor-state replication over SIO1.
 *
 * Sits above NetLink (framing/reliability) and drives the replication model on
 * top of the actor system:
 *
 *   - Player avatars are keyed by SLOT, because each console's own player is
 *     actorId 0 locally. Each console sends its own avatar (its local avatar
 *     actor, default actor 0) tagged with its slot; a receiver applies an
 *     incoming avatar to the local proxy actor it has mapped for that slot.
 *
 *   - Shared world objects are keyed by ACTORID, which is identical across
 *     consoles running the same splashpack. These are host-authoritative: the
 *     host captures and sends them; clients apply them.
 *
 * A symmetric handshake agrees protocol version + scene hash and elects a host
 * (higher tiebreak wins) / assigns slots. Dormant until begin(): a build that
 * never starts a session pays nothing.
 */
class NetworkManager final : public net::NetLinkHandler {
  public:
    enum class State : uint8_t {
        Disconnected,
        Connecting,
        Connected,
        VersionMismatch,
        SceneMismatch,
    };

    // Slot 0 is always the authority. In a peer-to-peer link that is one of the two
    // consoles; under a central server it is the SERVER, which has no avatar of its
    // own - so 10 players occupy slots 1..10 and need ELEVEN slots. Getting this off
    // by one silently costs a player.
    //
    // Ten is what the snapshot budget supports, not an arbitrary cap. In the compact
    // format a ten-avatar snapshot is 4 + 12 + 10*10 = 116 bytes plus 11 of framing;
    // at the 30Hz cadence that is ~3.8 KB/s against the 5.76 KB/s a 57600 link
    // carries. Raising it means shrinking the record or lowering the cadence first.
    static constexpr uint8_t c_maxSlots = 11;

    static NetworkManager& Get();

    // Start a session over SIO1. `sceneHash` identifies the loaded scene (both
    // ends must match). `seed` seeds the host-election tiebreak. `rxMode`
    // defaults to Auto, which picks the correct strategy for the emulator vs
    // real hardware on its own - see sio1.hh before overriding it.
    // The baud default comes from Sio1, never a literal. A second copy of this
    // number in the Lua binding is what made changing it move the bridge and not
    // the console, and a baud mismatch presents as an unplugged cable.
    void begin(uint32_t sceneHash, uint32_t seed, uint32_t baud = Sio1::c_defaultBaud,
               Sio1::RxMode rxMode = Sio1::RxMode::Auto);
    void end();

    // Drop everything: session (slot, handshake, link) AND scene bindings.
    void reset();

    // Drop only the scene-scoped bindings, keeping the session alive.
    //
    // actorIds are indices into the loaded splashpack, so every binding that
    // names one dies with the scene. The SESSION does not have to: our slot and
    // the link itself outlive a scene change. Splitting the two is what lets a
    // lobby scene hand over to the game scene without the server seeing a
    // disconnect and a brand new player.
    void resetSceneBindings();

    // Survive the next scene load (see resetSceneBindings). After the new scene
    // is up, re-Hello with c_flagRebind to reclaim the same slot.
    void setPersistent(bool persistent) { m_persistent = persistent; }
    bool isPersistent() const { return m_persistent; }

    // Start of GameTick: pump link, advance handshake, apply remote state.
    void preTick(SceneManager& sm, int32_t dt12);
    // End of GameTick: at the net-tick cadence, capture owned state and send.
    void postTick(SceneManager& sm, int32_t dt12);

    // Which local actor is THIS console's player avatar (sent to peers). Default
    // actor 0 (the player).
    void setLocalAvatarActor(uint16_t actorId) { m_localAvatarActor = actorId; }

    /// Whether this console replicates its own avatar at all.
    ///
    /// A scene with no avatar in it - a menu, a lobby, a cutscene - has nothing
    /// worth replicating, and sending anyway is not free. Measured on hardware: a
    /// console sitting in the lobby was spending 894 B/s, essentially its entire
    /// outbound budget, broadcasting the position of a player that did not exist,
    /// while the reliable message it was waiting for queued behind that traffic.
    ///
    /// The server already had this concept (Game.replicates) and suppressed its
    /// own lobby fan-out; the console had no equivalent and broadcast regardless.
    ///
    /// Defaults to true, so no existing game changes behaviour by upgrading. A
    /// game that knows a scene is not a gameplay scene should turn it off - see
    /// Net.SetReplicationEnabled.
    void setReplicationEnabled(bool enabled) { m_replicationEnabled = enabled; }
    bool replicationEnabled() const { return m_replicationEnabled; }
    // Which local proxy actor represents a remote slot's player avatar. Set this
    // (e.g. from Lua) so incoming avatars for `slot` drive a visible object.
    void setRemoteAvatarActor(uint8_t slot, uint16_t actorId);
    uint16_t remoteAvatarActor(uint8_t slot) const;

    // Register a shared world object (by actorId) for host-authoritative sync.
    bool registerNetworkedActor(uint16_t actorId);
    void unregisterNetworkedActor(uint16_t actorId);
    bool isNetworkedActor(uint16_t actorId) const;

    // Reliable event to peers/server.
    bool sendEvent(const uint8_t* data, uint16_t len);
    // Reliable game event (id + arg), surfaced to the scene script as
    // onNetEvent(id, arg) on the receiving console(s).
    bool sendGameEvent(int32_t eventId, int32_t arg);
    // Reliable per-object Lua state. `payload` = [actorId:u16][serialized self.sync];
    // the receiver deserialises it into that actor's object `self.sync`.
    bool sendObjectState(const uint8_t* payload, uint16_t len);
    // Reliable opaque application payload, surfaced to the receiving scene
    // script as onNetData(str). The engine never interprets it.
    bool sendAppData(const uint8_t* payload, uint16_t len);
    // Queued reliable packets vs capacity - for app-layer backpressure.
    uint32_t reliableQueueDepth() const { return m_link.reliableQueueDepth(); }
    /// Snapshots replaced before they reached the wire.
    ///
    /// This used to count snapshots refused by a congestion threshold. There is no
    /// threshold any more - NetLink holds one latest-wins slot and overwrites it -
    /// so the number now comes from the place that actually knows. It means the
    /// same thing to a reader and is a better measurement: a rising count is a
    /// direct statement that the link cannot carry the offered rate.
    uint32_t snapshotsDropped() const { return m_link.latestSuperseded(); }
    uint32_t snapshotFormatUnknown() const { return m_snapshotFormatUnknown; }

    /// Snapshots APPLIED per second, and the interval the interpolator is
    /// actually playing back over (milliseconds).
    ///
    /// These separate the only two explanations for choppy remote movement, which
    /// otherwise take a disc each to tell apart: if the rate is steady and near the
    /// server's cadence, arrival is fine and any remaining stutter is playback; if
    /// it is low or erratic, the packets are not getting here and no amount of
    /// interpolation will help.
    uint32_t snapshotsPerSecond() const { return m_snapsPerSec; }
    uint32_t lerpIntervalMillis() const;

    // Session queries.
    State state() const { return m_state; }
    bool isConnected() const { return m_state == State::Connected; }
    bool isHost() const { return m_isHost; }
    uint8_t localSlot() const { return m_localSlot; }
    uint8_t playerCount() const { return m_playerCount; }
    const net::NetLink& link() const { return m_link; }

    // --- net::NetLinkHandler ---
    void onHello(const net::HelloPayload& hello, uint8_t srcSlot) override;
    void onHelloAck(const net::HelloAckPayload& ack) override;
    void onSnapshot(const uint8_t* payload, uint16_t len, uint8_t srcSlot, uint16_t seq) override;
    void onEvent(const uint8_t* payload, uint16_t len, uint8_t srcSlot, uint16_t seq) override;
    void onObjectState(const uint8_t* payload, uint16_t len, uint8_t srcSlot, uint16_t seq) override;
    void onAppData(const uint8_t* payload, uint16_t len, uint8_t srcSlot, uint16_t seq) override;
    void onPeerBye(uint8_t srcSlot) override;

  private:
    NetworkManager();
    static NetworkManager s_instance;

    void sendHello();
    void applyPendingSnapshot(SceneManager& sm);
    void buildAndSendSnapshot(SceneManager& sm);
    /// Ease every remote avatar toward the last position the network gave it.
    /// Called once per frame, unlike the snapshots that feed it.
    void interpolateRemotes(SceneManager& sm, int32_t dt12);

    net::NetLink m_link;
    State m_state = State::Disconnected;
    bool m_isHost = false;
    uint8_t m_localSlot = net::c_noSlot;
    uint8_t m_hostSlot = net::c_hostSlot;
    uint8_t m_playerCount = 1;
    uint32_t m_sceneHash = 0;
    // Default true so upgrading changes no existing game's behaviour.
    bool m_replicationEnabled = true;
    /// Snapshots discarded because they used a format this build does not know.
    /// Non-zero means the peer is newer than this console; the only safe response
    /// is to drop them, since guessing a layout drives every actor somewhere wrong.
    uint32_t m_snapshotFormatUnknown = 0;
    // Applied-snapshot rate, published once a second like NetLink's goodput. A
    // RATE, not a total: totals hide exactly the irregularity being hunted here.
    uint32_t m_snapsPerSec = 0;
    uint32_t m_snapsThisWindow = 0;
    int32_t m_snapWindowDt = 0;
    uint32_t m_tiebreak = 0;
    int32_t m_helloTimer = 0;      // raw dt units until the next Hello
    int32_t m_snapshotTimer = 0;   // raw dt units until the next snapshot

    uint16_t m_localAvatarActor = 0;  // PLAYER_ACTOR_ID
    uint16_t m_remoteAvatarActor[c_maxSlots];  // 0xFFFF = none

    // --- remote avatar smoothing ---
    //
    // Snapshots land every other frame at best, and with gaps whenever the link
    // drops one. Applying them straight to the actor makes every other player
    // teleport in visible steps. Instead the snapshot sets a TARGET here and the
    // actor eases toward it once per frame, which turns a dropped packet into a
    // slightly late arrival rather than a stutter.
    //
    // Presentation only: the target is still exactly what the network said, so
    // this changes how a remote avatar looks, never where it authoritatively is.
    /// One remote avatar's interpolation state.
    ///
    /// TWO endpoints and a clock, not one target. Easing toward the latest
    /// received position converges on it and then STOPS until the next snapshot
    /// arrives - so at a 15Hz cadence a remote player moves for a few frames,
    /// sits still for the rest of the 67ms, then jerks forward again. Measured on
    /// hardware as "other players' movement is choppy" while the local player,
    /// driven by input every frame and never stalling, looked perfect. The
    /// asymmetry was the clue: nothing was wrong with the link, the remote motion
    /// simply had nothing to do between packets.
    ///
    /// Interpolating BETWEEN the previous and current positions over the measured
    /// interval keeps the avatar moving continuously for the whole gap. It costs
    /// one snapshot interval of latency, which is the standard trade every
    /// networked game makes and is invisible next to the stutter it removes.
    struct AvatarTarget {
        // fp12 raw components, exactly as the wire carries them - this header
        // deliberately keeps psyqo types out.
        int32_t px, py, pz;      // where the newest snapshot put this avatar
        int32_t prevPx, prevPy, prevPz;  // where it was when interpolation started
        int32_t elapsedDt;       // time since the newest snapshot, 4.12
        int32_t intervalDt;      // SMOOTHED gap between snapshots - see below
        uint8_t intervalSamples; // 0 until a gap has actually been measured
        bool active;             // false until the first snapshot for this slot
    };
    AvatarTarget m_avatarTarget[c_maxSlots];

    /// Fallback interval before two snapshots have been seen for a slot, and the
    /// bounds a measured interval is held to.
    ///
    /// Measured rather than assumed, because the server's cadence is not the
    /// console's business: it is 30Hz to an emulator and 15Hz to a serial peer
    /// today, and Phase 4 may make it adaptive. Interpolating over an assumed
    /// interval would run visibly fast or slow the moment that assumption moved.
    static constexpr int32_t c_lerpDefaultIntervalDt = 2 * 4096;   // 15Hz
    static constexpr int32_t c_lerpMinIntervalDt = 4096 / 4;       // sanity floor
    static constexpr int32_t c_lerpMaxIntervalDt = 12 * 4096;      // ~2.5Hz

    /// How hard to smooth the measured interval: new = old + (sample - old) / N.
    ///
    /// SMOOTHING IS NOT OPTIONAL HERE, and this is the subtle part. Time only
    /// advances inside a frame, so a single measurement is quantised to whole
    /// frames - and on the target that matters the frame rate is CLOSE TO the
    /// snapshot rate, which is the worst case for that.
    ///
    /// Measured on hardware: ~19fps (52ms frames) receiving 15Hz snapshots (67ms).
    /// A single sample can only ever read 52ms or 105ms, never 67. So the playback
    /// speed alternated between 1.6x too fast (arrive early, then freeze) and 0.6x
    /// too slow (still walking when the next packet lands) - the interpolator was
    /// manufacturing exactly the stutter it exists to remove, and it did so ONLY on
    /// hardware, because an emulator runs far enough above the snapshot rate for
    /// one sample to be roughly right.
    ///
    /// Averaging over several gaps converges on the true cadence, because the
    /// quantisation error alternates sign. 4 is fast enough to follow a real
    /// cadence change within a few snapshots.
    static constexpr int32_t c_lerpIntervalSmoothing = 4;

    /// Fraction of the remaining distance covered per frame, as a right shift:
    /// 2 means a quarter of the gap each frame. Fast enough to keep up with a
    /// running player, slow enough to hide the snapshot cadence.

    /// Past this far from the target (in fp12 world units) we snap instead of
    /// sliding: a vent hop or a respawn is a teleport, and easing it would drag
    /// the avatar across the whole map.
    static constexpr int32_t c_lerpSnapDistance = 48 << 12;

    // Shared world objects synced by actorId (host-authoritative).
    static constexpr uint32_t c_maxNetActors = 48;
    uint16_t m_netActors[c_maxNetActors];
    uint32_t m_netActorCount = 0;

    // Received reliable game events, drained to Lua (onNetEvent) in preTick.
    static constexpr uint32_t c_eventQueueLen = 8;
    struct PendingGameEvent {
        int32_t id;
        int32_t arg;
    };
    PendingGameEvent m_recvEvents[c_eventQueueLen];
    uint32_t m_recvEventHead = 0;
    uint32_t m_recvEventCount = 0;

    // Received per-object state blobs, applied to Lua (self.sync) in preTick.
    static constexpr uint32_t c_objStateQueueLen = 4;
    static constexpr uint16_t c_maxObjStateBlob = 256;
    struct PendingObjState {
        uint16_t actorId;
        uint16_t len;
        uint8_t data[c_maxObjStateBlob];
    };
    PendingObjState m_recvObjStates[c_objStateQueueLen];
    uint32_t m_objStateHead = 0;
    uint32_t m_objStateCount = 0;

    // Received AppData blobs, drained to Lua (onNetData) in preTick.
    static constexpr uint32_t c_appDataQueueLen = 4;
    struct PendingAppData {
        uint16_t len;
        uint8_t data[net::c_maxEventPayload];
    };
    PendingAppData m_recvAppData[c_appDataQueueLen];
    uint32_t m_appDataHead = 0;
    uint32_t m_appDataCount = 0;

    bool m_persistent = false;

    // Latest received snapshot, staged for application in preTick.
    bool m_hasPendingSnapshot = false;
    uint16_t m_pendingLen = 0;
    uint8_t m_pendingSlot = net::c_noSlot;
    uint8_t m_pendingSnapshot[net::c_maxPayload];

    // Cadences in REAL TIME, as 4.12 fixed point where 4096 == one 30Hz frame
    // (the units of SceneManager's measured m_dt12).
    //
    // These used to be frame counts, and that made a frame-rate drop CAUSE a
    // desync rather than merely accompany one: at 20fps a console broadcast its
    // position at 10Hz instead of 30 and retransmitted three times slower, so
    // one player's stutter made every other player see them lag - which is
    // exactly the "everyone is really desynced" report. Wall-clock cadences hold
    // the network steady while the renderer struggles.
    // c_snapshotCongestionLimit is GONE, deliberately. It was a threshold (a
    // quarter of the TX ring) above which a snapshot was dropped rather than
    // queued, and it was the third such threshold tried against the same problem:
    // stale position data accumulating ahead of acknowledgements and reliable messages.
    //
    // Each one worked until the threshold itself was wrong for the link in front
    // of it, and each was invisible when wrong. NetLink::sendLatest removes the
    // problem instead of guarding it - one overwritable slot, emitted last - so
    // there is no longer a number here to get wrong.
    static constexpr int32_t c_snapshotIntervalDt = 4096;        // 30 Hz
    static constexpr int32_t c_helloIntervalDt = 4096 * 10;      // ~3 Hz
};

}  // namespace psxsplash
