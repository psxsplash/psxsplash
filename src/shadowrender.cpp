// Renderer::renderShadows (blob shadows), in its own file so that the
// shadows feature leaves renderer.cpp's code, and what follows it in the
// link, where it is without the feature.
#include "renderer.hh"

#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/primitives/control.hh>

#include "navregion.hh"

using namespace psyqo::GTE;

#if PSXSPLASH_FEATURE_SHADOWS
// Blob shadows (shadowmath.hh has the model). Each blob is a fan of eight
// untextured triangles drawn with the subtract blend. An untextured primitive
// takes its blend mode from the GPU's current texpage (E1), which every
// textured triangle rewrites to additive for the fog pass, so the whole blob
// goes in ONE ordering-table bucket bracketed by two TPage commands: subtract
// before the fan, additive (what the fog pass expects) after it.
//
// The bucket is the centre's depth less kShadowDepthBias: in front of the
// floor under it, so the floor is drawn first, and level with the caster's
// feet.
static constexpr int32_t kShadowDepthBias = 8;

void psxsplash::Renderer::renderShadows(eastl::vector<GameObject*>& objects, const psyqo::Vec3& cameraPosition,
                                        int32_t fogFarSZ, OT& ot, Balloc& balloc) {
    if (m_shadowCount == 0) return;
    const bool nav = m_shadowNav != nullptr && m_shadowNav->isLoaded();
    for (int si = 0; si < m_shadowCount; si++) {
        const ShadowRecord& rec = m_shadows[si];
        if (rec.mode != SHADOW_BLOB || rec.gameObjectIndex >= objects.size()) continue;
        GameObject* obj = objects[rec.gameObjectIndex];
        if (obj == nullptr || !obj->isActive()) continue;

        // The floor: the nav region under the object, else its bounds' bottom
        // (Y points down, so the bottom is aabbMaxY).
        const int32_t cx = obj->position.x.value, cz = obj->position.z.value;
        const int32_t bottom = obj->aabbMaxY;
        const int32_t middle = (obj->aabbMinY + obj->aabbMaxY) / 2;
        int32_t floorY = bottom;
        uint16_t region = NAV_NO_REGION;
        if (nav) {
            region = m_shadowNav->findFloorUnder(cx, cz, middle, bottom, rec.fadeHeight, floorY);
            // Off the nav floor, an object that never moved is a prop standing
            // in the hole it cuts, on the ground; one that moved is in the air
            // or falling, and its blob would float at its feet.
            if (region == NAV_NO_REGION && obj->isDynamicMoved()) continue;
        }
        const int32_t fade = shadowmath::heightFade(floorY - bottom, rec.fadeHeight);
        if (fade == 0) continue;
        int32_t radius = shadowmath::blobRadius(rec.size, fade);
        if (radius < 1) continue;
        if (radius > 0x7FFF) radius = 0x7FFF;
        int32_t edge = 4096;
        if (region != NAV_NO_REGION)
            edge = shadowmath::edgeFade(m_shadowNav->distanceToWalkoffEdge(cx, cz, region), radius);

        // Camera transform with the blob's centre as the object origin, as
        // setupObjectTransform does for a mesh.
        ::clear<Register::TRX, Safe>();
        ::clear<Register::TRY, Safe>();
        ::clear<Register::TRZ, Safe>();
        writeSafe<PseudoRegister::Rotation>(m_currentCamera->GetRotation());
        psyqo::Vec3 centre;
        centre.x.value = cx; centre.y.value = floorY; centre.z.value = cz;
        writeSafe<PseudoRegister::V0>(centre);
        Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
        psyqo::Vec3 t = readSafe<PseudoRegister::SV>();
        t.x += cameraPosition.x; t.y += cameraPosition.y; t.z += cameraPosition.z;
        writeSafe<PseudoRegister::Translation>(t);

        // Centre plus eight rim vertices, each rim Y on the centre's floor
        // plane at its own XZ so the blob tilts with a ramp.
        psyqo::GTE::PackedVec3 local[1 + shadowmath::kRimVerts];
        local[0].x.value = 0; local[0].y.value = 0; local[0].z.value = 0;
        for (int i = 0; i < shadowmath::kRimVerts; i++) {
            int32_t dx = (radius * shadowmath::kRimCos[i]) >> 12;
            int32_t dz = (radius * shadowmath::kRimSin[i]) >> 12;
            int32_t dy = 0;
            if (region != NAV_NO_REGION) dy = m_shadowNav->getFloorY(cx + dx, cz + dz, region) - floorY;
            if (dy > 0x7FFF) dy = 0x7FFF;
            if (dy < -0x7FFF) dy = -0x7FFF;
            local[1 + i].x.value = (int16_t)dx; local[1 + i].y.value = (int16_t)dy; local[1 + i].z.value = (int16_t)dz;
        }
        psyqo::Vertex sxy[1 + shadowmath::kRimVerts];
        int32_t sz[1 + shadowmath::kRimVerts];
        for (int b = 0; b < 1 + shadowmath::kRimVerts; b += 3) {
            writeSafe<PseudoRegister::V0>(local[b]);
            writeSafe<PseudoRegister::V1>(local[b + 1]);
            writeSafe<PseudoRegister::V2>(local[b + 2]);
            Kernels::rtpt();
            uint32_t u0, u1, u2;
            read<Register::SZ1>(&u0);
            read<Register::SZ2>(&u1);
            read<Register::SZ3>(&u2);
            sz[b] = (int32_t)u0; sz[b + 1] = (int32_t)u1; sz[b + 2] = (int32_t)u2;
            read<Register::SXY0>(&sxy[b].packed);
            read<Register::SXY1>(&sxy[b + 1].packed);
            read<Register::SXY2>(&sxy[b + 2].packed);
        }

        // Cull: any vertex at the near plane (its projection is garbage), the
        // centre past the fog wall or the table, or all of it off one screen edge.
        static constexpr int32_t NEAR_SZ_THRESHOLD = 4;
        bool nearPlane = false;
        int16_t minX = 0x7FFF, maxX = -0x7FFF, minY = 0x7FFF, maxY = -0x7FFF;
        for (int i = 0; i < 1 + shadowmath::kRimVerts; i++) {
            if (sz[i] < NEAR_SZ_THRESHOLD) nearPlane = true;
            if (sxy[i].x < minX) minX = sxy[i].x;
            if (sxy[i].x > maxX) maxX = sxy[i].x;
            if (sxy[i].y < minY) minY = sxy[i].y;
            if (sxy[i].y > maxY) maxY = sxy[i].y;
        }
        if (nearPlane) continue;
        if (fogFarSZ > 0 && sz[0] >= fogFarSZ) continue;
        if (sz[0] >= (int32_t)m_otSize) continue;
        if (maxX < 0 || minX >= currentScreen().width || maxY < 0 || minY >= currentScreen().height) continue;

        int32_t fogKeep = 4096;
        if (m_fog.enabled) fogKeep = 4096 - shadowmath::fogAmount(sz[0], fogFarSZ);
        const uint8_t dark = shadowmath::blobDarkness(rec.darkness, fade, edge, fogKeep);
        if (dark == 0) continue;

        int32_t zIndex = sz[0] - kShadowDepthBias;
        if (zIndex < WORLD_DEPTH_MIN) zIndex = WORLD_DEPTH_MIN;
        for (int i = 0; i < 1 + shadowmath::kRimVerts; i++) clampForRasterizer(sxy[i]);

        // Last inserted is drawn first: restore, fan, subtract.
        auto& restore = balloc.allocateFragment<psyqo::Prim::TPage>();
        restore.primitive.attr.setDithering(true);
        restore.primitive.attr.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
        ot.insert(restore, zIndex);
        const psyqo::Color centreColour = {.r = dark, .g = dark, .b = dark};
        const psyqo::Color rimColour = {.r = 0, .g = 0, .b = 0};
        for (int i = 0; i < shadowmath::kRimVerts; i++) {
            auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
            p.primitive.pointA = sxy[0];
            p.primitive.pointB = sxy[1 + i];
            p.primitive.pointC = sxy[1 + (i + 1) % shadowmath::kRimVerts];
            p.primitive.setColorA(centreColour);
            p.primitive.setColorB(rimColour);
            p.primitive.setColorC(rimColour);
            p.primitive.setSemiTrans();
            ot.insert(p, zIndex);
        }
        auto& sub = balloc.allocateFragment<psyqo::Prim::TPage>();
        sub.primitive.attr.setDithering(true);
        sub.primitive.attr.set(psyqo::Prim::TPageAttr::FullBackSubFullFront);
        ot.insert(sub, zIndex);
    }
}
#endif
