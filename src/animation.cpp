#include "animation.hh"
#include "interpolation.hh"

#include <psyqo/fixed-point.hh>
#include <psyqo/soft-math.hh>
#include "gtemath.hh"
#include "streq.hh"
#include "uisystem.hh"
#include "scenemanager.hh"
#include "skinmesh.hh"
#include "controls.hh"

namespace psxsplash {

void AnimationPlayer::init(Animation* animations, int count, UISystem* uiSystem,
                           SceneManager* sceneMgr, Controls* controls) {
    m_animations = animations;
    m_animCount  = count;
    m_uiSystem   = uiSystem;
    m_sceneMgr   = sceneMgr;
    m_controls   = controls;
    for (int i = 0; i < MAX_SIMULTANEOUS_ANIMS; i++) {
        m_slots[i].anim = nullptr;
        m_slots[i].onCompleteRef = LUA_NOREF;
    }
}

Animation* AnimationPlayer::findByName(const char* name) const {
    if (!name || !m_animations) return nullptr;
    for (int i = 0; i < m_animCount; i++) {
        if (m_animations[i].name && streq(m_animations[i].name, name))
            return &m_animations[i];
    }
    return nullptr;
}

bool AnimationPlayer::play(const char* name, bool loop) {
    Animation* anim = findByName(name);
    if (!anim) return false;

    // Find a free slot
    int freeSlot = -1;
    for (int i = 0; i < MAX_SIMULTANEOUS_ANIMS; i++) {
        if (!m_slots[i].anim) {
            freeSlot = i;
            break;
        }
    }
    if (freeSlot < 0) return false;

    ActiveSlot& slot = m_slots[freeSlot];
    slot.anim  = anim;
    slot.frame = 0;
    slot.subFrame = 0;
    slot.loop  = loop;
    slot.nextSkinAnim = 0;
    // onCompleteRef is set separately via setOnCompleteRef before play()

    captureInitialValues(anim);
    return true;
}

void AnimationPlayer::stop(const char* name) {
    if (!name) return;
    for (int i = 0; i < MAX_SIMULTANEOUS_ANIMS; i++) {
        if (m_slots[i].anim && m_slots[i].anim->name && streq(m_slots[i].anim->name, name)) {
            fireSlotComplete(m_slots[i]);
            m_slots[i].anim = nullptr;
        }
    }
    // Stop vibration in case any stopped animation was driving motors
    if (m_controls) m_controls->stopMotors();
}

void AnimationPlayer::stopAll() {
    for (int i = 0; i < MAX_SIMULTANEOUS_ANIMS; i++) {
        if (m_slots[i].anim) {
            fireSlotComplete(m_slots[i]);
            m_slots[i].anim = nullptr;
        }
    }
    // Stop vibration motors
    if (m_controls) m_controls->stopMotors();
}

bool AnimationPlayer::isPlaying(const char* name) const {
    if (!name) return false;
    for (int i = 0; i < MAX_SIMULTANEOUS_ANIMS; i++) {
        if (m_slots[i].anim && m_slots[i].anim->name && streq(m_slots[i].anim->name, name))
            return true;
    }
    return false;
}

void AnimationPlayer::setOnCompleteRef(const char* name, int ref) {
    // Find the most recently started slot for this animation (highest index with frame 0)
    // Fallback: find first slot with this name
    for (int i = MAX_SIMULTANEOUS_ANIMS - 1; i >= 0; i--) {
        if (m_slots[i].anim && m_slots[i].anim->name && streq(m_slots[i].anim->name, name)) {
            m_slots[i].onCompleteRef = ref;
            return;
        }
    }
}

void AnimationPlayer::tick(int32_t dt12) {
    for (int i = 0; i < MAX_SIMULTANEOUS_ANIMS; i++) {
        ActiveSlot& slot = m_slots[i];
        if (!slot.anim) continue;

        // Apply all tracks
        for (uint8_t ti = 0; ti < slot.anim->trackCount; ti++) {
            applyTrack(slot.anim->tracks[ti], slot.frame, slot.subFrame);
        }

        // Process skin animation events
        while (slot.nextSkinAnim < slot.anim->skinAnimEventCount) {
            CutsceneSkinAnimEvent& evt = slot.anim->skinAnimEvents[slot.nextSkinAnim];
            if (evt.frame <= slot.frame) {
                if (m_sceneMgr) {
                    int skinIdx = (int)evt.skinMeshIndex;
                    if (skinIdx < m_sceneMgr->getSkinnedMeshCount()) {
                        SkinAnimState& state = m_sceneMgr->getSkinAnimState(skinIdx);
                        state.currentClip  = evt.clipIndex;
                        state.currentFrame = 0;
                        state.subFrame     = 0;
                        state.playing      = true;
                        state.loop         = (evt.loop != 0);
                    }
                }
                slot.nextSkinAnim++;
            } else {
                break;
            }
        }

        // Advance frame using dt12
        uint32_t accum = (uint32_t)slot.subFrame + (uint32_t)dt12;
        uint16_t wholeFrames = (uint16_t)(accum >> 12);
        slot.subFrame = (uint16_t)(accum & 0xFFF);
        slot.frame += wholeFrames;

        if (slot.frame > slot.anim->totalFrames) {
            if (slot.loop) {
                slot.frame = 0;
                slot.subFrame = 0;
                slot.nextSkinAnim = 0;
            } else {
                Animation* finished = slot.anim;
                slot.anim = nullptr;
                // Stop vibration when animation ends naturally
                if (m_controls) m_controls->stopMotors();
                fireSlotComplete(slot);
                (void)finished;
            }
        }
    }
}

void AnimationPlayer::captureInitialValues(Animation* anim) {
    for (uint8_t ti = 0; ti < anim->trackCount; ti++) {
        CutsceneTrack& track = anim->tracks[ti];
        track.initialValues[0] = track.initialValues[1] = track.initialValues[2] = 0;
        switch (track.trackType) {
            case TrackType::ObjectPosition:
                if (track.target) {
                    track.initialValues[0] = (int16_t)track.target->position.x.value;
                    track.initialValues[1] = (int16_t)track.target->position.y.value;
                    track.initialValues[2] = (int16_t)track.target->position.z.value;
                }
                break;
            case TrackType::ObjectRotation:
                break;
            case TrackType::ObjectActive:
                if (track.target) {
                    track.initialValues[0] = track.target->isActive() ? 1 : 0;
                }
                break;
            case TrackType::UICanvasVisible:
                if (m_uiSystem) {
                    track.initialValues[0] = m_uiSystem->isCanvasVisible(track.uiHandle) ? 1 : 0;
                }
                break;
            case TrackType::UIElementVisible:
                if (m_uiSystem) {
                    track.initialValues[0] = m_uiSystem->isElementVisible(track.uiHandle) ? 1 : 0;
                }
                break;
            case TrackType::UIProgress:
                if (m_uiSystem) {
                    track.initialValues[0] = m_uiSystem->getProgress(track.uiHandle);
                }
                break;
            case TrackType::UIPosition:
                if (m_uiSystem) {
                    int16_t px, py;
                    m_uiSystem->getPosition(track.uiHandle, px, py);
                    track.initialValues[0] = px;
                    track.initialValues[1] = py;
                }
                break;
            case TrackType::UIColor:
                if (m_uiSystem) {
                    uint8_t cr, cg, cb;
                    m_uiSystem->getColor(track.uiHandle, cr, cg, cb);
                    track.initialValues[0] = cr;
                    track.initialValues[1] = cg;
                    track.initialValues[2] = cb;
                }
                break;
            case TrackType::RumbleSmall:
            case TrackType::RumbleLarge:
                // Motors always start at 0 (off)
                track.initialValues[0] = 0;
                break;
            default:
                break;
        }
    }
}

void AnimationPlayer::applyTrack(CutsceneTrack& track, uint16_t frame, uint16_t subFrame) {
    if (track.keyframeCount == 0 || !track.keyframes) return;

    int16_t out[3];

    switch (track.trackType) {
        case TrackType::ObjectPosition: {
            if (!track.target) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, frame, subFrame, track.initialValues, out);
            // Compute delta and shift AABB for frustum culling
            int32_t dx = (int32_t)out[0] - track.target->position.x.value;
            int32_t dy = (int32_t)out[1] - track.target->position.y.value;
            int32_t dz = (int32_t)out[2] - track.target->position.z.value;
            track.target->position.x.value = (int32_t)out[0];
            track.target->position.y.value = (int32_t)out[1];
            track.target->position.z.value = (int32_t)out[2];
            track.target->aabbMinX += dx; track.target->aabbMaxX += dx;
            track.target->aabbMinY += dy; track.target->aabbMaxY += dy;
            track.target->aabbMinZ += dz; track.target->aabbMaxZ += dz;
            track.target->setDynamicMoved(true);
            break;
        }

        case TrackType::ObjectRotation: {
            if (!track.target) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, frame, subFrame, track.initialValues, out);
            psyqo::Angle rx, ry, rz;
            rx.value = (int32_t)out[0];
            ry.value = (int32_t)out[1];
            rz.value = (int32_t)out[2];
            auto matY = psyqo::SoftMath::generateRotationMatrix33(ry, psyqo::SoftMath::Axis::Y, m_trig);
            auto matX = psyqo::SoftMath::generateRotationMatrix33(rx, psyqo::SoftMath::Axis::X, m_trig);
            auto matZ = psyqo::SoftMath::generateRotationMatrix33(rz, psyqo::SoftMath::Axis::Z, m_trig);
            auto temp = psyqo::SoftMath::multiplyMatrix33(matY, matX);
            track.target->rotation = psxsplash::transposeMatrix33(
                psyqo::SoftMath::multiplyMatrix33(temp, matZ));
            break;
        }

