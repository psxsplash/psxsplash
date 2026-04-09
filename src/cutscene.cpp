#include "cutscene.hh"
#include "interpolation.hh"

#include <psyqo/fixed-point.hh>
#include <psyqo/soft-math.hh>
#include <psyqo/trigonometry.hh>
#include "gtemath.hh"
#include "streq.hh"
#include "uisystem.hh"
#include "scenemanager.hh"
#include "skinmesh.hh"
#include "controls.hh"

namespace psxsplash {

void CutscenePlayer::init(Cutscene* cutscenes, int count, Camera* camera, AudioManager* audio,
                          UISystem* uiSystem, SceneManager* sceneMgr, Controls* controls) {
    m_cutscenes     = cutscenes;
    m_count         = count;
    m_active        = nullptr;
    m_frame         = 0;
    m_subFrame      = 0;
    m_nextAudio     = 0;
    m_nextSkinAnim  = 0;
    m_loop          = false;
    m_camera        = camera;
    m_audio         = audio;
    m_uiSystem      = uiSystem;
    m_sceneMgr      = sceneMgr;
    m_controls      = controls;
    m_onCompleteRef = LUA_NOREF;
}

bool CutscenePlayer::play(const char* name, bool loop) {
    if (!name || !m_cutscenes) return false;
    m_loop = loop;

    for (int i = 0; i < m_count; i++) {
        if (m_cutscenes[i].name && streq(m_cutscenes[i].name, name)) {
            m_active    = &m_cutscenes[i];
            m_frame     = 0;
            m_subFrame  = 0;
            m_nextAudio = 0;
            m_nextSkinAnim = 0;

            // Capture initial state for pre-first-keyframe blending
            for (uint8_t ti = 0; ti < m_active->trackCount; ti++) {
                CutsceneTrack& track = m_active->tracks[ti];
                track.initialValues[0] = track.initialValues[1] = track.initialValues[2] = 0;
                switch (track.trackType) {
                    case TrackType::CameraPosition:
                        if (m_camera) {
                            auto& pos = m_camera->GetPosition();
                            track.initialValues[0] = (int16_t)pos.x.value;
                            track.initialValues[1] = (int16_t)pos.y.value;
                            track.initialValues[2] = (int16_t)pos.z.value;
                        }
                        break;
                    case TrackType::CameraRotation:
                        if (m_camera) {
                            track.initialValues[0] = m_camera->GetAngleX();
                            track.initialValues[1] = m_camera->GetAngleY();
                            track.initialValues[2] = m_camera->GetAngleZ();
                        }
                        break;
                    case TrackType::CameraH:
                        if (m_camera) {
                            track.initialValues[0] = (int16_t)m_camera->GetProjectionH();
                        }
                        break;
                    case TrackType::ObjectPosition:
                        if (track.target) {
                            track.initialValues[0] = (int16_t)track.target->position.x.value;
                            track.initialValues[1] = (int16_t)track.target->position.y.value;
                            track.initialValues[2] = (int16_t)track.target->position.z.value;
                        }
                        break;
                    case TrackType::ObjectRotation:
                        // Initial rotation angles: 0,0,0 (no way to extract Euler from matrix)
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
                }
            }

            return true;
        }
    }
    return false;
}

void CutscenePlayer::stop() {
    if (!m_active) return;
    // Stop vibration motors when cutscene stops
    if (m_controls) {
        m_controls->stopMotors();
    }
    m_active = nullptr;
    fireOnComplete();
}

bool CutscenePlayer::hasCameraTracks() const {
    if (!m_active) return false;
    for (uint8_t i = 0; i < m_active->trackCount; i++) {
        auto t = m_active->tracks[i].trackType;
        if (t == TrackType::CameraPosition || t == TrackType::CameraRotation || t == TrackType::CameraH)
            return true;
    }
    return false;
}

void CutscenePlayer::tick(int32_t dt12) {
    if (!m_active) return;

    for (uint8_t i = 0; i < m_active->trackCount; i++) {
        applyTrack(m_active->tracks[i]);
    }

    while (m_nextAudio < m_active->audioEventCount) {
        CutsceneAudioEvent& evt = m_active->audioEvents[m_nextAudio];
        if (evt.frame <= m_frame) {
            if (m_audio) {
                m_audio->play(evt.clipIndex, evt.volume, evt.pan);
            }
            m_nextAudio++;
        } else {
            break;
        }
    }

    // Process skin animation events
    while (m_nextSkinAnim < m_active->skinAnimEventCount) {
        CutsceneSkinAnimEvent& evt = m_active->skinAnimEvents[m_nextSkinAnim];
        if (evt.frame <= m_frame) {
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
            m_nextSkinAnim++;
        } else {
            break;
        }
    }

    // Advance frame using dt12
    uint32_t accum = (uint32_t)m_subFrame + (uint32_t)dt12;
    uint16_t wholeFrames = (uint16_t)(accum >> 12);
    m_subFrame = (uint16_t)(accum & 0xFFF);
    m_frame += wholeFrames;

    if (m_frame > m_active->totalFrames) {
        if (m_loop) {
            m_frame = 0;
            m_subFrame = 0;
            m_nextAudio = 0;
            m_nextSkinAnim = 0;
        } else {
            // Stop vibration when cutscene ends naturally
            if (m_controls) {
                m_controls->stopMotors();
            }
            m_active = nullptr;
            fireOnComplete();
        }
    }
}

void CutscenePlayer::fireOnComplete() {
    if (m_onCompleteRef == LUA_NOREF || !m_luaState) return;
    psyqo::Lua L(m_luaState);
    L.rawGetI(LUA_REGISTRYINDEX, m_onCompleteRef);
    if (L.isFunction(-1)) {
        if (L.pcall(0, 0) != LUA_OK) {
            L.pop();
        }
    } else {
        L.pop();
    }
    // Unreference the callback (one-shot)
    luaL_unref(m_luaState, LUA_REGISTRYINDEX, m_onCompleteRef);
    m_onCompleteRef = LUA_NOREF;
}

void CutscenePlayer::applyTrack(CutsceneTrack& track) {
    if (track.keyframeCount == 0 || !track.keyframes) return;

    int16_t out[3];

    switch (track.trackType) {
        case TrackType::CameraPosition: {
            if (!m_camera) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, m_frame, m_subFrame, track.initialValues, out);
            psyqo::FixedPoint<12> x, y, z;
            x.value = (int32_t)out[0];
            y.value = (int32_t)out[1];
            z.value = (int32_t)out[2];
            m_camera->SetPosition(x, y, z);
            break;
        }

        case TrackType::CameraRotation: {
            if (!m_camera) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, m_frame, m_subFrame, track.initialValues, out);
            psyqo::Angle rx, ry, rz;
            rx.value = (int32_t)out[0];
            ry.value = (int32_t)out[1];
            rz.value = (int32_t)out[2];
            m_camera->SetRotation(rx, ry, rz);
            break;
        }

