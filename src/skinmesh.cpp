#include "skinmesh.hh"

#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/kernel.hh>
#include <psyqo/soft-math.hh>

#include "gtemath.hh"

#include "streq.hh" // needed for streq() in SkinMesh_FindBoneByName

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace psyqo::GTE;

namespace psxsplash {

    struct BindPositionCacheEntry {
        const SkinAnimSet* animSet = nullptr; // nullptr = empty slot
        psyqo::Vec3 positions[SKINMESH_MAX_BONES];
        bool valid[SKINMESH_MAX_BONES] = {};
    };

// ============================================================================
// Animation tick
// ============================================================================

void SkinMesh_Tick(SkinAnimState* state, lua_State* L, int32_t dt12) {
    if (!state->playing) return;

    const SkinAnimClip& clip = state->animSet->clips[state->currentClip];
    // advance = dt12 * fps / 30
    uint32_t advance = ((uint32_t)dt12 * (uint32_t)clip.fps) / 30u;

    uint32_t accum = (uint32_t)state->subFrame + advance;

    uint16_t wholeFrames = (uint16_t)(accum >> 12);
    state->subFrame = (uint16_t)(accum & 0xFFF);
    state->currentFrame += wholeFrames;

    if (state->currentFrame >= clip.frameCount) {
        if (state->loop || (clip.flags & 0x01)) {
            // Looping — wrap
            state->currentFrame = state->currentFrame % clip.frameCount;
        } else {
            // Stop at last frame
            state->currentFrame = clip.frameCount - 1;
            state->subFrame = 0;
            state->playing = false;

            // Fire Lua callback if registered
            if (state->luaCallbackRef != LUA_NOREF && L) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, state->luaCallbackRef);
                lua_pcall(L, 0, 0, 0);
                luaL_unref(L, LUA_REGISTRYINDEX, state->luaCallbackRef);
                state->luaCallbackRef = LUA_NOREF;
            }
            return;
        }
    }
}

    // Rotates a vector by a 3x3 matrix via the GTE — same sequence
    // computeCameraViewPos()/setupObjectTransform() in renderer.cpp already use
    // for this kind of transform. Local to this file; not a hot-path function.
    static psyqo::Vec3 rotateVectorGTE(const psyqo::Matrix33& mat, const psyqo::Vec3& v) {
        using namespace psyqo::GTE;
        clear<Register::TRX, Safe>();
        clear<Register::TRY, Safe>();
        clear<Register::TRZ, Safe>();
        writeSafe<PseudoRegister::Rotation>(mat);
        writeSafe<PseudoRegister::V0>(v);
        Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
        return readSafe<PseudoRegister::SV>();
    }

    static BindPositionCacheEntry s_bindCache[MAX_SKINNED_MESHES];

    static BindPositionCacheEntry* findOrBuildBindCache(const SkinAnimSet& animSet) {
        for (auto& entry : s_bindCache) {
            if (entry.animSet == &animSet) return &entry; // already built
        }

        BindPositionCacheEntry* slot = nullptr;
        for (auto& entry : s_bindCache) {
            if (entry.animSet == nullptr) { slot = &entry; break; }
        }
        if (!slot) return nullptr; // shouldn't happen: capped at MAX_SKINNED_MESHES, same bound as animSet count

        slot->animSet = &animSet;
        for (int b = 0; b < SKINMESH_MAX_BONES; b++) slot->valid[b] = false;

        if (!animSet.polygons || !animSet.boneIndices || animSet.boneCount == 0) return slot;

        int32_t sumX[SKINMESH_MAX_BONES] = {};
        int32_t sumY[SKINMESH_MAX_BONES] = {};
        int32_t sumZ[SKINMESH_MAX_BONES] = {};
        int32_t count[SKINMESH_MAX_BONES] = {};

        for (uint16_t ti = 0; ti < animSet.polyCount; ti++) {
            const Tri& tri = animSet.polygons[ti];
            const psyqo::GTE::PackedVec3* verts[3] = { &tri.v0, &tri.v1, &tri.v2 };
            for (int c = 0; c < 3; c++) {
                uint8_t bone = animSet.boneIndices[ti * 3 + c];
                if (bone >= animSet.boneCount || bone >= SKINMESH_MAX_BONES) continue;
                sumX[bone] += verts[c]->x.value;
                sumY[bone] += verts[c]->y.value;
                sumZ[bone] += verts[c]->z.value;
                count[bone]++;
            }
        }

        for (int b = 0; b < animSet.boneCount && b < SKINMESH_MAX_BONES; b++) {
            if (count[b] > 0) {
                slot->positions[b].x.value = sumX[b] / count[b];
                slot->positions[b].y.value = sumY[b] / count[b];
                slot->positions[b].z.value = sumZ[b] / count[b];
                slot->valid[b] = true;
            }
            // count[b] == 0: no vertex is weighted to this bone — valid[b] stays
            // false. Root/control bones sometimes have no directly-skinned
            // vertices; SkinMesh_GetBoneWorldPosition() returns false for these.
        }
        return slot;
    }

    bool SkinMesh_GetApproxBoneBindPosition(const SkinAnimSet& animSet, uint8_t boneIndex,
        psyqo::Vec3& outPos) {
        if (boneIndex >= animSet.boneCount || boneIndex >= SKINMESH_MAX_BONES) return false;
        BindPositionCacheEntry* entry = findOrBuildBindCache(animSet);
        if (!entry || !entry->valid[boneIndex]) return false;
        outPos = entry->positions[boneIndex];
        return true;
    }

    void SkinMesh_PrecomputeApproxBindPositions(const SkinAnimSet& animSet) {
        findOrBuildBindCache(animSet); // builds + caches; discard the result, this call is just for the side effect
    }

    void SkinMesh_ClearBindPositionCache() {
        for (auto& entry : s_bindCache) entry.animSet = nullptr;
    }

    // ============================================================================
    // Bone name lookup — insert until a future splashpack change populates
    // animSet.boneNames. Kept here so Entity.GetBonePosition's string-name
    // path (already shipped in luaapi.cpp) has something to call rather than
    // needing its own removal/re-add later.
    // ============================================================================

    int SkinMesh_FindBoneByName(const SkinAnimSet& animSet, const char* name) {
        if (!animSet.boneNames || !name) return -1;
        for (uint8_t i = 0; i < animSet.boneCount; i++) {
            if (animSet.boneNames[i] && streq(animSet.boneNames[i], name)) return (int)i;
        }
        return -1;
    }

    // ============================================================================
    // Bone world-position query
    // ============================================================================

    bool SkinMesh_GetBoneWorldPosition(const SkinAnimSet& animSet, const SkinAnimState& animState,
        const GameObject& obj, uint8_t boneIndex,
        psyqo::Vec3& outPos) {
        if (boneIndex >= animSet.boneCount || boneIndex >= SKINMESH_MAX_BONES) return false;

        uint8_t clipIdx = animState.currentClip;
        if (clipIdx >= animSet.clipCount) return false;
        const SkinAnimClip& clip = animSet.clips[clipIdx];
        if (!clip.frames || clip.frameCount == 0) return false;

        psyqo::Vec3 bindPos;
        if (!SkinMesh_GetApproxBoneBindPosition(animSet, boneIndex, bindPos)) return false;

        uint16_t frame = animState.currentFrame;
        if (frame >= clip.frameCount) frame = clip.frameCount - 1;

        const BakedBoneMatrix* boneMatricesA = &clip.frames[(uint32_t)frame * animSet.boneCount];
        const BakedBoneMatrix* bm = &boneMatricesA[boneIndex];

        static BakedBoneMatrix lerped;
        uint16_t sf = animState.subFrame;
        if (sf > 0 && frame + 1 < clip.frameCount) {
            const BakedBoneMatrix& bA = boneMatricesA[boneIndex];
            const BakedBoneMatrix& bB = clip.frames[(uint32_t)(frame + 1) * animSet.boneCount + boneIndex];
            for (int k = 0; k < 9; k++) {
                int32_t a = bA.r[k], b = bB.r[k];
                lerped.r[k] = (int16_t)(a + (((b - a) * sf) >> 12));
            }
            for (int k = 0; k < 3; k++) {
                int32_t a = bA.t[k], b = bB.t[k];
                lerped.t[k] = (int16_t)(a + (((b - a) * sf) >> 12));
            }
            bm = &lerped;
        }
        else if (sf > 0 && (animState.loop || (clip.flags & 0x01)) && clip.frameCount > 1) {
            const BakedBoneMatrix& bA = boneMatricesA[boneIndex];
            const BakedBoneMatrix& bB = clip.frames[boneIndex]; // frame 0
            for (int k = 0; k < 9; k++) {
                int32_t a = bA.r[k], b = bB.r[k];
                lerped.r[k] = (int16_t)(a + (((b - a) * sf) >> 12));
            }
            for (int k = 0; k < 3; k++) {
                int32_t a = bA.t[k], b = bB.t[k];
                lerped.t[k] = (int16_t)(a + (((b - a) * sf) >> 12));
            }
            bm = &lerped;
        }

        psyqo::Matrix33 boneRot;
        boneRot.vs[0].x.value = bm->r[0]; boneRot.vs[0].y.value = bm->r[1]; boneRot.vs[0].z.value = bm->r[2];
        boneRot.vs[1].x.value = bm->r[3]; boneRot.vs[1].y.value = bm->r[4]; boneRot.vs[1].z.value = bm->r[5];
        boneRot.vs[2].x.value = bm->r[6]; boneRot.vs[2].y.value = bm->r[7]; boneRot.vs[2].z.value = bm->r[8];

        psyqo::Vec3 skinnedPos = rotateVectorGTE(boneRot, bindPos);
        skinnedPos.x += psyqo::FixedPoint<12>(bm->t[0], psyqo::FixedPoint<12>::RAW);
        skinnedPos.y += psyqo::FixedPoint<12>(bm->t[1], psyqo::FixedPoint<12>::RAW);
        skinnedPos.z += psyqo::FixedPoint<12>(bm->t[2], psyqo::FixedPoint<12>::RAW);

        psyqo::Vec3 rotated = rotateVectorGTE(obj.rotation, skinnedPos);
        outPos.x = obj.position.x + rotated.x;
        outPos.y = obj.position.y + rotated.y;
        outPos.z = obj.position.z + rotated.z;
        return true;
    }

}  // namespace psxsplash