        case TrackType::ObjectActive: {
            if (!track.target) return;
            CutsceneKeyframe* kf = track.keyframes;
            uint8_t count = track.keyframeCount;
            int16_t activeVal = (count > 0 && frame < kf[0].getFrame())
                ? track.initialValues[0]
                : kf[0].values[0];
            for (uint8_t i = 0; i < count; i++) {
                if (kf[i].getFrame() <= frame) {
                    activeVal = kf[i].values[0];
                } else {
                    break;
                }
            }
            track.target->setActive(activeVal != 0);
            break;
        }

        case TrackType::ObjectUVOffset: {
            if (!track.target) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, frame, subFrame, track.initialValues, out);
            uint8_t u = (out[0] < 0) ? 0 : ((out[0] > 255) ? 255 : (uint8_t)out[0]);
            uint8_t v = (out[1] < 0) ? 0 : ((out[1] > 255) ? 255 : (uint8_t)out[1]);

            track.target->uvOffset = { u, v };
            break;
        }

        case TrackType::UICanvasVisible: {
            if (!m_uiSystem) return;
            CutsceneKeyframe* kf = track.keyframes;
            uint8_t count = track.keyframeCount;
            int16_t val = (count > 0 && frame < kf[0].getFrame())
                ? track.initialValues[0] : kf[0].values[0];
            for (uint8_t i = 0; i < count; i++) {
                if (kf[i].getFrame() <= frame) val = kf[i].values[0];
                else break;
            }
            m_uiSystem->setCanvasVisible(track.uiHandle, val != 0);
            break;
        }

        case TrackType::UIElementVisible: {
            if (!m_uiSystem) return;
            CutsceneKeyframe* kf = track.keyframes;
            uint8_t count = track.keyframeCount;
            int16_t val = (count > 0 && frame < kf[0].getFrame())
                ? track.initialValues[0] : kf[0].values[0];
            for (uint8_t i = 0; i < count; i++) {
                if (kf[i].getFrame() <= frame) val = kf[i].values[0];
                else break;
            }
            m_uiSystem->setElementVisible(track.uiHandle, val != 0);
            break;
        }

        case TrackType::UIProgress: {
            if (!m_uiSystem) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, frame, subFrame, track.initialValues, out);
            int16_t v = out[0];
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            m_uiSystem->setProgress(track.uiHandle, (uint8_t)v);
            break;
        }

        case TrackType::UIPosition: {
            if (!m_uiSystem) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, frame, subFrame, track.initialValues, out);
            m_uiSystem->setPosition(track.uiHandle, out[0], out[1]);
            break;
        }

        case TrackType::UIColor: {
            if (!m_uiSystem) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, frame, subFrame, track.initialValues, out);
            uint8_t cr = (out[0] < 0) ? 0 : ((out[0] > 255) ? 255 : (uint8_t)out[0]);
            uint8_t cg = (out[1] < 0) ? 0 : ((out[1] > 255) ? 255 : (uint8_t)out[1]);
            uint8_t cb = (out[2] < 0) ? 0 : ((out[2] > 255) ? 255 : (uint8_t)out[2]);
            m_uiSystem->setColor(track.uiHandle, cr, cg, cb);
            break;
        }

        // ── Vibration track types ──

        case TrackType::RumbleSmall: {
            if (!m_controls) return;
            // Step semantics: on/off, no interpolation
            CutsceneKeyframe* kf = track.keyframes;
            uint8_t count = track.keyframeCount;
            int16_t val = (count > 0 && frame < kf[0].getFrame())
                ? track.initialValues[0] : kf[0].values[0];
            for (uint8_t i = 0; i < count; i++) {
                if (kf[i].getFrame() <= frame) val = kf[i].values[0];
                else break;
            }
            m_controls->setSmallMotor((uint8_t)(val != 0 ? 1 : 0));
            break;
        }

        case TrackType::RumbleLarge: {
            if (!m_controls) return;
            // Interpolated: 0-255 motor speed
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, frame, subFrame, track.initialValues, out);
            int16_t v = out[0];
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            m_controls->setLargeMotor((uint8_t)v);
            break;
        }

        default:
            break;
    }
}

void AnimationPlayer::fireSlotComplete(ActiveSlot& slot) {
    if (slot.onCompleteRef == LUA_NOREF || !m_luaState) return;
    psyqo::Lua L(m_luaState);
    L.rawGetI(LUA_REGISTRYINDEX, slot.onCompleteRef);
    if (L.isFunction(-1)) {
        if (L.pcall(0, 0) != LUA_OK) {
            L.pop();
        }
    } else {
        L.pop();
    }
    luaL_unref(m_luaState, LUA_REGISTRYINDEX, slot.onCompleteRef);
    slot.onCompleteRef = LUA_NOREF;
}

}  // namespace psxsplash