        case TrackType::CameraH: {
            if (!m_camera) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, m_frame, m_subFrame, track.initialValues, out);
            int32_t h = (int32_t)out[0];
            if (h < 1) h = 1;           // Avoid zero/negative — GTE would divide-by-zero
            if (h > 1024) h = 1024;     // Practical upper limit (~13° vFOV)
            m_camera->SetProjectionH(h);
            break;
        }

        case TrackType::ObjectPosition: {
            if (!track.target) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, m_frame, m_subFrame, track.initialValues, out);
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
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, m_frame, m_subFrame, track.initialValues, out);
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
            int16_t activeVal = (count > 0 && m_frame < kf[0].getFrame())
                ? track.initialValues[0]
                : kf[0].values[0];
            for (uint8_t i = 0; i < count; i++) {
                if (kf[i].getFrame() <= m_frame) {
                    activeVal = kf[i].values[0];
                } else {
                    break;
                }
            }
            track.target->setActive(activeVal != 0);
            break;
        }

        // ── UI track types ──

        case TrackType::UICanvasVisible: {
            if (!m_uiSystem) return;
            CutsceneKeyframe* kf = track.keyframes;
            uint8_t count = track.keyframeCount;
            int16_t val = (count > 0 && m_frame < kf[0].getFrame())
                ? track.initialValues[0] : kf[0].values[0];
            for (uint8_t i = 0; i < count; i++) {
                if (kf[i].getFrame() <= m_frame) val = kf[i].values[0];
                else break;
            }
            m_uiSystem->setCanvasVisible(track.uiHandle, val != 0);
            break;
        }

        case TrackType::UIElementVisible: {
            if (!m_uiSystem) return;
            CutsceneKeyframe* kf = track.keyframes;
            uint8_t count = track.keyframeCount;
            int16_t val = (count > 0 && m_frame < kf[0].getFrame())
                ? track.initialValues[0] : kf[0].values[0];
            for (uint8_t i = 0; i < count; i++) {
                if (kf[i].getFrame() <= m_frame) val = kf[i].values[0];
                else break;
            }
            m_uiSystem->setElementVisible(track.uiHandle, val != 0);
            break;
        }

        case TrackType::UIProgress: {
            if (!m_uiSystem) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, m_frame, m_subFrame, track.initialValues, out);
            int16_t v = out[0];
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            m_uiSystem->setProgress(track.uiHandle, (uint8_t)v);
            break;
        }

        case TrackType::UIPosition: {
            if (!m_uiSystem) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, m_frame, m_subFrame, track.initialValues, out);
            m_uiSystem->setPosition(track.uiHandle, out[0], out[1]);
            break;
        }

        case TrackType::UIColor: {
            if (!m_uiSystem) return;
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, m_frame, m_subFrame, track.initialValues, out);
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
            int16_t val = (count > 0 && m_frame < kf[0].getFrame())
                ? track.initialValues[0] : kf[0].values[0];
            for (uint8_t i = 0; i < count; i++) {
                if (kf[i].getFrame() <= m_frame) val = kf[i].values[0];
                else break;
            }
            m_controls->setSmallMotor((uint8_t)(val != 0 ? 1 : 0));
            break;
        }

        case TrackType::RumbleLarge: {
            if (!m_controls) return;
            // Interpolated: 0-255 motor speed
            psxsplash::lerpKeyframesSub(track.keyframes, track.keyframeCount, m_frame, m_subFrame, track.initialValues, out);
            int16_t v = out[0];
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            m_controls->setLargeMotor((uint8_t)v);
            break;
        }
    }
}

}  // namespace psxsplash
