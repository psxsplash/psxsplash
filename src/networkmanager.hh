#pragma once

#include <stdint.h>

#include "netlink.hh"
#include "netprotocol.hh"
#include "sio1.hh"

namespace psxsplash {

class SceneManager;

/**
 * NetworkManager — session, ownership, and actor-state replication over SIO1.
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

    // Slot 0 is always the authority. In a peer-to-peer link that is one of the
    // two consoles; in the central server's star topology it is the SERVER, which
    // has no avatar of its own — so 10 human players need slots 1..10, i.e. 11
    // slots, not 10. Getting this off by one silently costs a player.
    //
    // 10 players is what the snapshot bandwidth budget was sized against:
    // 10 avatars x 20B + framing = 215B per snapshot, sent every 2 frames
    // against a ~384B budget at 115200 8N1.
    static constexpr uint8_t c_maxSlots = 11;

    static NetworkManager& Get();

    // Start a session over SIO1. `sceneHash` identifies the loaded scene (both
    // ends must match). `seed` seeds the host-election tiebreak. `rxMode`
    // defaults to Auto, which picks the correct strategy for the emulator vs
    // real hardware on its own — see sio1.hh before overriding it.
    void begin(uint32_t sceneHash, uint32_t seed, uint32_t baud = 115200,
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
    void preTick(SceneManager& sm);
    // End of GameTick: at the net-tick cadence, capture owned state and send.
    void postTick(SceneManager& sm);

    // Which local actor is THIS console's player avatar (sent to peers). Default
    // actor 0 (the player).
    void setLocalAvatarActor(uint16_t actorId) { m_localAvatarActor = actorId; }
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
    // Queued reliable packets vs capacity — for app-layer backpressure.
    uint32_t reliableQueueDepth() const { return m_link.reliableQueueDepth(); }

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

    net::NetLink m_link;
    State m_state = State::Disconnected;
    bool m_isHost = false;
    uint8_t m_localSlot = net::c_noSlot;
    uint8_t m_hostSlot = net::c_hostSlot;
    uint8_t m_playerCount = 1;
    uint32_t m_sceneHash = 0;
    uint32_t m_tiebreak = 0;
    uint32_t m_helloTimer = 0;
    uint32_t m_snapshotTimer = 0;

    uint16_t m_localAvatarActor = 0;  // PLAYER_ACTOR_ID
    uint16_t m_remoteAvatarActor[c_maxSlots];  // 0xFFFF = none

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

    static constexpr uint32_t c_snapshotIntervalFrames = 2;  // ~30/15 Hz
    static constexpr uint32_t c_helloIntervalFrames = 20;
};

}  // namespace psxsplash
