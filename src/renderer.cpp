#include "renderer.hh"

#include <EASTL/array.h>
#include <EASTL/vector.h>

#include <psyqo/fixed-point.hh>
#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/kernel.hh>
#include <psyqo/matrix.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/control.hh>
#include <psyqo/primitives/triangles.hh>
#include <psyqo/soft-math.hh>
#include <psyqo/trigonometry.hh>
#include <psyqo/vector.hh>

#include "gtemath.hh"
#include "skinmesh.hh"
#include "uisystem.hh"
#ifdef PSXSPLASH_MEMOVERLAY
#include "memoverlay.hh"
#endif

using namespace psyqo::fixed_point_literals;
using namespace psyqo::trig_literals;
using namespace psyqo::GTE;

psxsplash::Renderer* psxsplash::Renderer::instance = nullptr;

void psxsplash::Renderer::Init(psyqo::GPU& gpuInstance) {
    psyqo::Kernel::assert(instance == nullptr,
                          "A second initialization of Renderer was tried");
    clear<Register::TRX, Safe>();
    clear<Register::TRY, Safe>();
    clear<Register::TRZ, Safe>();
    write<Register::OFX, Safe>(psyqo::FixedPoint<16>(160.0).raw());
    write<Register::OFY, Safe>(psyqo::FixedPoint<16>(120.0).raw());
    write<Register::H, Safe>(PROJ_H);
    write<Register::ZSF3, Safe>(ORDERING_TABLE_SIZE / 3);
    write<Register::ZSF4, Safe>(ORDERING_TABLE_SIZE / 4);
    if (!instance) { instance = new Renderer(gpuInstance); }
}

void psxsplash::Renderer::SetCamera(psxsplash::Camera& camera) {
    m_currentCamera = &camera;
    // Update GTE H register to match camera's projection distance
    write<Register::H, Unsafe>(camera.GetProjectionH());
}

void psxsplash::Renderer::SetFog(const FogConfig& fog) {
    m_fog = fog;
    // Always use fog color as the GPU clear/back color
    m_clearcolor = fog.color;
    if (fog.enabled) {
        m_fog.fogFarSZ = 20000 / fog.density;
    } else {
        m_fog.fogFarSZ = 0;
    }
}

psyqo::Vec3 psxsplash::Renderer::computeCameraViewPos() {
    ::clear<Register::TRX, Safe>();
    ::clear<Register::TRY, Safe>();
    ::clear<Register::TRZ, Safe>();
    writeSafe<PseudoRegister::Rotation>(m_currentCamera->GetRotation());
    writeSafe<PseudoRegister::V0>(-m_currentCamera->GetPosition());
    Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
    return readSafe<PseudoRegister::SV>();
}

void psxsplash::Renderer::setupObjectTransform(
    GameObject* obj, const psyqo::Vec3& cameraPosition) {
    ::clear<Register::TRX, Safe>();
    ::clear<Register::TRY, Safe>();
    ::clear<Register::TRZ, Safe>();
    writeSafe<PseudoRegister::Rotation>(m_currentCamera->GetRotation());
    writeSafe<PseudoRegister::V0>(obj->position);
    Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
    psyqo::Vec3 objectPosition = readSafe<PseudoRegister::SV>();
    objectPosition.x += cameraPosition.x;
    objectPosition.y += cameraPosition.y;
    objectPosition.z += cameraPosition.z;
    psyqo::Matrix33 finalMatrix;
    MatrixMultiplyGTE(m_currentCamera->GetRotation(), obj->rotation, &finalMatrix);
    writeSafe<PseudoRegister::Translation>(objectPosition);
    writeSafe<PseudoRegister::Rotation>(finalMatrix);
}

// Per-vertex fog blend for untextured triangles: interpolate vertex color toward fog color.
static inline psyqo::Color fogBlend(psyqo::Color vc, int32_t ir0, psyqo::Color fogC) {
    if (ir0 <= 0) return vc;
    if (ir0 >= 4096) return fogC;
    int32_t keep = 4096 - ir0;
    return {
        .r = (uint8_t)((vc.r * keep + fogC.r * ir0) >> 12),
        .g = (uint8_t)((vc.g * keep + fogC.g * ir0) >> 12),
        .b = (uint8_t)((vc.b * keep + fogC.b * ir0) >> 12),
    };
}


static inline psyqo::Color fogBaseColor(int32_t ir, psyqo::Color fogC) {
    if (ir <= 0) return {.r = 0, .g = 0, .b = 0};
    if (ir >= 4096) return fogC;
    return {
        .r = (uint8_t)((fogC.r * ir) >> 12),
        .g = (uint8_t)((fogC.g * ir) >> 12),
        .b = (uint8_t)((fogC.b * ir) >> 12),
    };
}

static inline psyqo::Color fogTexColor(psyqo::Color vc, int32_t ir) {
    if (ir <= 0) return vc;
    if (ir >= 4096) return {.r = 0, .g = 0, .b = 0};
    int32_t keep = 4096 - ir;
    return {
        .r = (uint8_t)((vc.r * keep) >> 12),
        .g = (uint8_t)((vc.g * keep) >> 12),
        .b = (uint8_t)((vc.b * keep) >> 12),
    };
}


void psxsplash::Renderer::processTriangle(
    Tri& tri, int32_t fogFarSZ,
    psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot,
    psyqo::BumpAllocator<BUMP_ALLOCATOR_SIZE>& balloc,
    int depth) {

    writeSafe<PseudoRegister::V0>(tri.v0);
    writeSafe<PseudoRegister::V1>(tri.v1);
    writeSafe<PseudoRegister::V2>(tri.v2);

    Kernels::rtpt();

    uint32_t u0, u1, u2;
    read<Register::SZ1>(&u0);
    read<Register::SZ2>(&u1);
    read<Register::SZ3>(&u2);
    int32_t sz0 = (int32_t)u0, sz1 = (int32_t)u1, sz2 = (int32_t)u2;

    // Entirely behind camera → invisible.
    if (sz0 <= 0 && sz1 <= 0 && sz2 <= 0) return;
    // Entirely beyond fog wall → invisible.
    if (fogFarSZ > 0 && sz0 > fogFarSZ && sz1 > fogFarSZ && sz2 > fogFarSZ) return;

    int32_t zIndex = eastl::max(eastl::max(sz0, sz1), sz2);
    if (zIndex >= (int32_t)ORDERING_TABLE_SIZE) return;
    if (zIndex < 1) zIndex = 1;

    psyqo::Vertex projected[3];
    read<Register::SXY0>(&projected[0].packed);
    read<Register::SXY1>(&projected[1].packed);
    read<Register::SXY2>(&projected[2].packed);

    // SZ below this threshold → GTE SXY output is garbage (near-zero Z division).
    static constexpr int32_t NEAR_SZ_THRESHOLD = 4;
    bool hasNearPlane = (sz0 < NEAR_SZ_THRESHOLD || sz1 < NEAR_SZ_THRESHOLD ||
                         sz2 < NEAR_SZ_THRESHOLD);

    // Near-plane subdivision: split on edge with largest SZ delta, re-project halves.
    static constexpr int MAX_SUBDIV_DEPTH = 5;
    if (hasNearPlane && depth < MAX_SUBDIV_DEPTH) {
        auto iabs = [](int32_t v) -> int32_t { return v < 0 ? -v : v; };
        int32_t score01 = iabs(sz0 - sz1);
        int32_t score12 = iabs(sz1 - sz2);
        int32_t score20 = iabs(sz2 - sz0);

        Tri childA = tri, childB = tri;

        if (score01 >= score12 && score01 >= score20) {
            // Split edge v0–v1 → midpoint M.  childA = {v0, M, v2}, childB = {M, v1, v2}.
            psyqo::GTE::PackedVec3 m;
            m.x.value = (int16_t)(((int32_t)tri.v0.x.value + (int32_t)tri.v1.x.value) >> 1);
            m.y.value = (int16_t)(((int32_t)tri.v0.y.value + (int32_t)tri.v1.y.value) >> 1);
            m.z.value = (int16_t)(((int32_t)tri.v0.z.value + (int32_t)tri.v1.z.value) >> 1);
            uint8_t mu = (uint8_t)(((int)tri.uvA.u + (int)tri.uvB.u) >> 1);
            uint8_t mv = (uint8_t)(((int)tri.uvA.v + (int)tri.uvB.v) >> 1);
            psyqo::Color mc = {
                .r = (uint8_t)(((int)tri.colorA.r + (int)tri.colorB.r) >> 1),
                .g = (uint8_t)(((int)tri.colorA.g + (int)tri.colorB.g) >> 1),
                .b = (uint8_t)(((int)tri.colorA.b + (int)tri.colorB.b) >> 1),
            };
            childA.v1 = m; childA.uvB.u = mu; childA.uvB.v = mv; childA.colorB = mc;
            childB.v0 = m; childB.uvA.u = mu; childB.uvA.v = mv; childB.colorA = mc;
        } else if (score12 >= score20) {
            // Split edge v1–v2 → midpoint M.  childA = {v0, v1, M}, childB = {v0, M, v2}.
            psyqo::GTE::PackedVec3 m;
            m.x.value = (int16_t)(((int32_t)tri.v1.x.value + (int32_t)tri.v2.x.value) >> 1);
            m.y.value = (int16_t)(((int32_t)tri.v1.y.value + (int32_t)tri.v2.y.value) >> 1);
            m.z.value = (int16_t)(((int32_t)tri.v1.z.value + (int32_t)tri.v2.z.value) >> 1);
            uint8_t mu = (uint8_t)(((int)tri.uvB.u + (int)tri.uvC.u) >> 1);
            uint8_t mv = (uint8_t)(((int)tri.uvB.v + (int)tri.uvC.v) >> 1);
            psyqo::Color mc = {
                .r = (uint8_t)(((int)tri.colorB.r + (int)tri.colorC.r) >> 1),
                .g = (uint8_t)(((int)tri.colorB.g + (int)tri.colorC.g) >> 1),
                .b = (uint8_t)(((int)tri.colorB.b + (int)tri.colorC.b) >> 1),
            };
            childA.v2 = m; childA.uvC.u = mu; childA.uvC.v = mv; childA.colorC = mc;
            childB.v1 = m; childB.uvB.u = mu; childB.uvB.v = mv; childB.colorB = mc;
        } else {
            // Split edge v2–v0 → midpoint M.  childA = {M, v1, v2}, childB = {v0, v1, M}.
            psyqo::GTE::PackedVec3 m;
            m.x.value = (int16_t)(((int32_t)tri.v2.x.value + (int32_t)tri.v0.x.value) >> 1);
            m.y.value = (int16_t)(((int32_t)tri.v2.y.value + (int32_t)tri.v0.y.value) >> 1);
            m.z.value = (int16_t)(((int32_t)tri.v2.z.value + (int32_t)tri.v0.z.value) >> 1);
            uint8_t mu = (uint8_t)(((int)tri.uvC.u + (int)tri.uvA.u) >> 1);
            uint8_t mv = (uint8_t)(((int)tri.uvC.v + (int)tri.uvA.v) >> 1);
            psyqo::Color mc = {
                .r = (uint8_t)(((int)tri.colorC.r + (int)tri.colorA.r) >> 1),
                .g = (uint8_t)(((int)tri.colorC.g + (int)tri.colorA.g) >> 1),
                .b = (uint8_t)(((int)tri.colorC.b + (int)tri.colorA.b) >> 1),
            };
            childA.v0 = m; childA.uvA.u = mu; childA.uvA.v = mv; childA.colorA = mc;
            childB.v2 = m; childB.uvC.u = mu; childB.uvC.v = mv; childB.colorC = mc;
        }

        processTriangle(childA, fogFarSZ, ot, balloc, depth + 1);
        processTriangle(childB, fogFarSZ, ot, balloc, depth + 1);
        return;
    }

    // Leaf path: off-screen reject + backface cull (skipped for near-plane leaves).
    if (!hasNearPlane) {
        if (isCompletelyOutside(projected[0], projected[1], projected[2])) return;

        Kernels::nclip();
        int32_t mac0 = 0;
        read<Register::MAC0>(reinterpret_cast<uint32_t*>(&mac0));
        if (mac0 <= 0) return;
    }

    // Clamp to safe rasterizer range (1023×511 max delta).
    clampForRasterizer(projected[0]);
    clampForRasterizer(projected[1]);
    clampForRasterizer(projected[2]);

    // Per-vertex fog (deferred to leaf to avoid wasted divisions during subdivision).
    int32_t fogIR[3] = {0, 0, 0};
    if (fogFarSZ > 0) {
        int32_t fogNear = fogFarSZ >> 3;
        int32_t range = fogFarSZ - fogNear;
        if (range < 1) range = 1;
        int32_t szArr[3] = {sz0, sz1, sz2};
        for (int vi = 0; vi < 3; vi++) {
            int32_t ir;
            if (szArr[vi] <= fogNear) {
                ir = 0;
            } else if (szArr[vi] >= fogFarSZ) {
                ir = 4096;
            } else {
                ir = ((szArr[vi] - fogNear) * 4096) / range;
            }
            fogIR[vi] = ir;
        }
    }

    psyqo::Color cA = tri.colorA, cB = tri.colorB, cC = tri.colorC;
    bool hasFog = m_fog.enabled && (fogIR[0] > 0 || fogIR[1] > 0 || fogIR[2] > 0);

    if (tri.isUntextured()) {
        if (hasFog) {
            cA = fogBlend(cA, fogIR[0], m_fog.color);
            cB = fogBlend(cB, fogIR[1], m_fog.color);
            cC = fogBlend(cC, fogIR[2], m_fog.color);
        }
        auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
        p.primitive.pointA = projected[0]; p.primitive.pointB = projected[1]; p.primitive.pointC = projected[2];
        p.primitive.setColorA(cA); p.primitive.setColorB(cB); p.primitive.setColorC(cC);
        p.primitive.setOpaque();
        ot.insert(p, zIndex);
    } else if (hasFog) {
        psyqo::Color fogA = fogBaseColor(fogIR[0], m_fog.color);
        psyqo::Color fogB = fogBaseColor(fogIR[1], m_fog.color);
        psyqo::Color fogC = fogBaseColor(fogIR[2], m_fog.color);
        psyqo::Color texA = fogTexColor(cA, fogIR[0]);
        psyqo::Color texB = fogTexColor(cB, fogIR[1]);
        psyqo::Color texC = fogTexColor(cC, fogIR[2]);

        auto& fogP = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
        fogP.primitive.pointA = projected[0]; fogP.primitive.pointB = projected[1]; fogP.primitive.pointC = projected[2];
        fogP.primitive.setColorA(fogA); fogP.primitive.setColorB(fogB); fogP.primitive.setColorC(fogC);
        fogP.primitive.setSemiTrans();
        ot.insert(fogP, zIndex);

        auto& texP = balloc.allocateFragment<psyqo::Prim::GouraudTexturedTriangle>();
        texP.primitive.pointA = projected[0]; texP.primitive.pointB = projected[1]; texP.primitive.pointC = projected[2];
        texP.primitive.uvA = tri.uvA;
        texP.primitive.uvB = tri.uvB;
        texP.primitive.uvC.u = tri.uvC.u; texP.primitive.uvC.v = tri.uvC.v;
        texP.primitive.tpage = tri.tpage;
        texP.primitive.tpage.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
        psyqo::PrimPieces::ClutIndex clut(tri.clutX, tri.clutY);
        texP.primitive.clutIndex = clut;
        texP.primitive.setColorA(texA); texP.primitive.setColorB(texB); texP.primitive.setColorC(texC);
        texP.primitive.setOpaque();
        ot.insert(texP, zIndex);
    } else {

        auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTexturedTriangle>();
        p.primitive.pointA = projected[0]; p.primitive.pointB = projected[1]; p.primitive.pointC = projected[2];
        p.primitive.uvA = tri.uvA;
        p.primitive.uvB = tri.uvB;
        p.primitive.uvC.u = tri.uvC.u; p.primitive.uvC.v = tri.uvC.v;
        p.primitive.tpage = tri.tpage;
        p.primitive.tpage.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
        psyqo::PrimPieces::ClutIndex clut(tri.clutX, tri.clutY);
        p.primitive.clutIndex = clut;
        p.primitive.setColorA(cA); p.primitive.setColorB(cB); p.primitive.setColorC(cC);
        p.primitive.setOpaque();
        ot.insert(p, zIndex);
    }
}

// ============================================================================
// Render paths
// ============================================================================

void psxsplash::Renderer::Render(eastl::vector<GameObject*>& objects) {
    psyqo::Kernel::assert(m_currentCamera != nullptr, "PSXSPLASH: Tried to render without an active camera");
    // Re-sync GTE H register each frame (supports dynamic FOV / cutscene H tracks)
    write<Register::H, Unsafe>(m_currentCamera->GetProjectionH());
    uint8_t parity = m_gpu.getParity();
    auto& ot = m_ots[parity]; auto& clear = m_clear[parity]; auto& balloc = m_ballocs[parity];
    balloc.reset();
    auto& ditherCmd = balloc.allocateFragment<psyqo::Prim::TPage>();
    ditherCmd.primitive.attr.setDithering(true);
    ditherCmd.primitive.attr.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
    ot.insert(ditherCmd, ORDERING_TABLE_SIZE - 1);

    psyqo::Vec3 cameraPosition = computeCameraViewPos();
    int32_t fogFarSZ = m_fog.fogFarSZ;
    for (auto& obj : objects) {
        if (!obj->isActive()) continue;
        if (obj->isSkinned()) continue;
        setupObjectTransform(obj, cameraPosition);
        for (int i = 0; i < obj->polyCount; i++)
            processTriangle(obj->polygons[i], fogFarSZ, ot, balloc);
    }
    renderSkinnedObjects(objects, cameraPosition, fogFarSZ, ot, balloc);
    if (m_uiSystem) m_uiSystem->renderOT(m_gpu, ot, balloc);
#ifdef PSXSPLASH_MEMOVERLAY
    if (m_memOverlay) m_memOverlay->renderOT(ot, balloc);
#endif
    m_gpu.getNextClear(clear.primitive, m_clearcolor);
    m_gpu.chain(clear); m_gpu.chain(ot);
    if (m_uiSystem) m_uiSystem->renderText(m_gpu);
#ifdef PSXSPLASH_MEMOVERLAY
    if (m_memOverlay) m_memOverlay->renderText(m_gpu);
#endif
    m_frameCount++;
}

void psxsplash::Renderer::RenderWithBVH(eastl::vector<GameObject*>& objects, const BVHManager& bvh) {
    psyqo::Kernel::assert(m_currentCamera != nullptr, "PSXSPLASH: Tried to render without an active camera");
    if (!bvh.isLoaded()) { Render(objects); return; }
    // Re-sync GTE H register each frame (supports dynamic FOV / cutscene H tracks)
    write<Register::H, Unsafe>(m_currentCamera->GetProjectionH());
    uint8_t parity = m_gpu.getParity();
    auto& ot = m_ots[parity]; auto& clear = m_clear[parity]; auto& balloc = m_ballocs[parity];
    balloc.reset();
    auto& ditherCmd2 = balloc.allocateFragment<psyqo::Prim::TPage>();
    ditherCmd2.primitive.attr.setDithering(true);
    ditherCmd2.primitive.attr.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
    ot.insert(ditherCmd2, ORDERING_TABLE_SIZE - 1);

    Frustum frustum; m_currentCamera->ExtractFrustum(frustum);
    int visibleCount = bvh.cullFrustum(frustum, m_visibleRefs, MAX_VISIBLE_TRIANGLES);
    psyqo::Vec3 cameraPosition = computeCameraViewPos();
    int32_t fogFarSZ = m_fog.fogFarSZ;
    int16_t lastObjectIndex = -1;
    bool lastObjCulled = false;
    for (int i = 0; i < visibleCount; i++) {
        const TriangleRef& ref = m_visibleRefs[i];
        if (ref.objectIndex >= objects.size()) continue;
        GameObject* obj = objects[ref.objectIndex];
        if (!obj->isActive()) continue;
        if (obj->isDynamicMoved()) continue;  // Skip dynamic objects in BVH pass — rendered below
        if (obj->isSkinned()) continue;  // Skip skinned objects — rendered in skinned pass
        if (ref.triangleIndex >= obj->polyCount) continue;
        if (ref.objectIndex != lastObjectIndex) {
            lastObjectIndex = ref.objectIndex;

            BVHNode objBox;
            objBox.minX = obj->aabbMinX; objBox.minY = obj->aabbMinY; objBox.minZ = obj->aabbMinZ;
            objBox.maxX = obj->aabbMaxX; objBox.maxY = obj->aabbMaxY; objBox.maxZ = obj->aabbMaxZ;
            if (!frustum.testAABB(objBox)) {
                lastObjCulled = true;
                continue;
            }
            lastObjCulled = false;
            setupObjectTransform(obj, cameraPosition);
        }
        if (lastObjCulled) continue;
        processTriangle(obj->polygons[ref.triangleIndex], fogFarSZ, ot, balloc);
    }

    // Second pass: render dynamically-moved objects (their BVH references are stale).
    // Uses per-object AABB frustum test then renders all triangles — like the non-BVH path.
    for (size_t oi = 0; oi < objects.size(); oi++) {
        GameObject* obj = objects[oi];
        if (!obj->isActive() || !obj->isDynamicMoved()) continue;
        if (obj->isSkinned()) continue;
        BVHNode objBox;
        objBox.minX = obj->aabbMinX; objBox.minY = obj->aabbMinY; objBox.minZ = obj->aabbMinZ;
        objBox.maxX = obj->aabbMaxX; objBox.maxY = obj->aabbMaxY; objBox.maxZ = obj->aabbMaxZ;
        if (!frustum.testAABB(objBox)) continue;
        setupObjectTransform(obj, cameraPosition);
        for (int t = 0; t < obj->polyCount; t++) {
            processTriangle(obj->polygons[t], fogFarSZ, ot, balloc);
        }
    }

    renderSkinnedObjects(objects, cameraPosition, fogFarSZ, ot, balloc, &frustum);

    if (m_uiSystem) m_uiSystem->renderOT(m_gpu, ot, balloc);
#ifdef PSXSPLASH_MEMOVERLAY
    if (m_memOverlay) m_memOverlay->renderOT(ot, balloc);
#endif
    m_gpu.getNextClear(clear.primitive, m_clearcolor);
    m_gpu.chain(clear); m_gpu.chain(ot);
    if (m_uiSystem) m_uiSystem->renderText(m_gpu);
#ifdef PSXSPLASH_MEMOVERLAY
    if (m_memOverlay) m_memOverlay->renderText(m_gpu);
#endif
    m_frameCount++;
}

// ============================================================================
// RenderWithRooms - Portal/room occlusion for interior scenes
// ============================================================================

struct ScreenRect { int16_t minX, minY, maxX, maxY; };

static inline bool intersectRect(const ScreenRect& a, const ScreenRect& b, ScreenRect& out) {
    out.minX = (a.minX > b.minX) ? a.minX : b.minX; out.minY = (a.minY > b.minY) ? a.minY : b.minY;
    out.maxX = (a.maxX < b.maxX) ? a.maxX : b.maxX; out.maxY = (a.maxY < b.maxY) ? a.maxY : b.maxY;
    return out.minX < out.maxX && out.minY < out.maxY;
}

// Safety margin added to portal screen rects (pixels).
// Prevents geometry from popping at portal edges due to fixed-point rounding.
static constexpr int16_t PORTAL_MARGIN = 16;

// Transform a world-space point to camera space using the view rotation matrix.
static inline void worldToCamera(int32_t wx, int32_t wy, int32_t wz,
    int32_t camX, int32_t camY, int32_t camZ,
    const psyqo::Matrix33& camRot,
    int32_t& outX, int32_t& outY, int32_t& outZ) {
    int32_t rx = wx - camX, ry = wy - camY, rz = wz - camZ;
    outX = (int32_t)(((int64_t)camRot.vs[0].x.value * rx + (int64_t)camRot.vs[0].y.value * ry +
                       (int64_t)camRot.vs[0].z.value * rz) >> 12);
    outY = (int32_t)(((int64_t)camRot.vs[1].x.value * rx + (int64_t)camRot.vs[1].y.value * ry +
                       (int64_t)camRot.vs[1].z.value * rz) >> 12);
    outZ = (int32_t)(((int64_t)camRot.vs[2].x.value * rx + (int64_t)camRot.vs[2].y.value * ry +
                       (int64_t)camRot.vs[2].z.value * rz) >> 12);
}

// Project a camera-space point to screen coordinates. Returns false if behind near plane.
static inline bool projectToScreen(int32_t vx, int32_t vy, int32_t vz,
    int32_t projH, int16_t& sx, int16_t& sy) {
    if (vz <= 0) return false;
    int32_t vzs = vz >> 4; if (vzs <= 0) vzs = 1;
    int32_t rawX = (vx >> 4) * projH / vzs + 160;
    int32_t rawY = (vy >> 4) * projH / vzs + 120;
    if (rawX < -2048) rawX = -2048; else if (rawX > 2048) rawX = 2048;
    if (rawY < -2048) rawY = -2048; else if (rawY > 2048) rawY = 2048;
    sx = (int16_t)rawX;
    sy = (int16_t)rawY;
    return true;
}

// Project a portal quad to a screen-space AABB.
// Computes the 4 corners, transforms to camera space, clips against the near plane,
// projects visible points to screen, and returns the bounding rect.
static bool projectPortalRect(const psxsplash::PortalData& portal,
    int32_t camX, int32_t camY, int32_t camZ, const psyqo::Matrix33& camRot,
    int32_t projH, ScreenRect& outRect) {

    // Compute portal corner offsets in world space.
    int32_t rwx = ((int32_t)portal.rightX * portal.halfW) >> 12;
    int32_t rwy = ((int32_t)portal.rightY * portal.halfW) >> 12;
    int32_t rwz = ((int32_t)portal.rightZ * portal.halfW) >> 12;
    int32_t uhx = ((int32_t)portal.upX * portal.halfH) >> 12;
    int32_t uhy = ((int32_t)portal.upY * portal.halfH) >> 12;
    int32_t uhz = ((int32_t)portal.upZ * portal.halfH) >> 12;

    int32_t cx = portal.centerX, cy = portal.centerY, cz = portal.centerZ;

    // Transform 4 corners to camera space
    struct CamVert { int32_t x, y, z; };
    CamVert cv[4];
    int32_t wCorners[4][3] = {
        {cx + rwx + uhx, cy + rwy + uhy, cz + rwz + uhz},
        {cx - rwx + uhx, cy - rwy + uhy, cz - rwz + uhz},
        {cx - rwx - uhx, cy - rwy - uhy, cz - rwz - uhz},
        {cx + rwx - uhx, cy + rwy - uhy, cz + rwz - uhz},
    };

    int behindCount = 0;
    for (int i = 0; i < 4; i++) {
        worldToCamera(wCorners[i][0], wCorners[i][1], wCorners[i][2],
                      camX, camY, camZ, camRot, cv[i].x, cv[i].y, cv[i].z);
        if (cv[i].z <= 0) behindCount++;
    }

    // Any corner behind camera → conservative fullscreen rect (unless portal center is far behind).
    if (behindCount > 0) {
        int32_t vx, vy, vz;
        worldToCamera(cx, cy, cz, camX, camY, camZ, camRot, vx, vy, vz);
        int32_t portalExtent = portal.halfW > portal.halfH ? portal.halfW : portal.halfH;
        int32_t distLimit = (behindCount == 4) ? portalExtent * 2 : portalExtent * 4;
        if (vz < 0 && -vz > distLimit) return false;
        outRect = {-512, -512, 832, 752};
        return true;
    }

    // Clip against near plane (z=1) and project visible points.
    // For each edge where one vertex is in front and one behind,
    // compute the intersection point and include it in the screen rect.
    constexpr int32_t NEAR_Z = 1;
    int16_t sxMin = 32767, sxMax = -32768;
    int16_t syMin = 32767, syMax = -32768;
    int projCount = 0;

    for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;

        // Project vertex i if in front
        if (cv[i].z > 0) {
            int16_t sx, sy;
            if (projectToScreen(cv[i].x, cv[i].y, cv[i].z, projH, sx, sy)) {
                if (sx < sxMin) sxMin = sx;
                if (sx > sxMax) sxMax = sx;
                if (sy < syMin) syMin = sy;
                if (sy > syMax) syMax = sy;
                projCount++;
            }
        }

        // If edge crosses the near plane, clip and project the intersection.
        // All 32-bit arithmetic (no __divdi3 on MIPS R3000).
        bool iFront = cv[i].z > 0;
        bool jFront = cv[j].z > 0;
        if (iFront != jFront) {
            int32_t dz = cv[j].z - cv[i].z;
            if (dz == 0) continue;
            int32_t dzs = dz >> 4;
            if (dzs == 0) dzs = (dz > 0) ? 1 : -1;  // prevent div-by-zero after shift
            // Compute t in 4.12 fixed-point. Shift num/den by 4 to keep * 4096 in 32 bits.
            int32_t t12 = (((NEAR_Z - cv[i].z) >> 4) * 4096) / dzs;
            // Apply t: clip = cv[i] + (cv[j] - cv[i]) * t12 / 4096
            // Shift dx by 4 so (dx>>4)*t12 fits int32, then >>8 to undo (4+8=12 total)
            int32_t clipX = cv[i].x + ((((cv[j].x - cv[i].x) >> 4) * t12) >> 8);
            int32_t clipY = cv[i].y + ((((cv[j].y - cv[i].y) >> 4) * t12) >> 8);
            int16_t sx, sy;
            if (projectToScreen(clipX, clipY, NEAR_Z, projH, sx, sy)) {
                if (sx < sxMin) sxMin = sx;
                if (sx > sxMax) sxMax = sx;
                if (sy < syMin) syMin = sy;
                if (sy > syMax) syMax = sy;
                projCount++;
            }
        }
    }

    if (projCount == 0) return false;

    outRect = {
        (int16_t)(sxMin - PORTAL_MARGIN), (int16_t)(syMin - PORTAL_MARGIN),
        (int16_t)(sxMax + PORTAL_MARGIN), (int16_t)(syMax + PORTAL_MARGIN)
    };
    return true;
}

// Test if a room's AABB is potentially visible to the camera frustum.
// Quick rejection test: if the room is entirely behind the camera, skip it.
static bool isRoomPotentiallyVisible(const psxsplash::RoomData& room,
    int32_t camX, int32_t camY, int32_t camZ, const psyqo::Matrix33& camRot) {
    // Transform the room's AABB center to camera space and check Z.
    // Use the p-vertex approach: find the corner most in the camera forward direction.
    int32_t fwdX = camRot.vs[2].x.value;
    int32_t fwdY = camRot.vs[2].y.value;
    int32_t fwdZ = camRot.vs[2].z.value;

    // p-vertex: corner of AABB closest to camera forward direction
    int32_t px = (fwdX >= 0) ? room.aabbMaxX : room.aabbMinX;
    int32_t py = (fwdY >= 0) ? room.aabbMaxY : room.aabbMinY;
    int32_t pz = (fwdZ >= 0) ? room.aabbMaxZ : room.aabbMinZ;

    // If p-vertex is behind camera, the entire AABB is behind
    int32_t rx = px - camX, ry = py - camY, rz = pz - camZ;
    int64_t dotFwd = ((int64_t)fwdX * rx + (int64_t)fwdY * ry + (int64_t)fwdZ * rz) >> 12;
    if (dotFwd < -4096) return false;  // Entirely behind with 1-unit margin

    return true;
}

// Project an axis-aligned bounding box to a screen-space bounding rectangle.
// Used to test whether a cell's geometry could contribute to the visible portal area.
// Returns false if the AABB is entirely behind the camera.
static bool projectAABBToScreen(
    int32_t bMinX, int32_t bMinY, int32_t bMinZ,
    int32_t bMaxX, int32_t bMaxY, int32_t bMaxZ,
    int32_t camX, int32_t camY, int32_t camZ,
    const psyqo::Matrix33& camRot, int32_t projH,
    ScreenRect& outRect) {

    int16_t sxMin = 32767, sxMax = -32768;
    int16_t syMin = 32767, syMax = -32768;
    int behindCount = 0;

    for (int i = 0; i < 8; i++) {
        int32_t wx = (i & 1) ? bMaxX : bMinX;
        int32_t wy = (i & 2) ? bMaxY : bMinY;
        int32_t wz = (i & 4) ? bMaxZ : bMinZ;
        int32_t vx, vy, vz;
        worldToCamera(wx, wy, wz, camX, camY, camZ, camRot, vx, vy, vz);
        if (vz <= 0) { behindCount++; continue; }
        int16_t sx, sy;
        if (projectToScreen(vx, vy, vz, projH, sx, sy)) {
            if (sx < sxMin) sxMin = sx;
            if (sx > sxMax) sxMax = sx;
            if (sy < syMin) syMin = sy;
            if (sy > syMax) syMax = sy;
        }
    }

    if (behindCount == 8) return false;  // Entirely behind camera
    if (behindCount > 0 || sxMin > sxMax) {
        // Partially behind or no projected points: report conservative fullscreen
        outRect = {-512, -512, 832, 752};
        return true;
    }
    outRect = {sxMin, syMin, sxMax, syMax};
    return true;
}

// Minimum visible portal dimension (pixels) before a room is rendered.
// Portals smaller than this on screen are skipped to avoid rendering
// entire rooms through tiny distant slivers or nearly-closed doors.
static constexpr int16_t MIN_PORTAL_SCREEN_DIM = 8;

void psxsplash::Renderer::RenderWithRooms(eastl::vector<GameObject*>& objects,
    const RoomData* rooms, int roomCount, const PortalData* portals, int portalCount,
    const TriangleRef* roomTriRefs, const RoomCell* cells,
    const RoomPortalRef* roomPortalRefs, int cameraRoom) {
    psyqo::Kernel::assert(m_currentCamera != nullptr, "PSXSPLASH: Tried to render without an active camera");
    if (roomCount == 0 || rooms == nullptr) { Render(objects); return; }
    // Re-sync GTE H register each frame (supports dynamic FOV / cutscene H tracks)
    write<Register::H, Unsafe>(m_currentCamera->GetProjectionH());

    uint8_t parity = m_gpu.getParity();
    auto& ot = m_ots[parity]; auto& clear = m_clear[parity]; auto& balloc = m_ballocs[parity];
    balloc.reset();
    auto& ditherCmd3 = balloc.allocateFragment<psyqo::Prim::TPage>();
    ditherCmd3.primitive.attr.setDithering(true);
    ditherCmd3.primitive.attr.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
    ot.insert(ditherCmd3, ORDERING_TABLE_SIZE - 1);

    psyqo::Vec3 cameraPosition = computeCameraViewPos();
    int32_t fogFarSZ = m_fog.fogFarSZ;
    int32_t camX = m_currentCamera->GetPosition().x.raw();
    int32_t camY = m_currentCamera->GetPosition().y.raw();
    int32_t camZ = m_currentCamera->GetPosition().z.raw();
    int32_t projH = m_currentCamera->GetProjectionH();
    Frustum frustum; m_currentCamera->ExtractFrustum(frustum);
    int catchAllIdx = roomCount - 1;

    // If no camera room provided (or invalid), fall back to AABB containment.
    // Pick the smallest room whose AABB (with margin) contains the camera.
    if (cameraRoom < 0 || cameraRoom >= catchAllIdx) {
        constexpr int32_t ROOM_MARGIN = 2048;  // 0.5 units in fp12
        int64_t bestVolume = 0x7FFFFFFFFFFFFFFFLL;
        for (int r = 0; r < catchAllIdx; r++) {
            if (camX >= rooms[r].aabbMinX - ROOM_MARGIN && camX <= rooms[r].aabbMaxX + ROOM_MARGIN &&
                camY >= rooms[r].aabbMinY - ROOM_MARGIN && camY <= rooms[r].aabbMaxY + ROOM_MARGIN &&
                camZ >= rooms[r].aabbMinZ - ROOM_MARGIN && camZ <= rooms[r].aabbMaxZ + ROOM_MARGIN) {
                int64_t dx = (int64_t)(rooms[r].aabbMaxX - rooms[r].aabbMinX);
                int64_t dy = (int64_t)(rooms[r].aabbMaxY - rooms[r].aabbMinY);
                int64_t dz = (int64_t)(rooms[r].aabbMaxZ - rooms[r].aabbMinZ);
                int64_t vol = dx * dy + dy * dz + dx * dz;
                if (vol < bestVolume) { bestVolume = vol; cameraRoom = r; }
            }
        }
    }

    uint32_t visited = 0;
    if (catchAllIdx < 32) visited = (1u << catchAllIdx);
    const auto& camRot = m_currentCamera->GetRotation();

    struct Entry { int room; int depth; ScreenRect clip; };
    Entry stack[64]; int top = 0;

    // Helper: render a span of tri-refs with per-object frustum culling.
    // lastObj/lastObjCulled are managed across the span to avoid redundant
    // object transforms when consecutive refs share an object.
    auto renderTriRefs = [&](const TriangleRef* refs, int count,
                             int16_t& lastObj, bool& lastObjCulled) {
        for (int ti = 0; ti < count; ti++) {
            const TriangleRef& ref = refs[ti];
            if (ref.objectIndex >= objects.size()) continue;
            GameObject* obj = objects[ref.objectIndex];
            if (!obj->isActive()) continue;
            if (obj->isDynamicMoved()) continue;  // Rendered in dynamic pass below
            if (obj->isSkinned()) continue;  // Rendered in skinned pass
            if (ref.triangleIndex >= obj->polyCount) continue;
            if (ref.objectIndex != lastObj) {
                lastObj = ref.objectIndex;
                BVHNode objBox;
                objBox.minX = obj->aabbMinX; objBox.minY = obj->aabbMinY; objBox.minZ = obj->aabbMinZ;
                objBox.maxX = obj->aabbMaxX; objBox.maxY = obj->aabbMaxY; objBox.maxZ = obj->aabbMaxZ;
                if (!frustum.testAABB(objBox)) {
                    lastObjCulled = true;
                    continue;
                }
                lastObjCulled = false;
                setupObjectTransform(obj, cameraPosition);
            }
            if (lastObjCulled) continue;
            processTriangle(obj->polygons[ref.triangleIndex], fogFarSZ, ot, balloc);
        }
    };

    ScreenRect full = {-512, -512, 832, 752};

    // Render a room's geometry, optionally clipped to a portal screen rect.
    // clipRect is the accumulated portal clip rectangle for portal-reached rooms,
    // or the fullscreen rect for the camera room and catch-all.
    auto renderRoom = [&](int ri, const ScreenRect& clipRect) {
        const RoomData& rm = rooms[ri];
        int16_t lastObj = -1;
        bool lastObjCulled = false;

        if (rm.cellCount > 0 && cells != nullptr) {
            // Only pay for cell-vs-screen projection when the clip rect is narrower
            // than fullscreen (i.e. room was reached through a portal chain).
            bool narrowClip = (clipRect.minX > -512 || clipRect.minY > -512 ||
                               clipRect.maxX < 832 || clipRect.maxY < 752);

            // Cell-based frustum culling: test each cell's AABB before processing its tris.
            for (int ci = 0; ci < rm.cellCount; ci++) {
                const RoomCell& cell = cells[rm.firstCell + ci];
                if (!frustum.testAABB(cell)) continue;

                // For portal-reached rooms, project cell AABB to screen and skip
                // cells that don't overlap the visible portal area.
                if (narrowClip) {
                    ScreenRect cellRect;
                    if (!projectAABBToScreen(cell.minX, cell.minY, cell.minZ,
                                            cell.maxX, cell.maxY, cell.maxZ,
                                            camX, camY, camZ, camRot, projH, cellRect)) continue;
                    ScreenRect clipped;
                    if (!intersectRect(clipRect, cellRect, clipped)) continue;
                }

                renderTriRefs(&roomTriRefs[cell.firstTriRef], cell.triRefCount,
                              lastObj, lastObjCulled);
            }
        } else {
            // Fallback: render all tris in room (no cell data available)
            renderTriRefs(&roomTriRefs[rm.firstTriRef], rm.triRefCount,
                          lastObj, lastObjCulled);
        }
    };

    // Always render catch-all room (geometry not assigned to any specific room)
    renderRoom(catchAllIdx, full);

    if (cameraRoom >= 0) {
        if (cameraRoom < 32) visited |= (1u << cameraRoom);
        stack[top++] = {cameraRoom, 0, full};
        while (top > 0) {
            Entry e = stack[--top];
            renderRoom(e.room, e.clip);
            if (e.depth >= 8) continue;  // Depth limit prevents infinite loops

            // Iterate portals connected to this room.
            // If per-room portal refs are available, use the indexed list (O(k) where k = neighbors).
            // Otherwise fall back to scanning all portals (O(N)).
            auto processPortal = [&](int p, int other) {
                if (other < 0 || other >= roomCount) return;
                if (other < 32 && (visited & (1u << other))) return;

                // Backface cull: skip portals that face away from the camera.
                {
                    int32_t dx = camX - portals[p].centerX;
                    int32_t dy = camY - portals[p].centerY;
                    int32_t dz = camZ - portals[p].centerZ;
                    int64_t dot = (int64_t)dx * portals[p].normalX +
                                  (int64_t)dy * portals[p].normalY +
                                  (int64_t)dz * portals[p].normalZ;
                    const int64_t BACKFACE_THRESHOLD = (int64_t)1024 * 4096;
                    if (portals[p].roomA == e.room) {
                        if (dot > BACKFACE_THRESHOLD) return;
                    } else {
                        if (dot < -BACKFACE_THRESHOLD) return;
                    }
                }

                // Frustum-cull the destination room's AABB.
                if (!isRoomPotentiallyVisible(rooms[other], camX, camY, camZ, camRot)) {
                    return;
                }

                // Project actual portal quad corners to screen.
                ScreenRect pr;
                if (!projectPortalRect(portals[p], camX, camY, camZ, camRot, projH, pr)) {
                    return;
                }
                ScreenRect isect;
                if (!intersectRect(e.clip, pr, isect)) {
                    return;
                }
                // Skip portals whose visible screen area is too small to matter.
                // Prevents rendering entire rooms through tiny distant slivers.
                if ((isect.maxX - isect.minX) < MIN_PORTAL_SCREEN_DIM ||
                    (isect.maxY - isect.minY) < MIN_PORTAL_SCREEN_DIM) return;

                // Project the destination room's AABB to screen and intersect with
                // the portal clip rect. This catches rooms whose geometry footprint
                // doesn't actually overlap the portal opening (e.g. a room off to
                // the side viewed through an oblique portal whose screen AABB is
                // misleadingly wide). Also tightens the clip rect for child portal
                // tests and cell culling.
                {
                    ScreenRect roomRect;
                    if (projectAABBToScreen(rooms[other].aabbMinX, rooms[other].aabbMinY,
                                            rooms[other].aabbMinZ, rooms[other].aabbMaxX,
                                            rooms[other].aabbMaxY, rooms[other].aabbMaxZ,
                                            camX, camY, camZ, camRot, projH, roomRect)) {
                        ScreenRect tighter;
                        if (!intersectRect(isect, roomRect, tighter)) return;
                        isect = tighter;
                        // Re-check minimum size after tightening
                        if ((isect.maxX - isect.minX) < MIN_PORTAL_SCREEN_DIM ||
                            (isect.maxY - isect.minY) < MIN_PORTAL_SCREEN_DIM) return;
                    }
                }

                if (other < 32) visited |= (1u << other);
                if (top < 64) stack[top++] = {other, e.depth + 1, isect};
            };

            if (roomPortalRefs && rooms[e.room].portalRefCount > 0) {
                // Per-room indexed portal list: only iterate portals touching this room.
                for (int pr = 0; pr < rooms[e.room].portalRefCount; pr++) {
                    const auto& ref = roomPortalRefs[rooms[e.room].firstPortalRef + pr];
                    processPortal(ref.portalIndex, ref.otherRoom);
                }
            } else {
                // Fallback: scan all portals to find ones connected to this room.
                for (int p = 0; p < portalCount; p++) {
                    int other = -1;
                    if (portals[p].roomA == e.room) other = portals[p].roomB;
                    else if (portals[p].roomB == e.room) other = portals[p].roomA;
                    else continue;
                    processPortal(p, other);
                }
            }
        }
    } else {
        // Camera room unknown - render ALL rooms as safety fallback.
        // This guarantees no geometry disappears, at the cost of no culling.
        for (int r = 0; r < roomCount; r++) if (r != catchAllIdx) renderRoom(r, full);
    }

#ifdef PSXSPLASH_ROOM_DEBUG
    // ================================================================
    // Debug overlay: render ALL room triangles as flat-color wireframe
    // on top of the scene. Each room gets a unique color. Brightness
    // modulates with distance so you can see depth. Triangles from
    // rooms that were NOT visited by the portal DFS are drawn dimmer.
    // Portal outlines and room AABB boxes are drawn as well.
    // ================================================================
    {
        static const psyqo::Color roomColors[] = {
            {.r = 255, .g = 50,  .b = 50},   // R0: red
            {.r = 50,  .g = 255, .b = 50},   // R1: green
            {.r = 50,  .g = 50,  .b = 255},  // R2: blue
            {.r = 255, .g = 255, .b = 50},   // R3: yellow
            {.r = 255, .g = 50,  .b = 255},  // R4: magenta
            {.r = 50,  .g = 255, .b = 255},  // R5: cyan
            {.r = 255, .g = 128, .b = 50},   // R6: orange
            {.r = 128, .g = 128, .b = 255},  // R7: lavender
        };
        constexpr int NUM_COLORS = sizeof(roomColors) / sizeof(roomColors[0]);

        // Room status bars at top of screen
        for (int r = 0; r < roomCount && r < 8; r++) {
            bool rendered = (visited & (1u << r)) != 0;
            bool isCamRoom = (r == cameraRoom);
            auto& tile = balloc.allocateFragment<psyqo::Prim::FastFill>();
            int16_t x = r * 18 + 2;
            tile.primitive.setColor(rendered ?
                roomColors[r % NUM_COLORS] : psyqo::Color{.r = 40, .g = 40, .b = 40});
            tile.primitive.rect = psyqo::Rect{
                .a = {.x = x, .y = (int16_t)2},
                .b = {.w = 14, .h = (int16_t)(isCamRoom ? 12 : 6)}
            };
            ot.insert(tile, 0);
        }

        // Render ALL room triangles as flat-colored tris on top.
        // Render triangles from visited rooms only, in per-room color.
        for (int ri = 0; ri < roomCount; ri++) {
            const RoomData& rm = rooms[ri];
            bool wasVisited = (ri < 32) ? ((visited & (1u << ri)) != 0) : false;
            bool isCatchAll = (ri == catchAllIdx);
            // Only draw rooms that were actually rendered (visited by portal DFS or catch-all)
            if (!wasVisited && !isCatchAll) continue;
            psyqo::Color baseColor = isCatchAll
                ? psyqo::Color{.r = 128, .g = 128, .b = 128}  // catch-all: gray
                : roomColors[ri % NUM_COLORS];

            for (int ti = 0; ti < rm.triRefCount; ti++) {
                const TriangleRef& ref = roomTriRefs[rm.firstTriRef + ti];
                if (ref.objectIndex >= objects.size()) continue;
                GameObject* obj = objects[ref.objectIndex];
                if (ref.triangleIndex >= obj->polyCount) continue;

                Tri& tri = obj->polygons[ref.triangleIndex];
                setupObjectTransform(obj, cameraPosition);

                // GTE transform
                writeSafe<PseudoRegister::V0>(tri.v0);
                writeSafe<PseudoRegister::V1>(tri.v1);
                writeSafe<PseudoRegister::V2>(tri.v2);
                Kernels::rtpt();

                uint32_t u0, u1, u2;
                read<Register::SZ1>(&u0);
                read<Register::SZ2>(&u1);
                read<Register::SZ3>(&u2);
                int32_t sz0 = (int32_t)u0, sz1 = (int32_t)u1, sz2 = (int32_t)u2;
                if (sz0 < 1 && sz1 < 1 && sz2 < 1) continue;
                int32_t zMax = eastl::max(eastl::max(sz0, sz1), sz2);
                if (zMax < 0 || zMax >= (int32_t)ORDERING_TABLE_SIZE) continue;

                psyqo::Vertex projected[3];
                read<Register::SXY0>(&projected[0].packed);
                read<Register::SXY1>(&projected[1].packed);
                read<Register::SXY2>(&projected[2].packed);
                if (isCompletelyOutside(projected[0], projected[1], projected[2])) continue;
                clampForRasterizer(projected[0]);
                clampForRasterizer(projected[1]);
                clampForRasterizer(projected[2]);

                // Depth-modulate brightness: near = full color, far = dimmer.
                // Also dim triangles from rooms that were NOT visited (culled).
                int32_t avgSZ = (sz0 + sz1 + sz2) / 3;
                int32_t bright = 4096 - (avgSZ * 4096 / (int32_t)ORDERING_TABLE_SIZE);
                if (bright < 512) bright = 512;
                if (bright > 4096) bright = 4096;

                psyqo::Color c = {
                    .r = (uint8_t)((baseColor.r * bright) >> 12),
                    .g = (uint8_t)((baseColor.g * bright) >> 12),
                    .b = (uint8_t)((baseColor.b * bright) >> 12),
                };

                auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
                p.primitive.pointA = projected[0];
                p.primitive.pointB = projected[1];
                p.primitive.pointC = projected[2];
                p.primitive.setColorA(c);
                p.primitive.setColorB(c);
                p.primitive.setColorC(c);
                p.primitive.setSemiTrans();
                ot.insert(p, 0);
            }
        }

        // Portal outlines: project portal quad and draw edges as thin lines.
        for (int p = 0; p < portalCount; p++) {
            const PortalData& portal = portals[p];

            int32_t rwx = ((int32_t)portal.rightX * portal.halfW) >> 12;
            int32_t rwy = ((int32_t)portal.rightY * portal.halfW) >> 12;
            int32_t rwz = ((int32_t)portal.rightZ * portal.halfW) >> 12;
            int32_t uhx = ((int32_t)portal.upX * portal.halfH) >> 12;
            int32_t uhy = ((int32_t)portal.upY * portal.halfH) >> 12;
            int32_t uhz = ((int32_t)portal.upZ * portal.halfH) >> 12;

            int32_t cx = portal.centerX, cy = portal.centerY, cz = portal.centerZ;
            struct { int32_t wx, wy, wz; } corners[4] = {
                {cx + rwx + uhx, cy + rwy + uhy, cz + rwz + uhz},
                {cx - rwx + uhx, cy - rwy + uhy, cz - rwz + uhz},
                {cx - rwx - uhx, cy - rwy - uhy, cz - rwz - uhz},
                {cx + rwx - uhx, cy + rwy - uhy, cz + rwz - uhz},
            };

            int16_t sx[4], sy[4];
            bool vis[4];
            int visCount = 0;
            for (int i = 0; i < 4; i++) {
                int32_t vx, vy, vz;
                worldToCamera(corners[i].wx, corners[i].wy, corners[i].wz,
                              camX, camY, camZ, camRot, vx, vy, vz);
                vis[i] = projectToScreen(vx, vy, vz, projH, sx[i], sy[i]);
                if (vis[i]) visCount++;
            }
            if (visCount < 2) continue;

            bool portalActive = (visited & (1u << portal.roomA)) || (visited & (1u << portal.roomB));
            psyqo::Color lineColor = portalActive ?
                psyqo::Color{.r = 255, .g = 255, .b = 255} :
                psyqo::Color{.r = 100, .g = 80, .b = 0};

            for (int i = 0; i < 4; i++) {
                int j = (i + 1) % 4;
                if (!vis[i] || !vis[j]) continue;
                int16_t x0 = sx[i], y0 = sy[i], x1 = sx[j], y1 = sy[j];
                if (x0 < 0) x0 = 0; if (x0 > 319) x0 = 319;
                if (y0 < 0) y0 = 0; if (y0 > 239) y0 = 239;
                if (x1 < 0) x1 = 0; if (x1 > 319) x1 = 319;
                if (y1 < 0) y1 = 0; if (y1 > 239) y1 = 239;

                auto& tri = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
                tri.primitive.pointA.x = x0; tri.primitive.pointA.y = y0;
                tri.primitive.pointB.x = x1; tri.primitive.pointB.y = y1;
                tri.primitive.pointC.x = x1; tri.primitive.pointC.y = (int16_t)(y1 + 1);
                tri.primitive.setColorA(lineColor);
                tri.primitive.setColorB(lineColor);
                tri.primitive.setColorC(lineColor);
                tri.primitive.setOpaque();
                ot.insert(tri, 0);
            }
        }

        // Room AABB outlines
        for (int r = 0; r < roomCount - 1 && r < 8; r++) {
            bool rendered = (visited & (1u << r)) != 0;
            psyqo::Color boxColor = rendered ?
                roomColors[r % NUM_COLORS] : psyqo::Color{.r = 60, .g = 60, .b = 60};

            const RoomData& rm = rooms[r];
            int32_t bmin[3] = {rm.aabbMinX, rm.aabbMinY, rm.aabbMinZ};
            int32_t bmax[3] = {rm.aabbMaxX, rm.aabbMaxY, rm.aabbMaxZ};

            int16_t csx[8], csy[8];
            bool cvis[8];
            int cvisCount = 0;
            for (int i = 0; i < 8; i++) {
                int32_t wx = (i & 1) ? bmax[0] : bmin[0];
                int32_t wy = (i & 2) ? bmax[1] : bmin[1];
                int32_t wz = (i & 4) ? bmax[2] : bmin[2];
                int32_t vx, vy, vz;
                worldToCamera(wx, wy, wz, camX, camY, camZ, camRot, vx, vy, vz);
                cvis[i] = projectToScreen(vx, vy, vz, projH, csx[i], csy[i]);
                if (cvis[i]) cvisCount++;
            }
            if (cvisCount < 2) continue;

            static const int edges[12][2] = {
                {0,1},{2,3},{4,5},{6,7},
                {0,2},{1,3},{4,6},{5,7},
                {0,4},{1,5},{2,6},{3,7},
            };
            for (int e = 0; e < 12; e++) {
                int a = edges[e][0], b = edges[e][1];
                if (!cvis[a] || !cvis[b]) continue;
                int16_t x0 = csx[a], y0 = csy[a], x1 = csx[b], y1 = csy[b];
                if (x0 < 0) x0 = 0; if (x0 > 319) x0 = 319;
                if (y0 < 0) y0 = 0; if (y0 > 239) y0 = 239;
                if (x1 < 0) x1 = 0; if (x1 > 319) x1 = 319;
                if (y1 < 0) y1 = 0; if (y1 > 239) y1 = 239;

                auto& tri = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
                tri.primitive.pointA.x = x0; tri.primitive.pointA.y = y0;
                tri.primitive.pointB.x = x1; tri.primitive.pointB.y = y1;
                tri.primitive.pointC.x = x1; tri.primitive.pointC.y = (int16_t)(y1 + 1);
                tri.primitive.setColorA(boxColor);
                tri.primitive.setColorB(boxColor);
                tri.primitive.setColorC(boxColor);
                tri.primitive.setOpaque();
                ot.insert(tri, 0);
            }
        }
    }
#endif

    // Render dynamically-moved objects that bypass room/portal culling
    for (size_t oi = 0; oi < objects.size(); oi++) {
        GameObject* obj = objects[oi];
        if (!obj->isActive() || !obj->isDynamicMoved()) continue;
        if (obj->isSkinned()) continue;
        BVHNode objBox;
        objBox.minX = obj->aabbMinX; objBox.minY = obj->aabbMinY; objBox.minZ = obj->aabbMinZ;
        objBox.maxX = obj->aabbMaxX; objBox.maxY = obj->aabbMaxY; objBox.maxZ = obj->aabbMaxZ;
        if (!frustum.testAABB(objBox)) continue;
        setupObjectTransform(obj, cameraPosition);
        for (int t = 0; t < obj->polyCount; t++) {
            processTriangle(obj->polygons[t], fogFarSZ, ot, balloc);
        }
    }

    renderSkinnedObjects(objects, cameraPosition, fogFarSZ, ot, balloc, &frustum);

    if (m_uiSystem) m_uiSystem->renderOT(m_gpu, ot, balloc);
#ifdef PSXSPLASH_MEMOVERLAY
    if (m_memOverlay) m_memOverlay->renderOT(ot, balloc);
#endif
    m_gpu.getNextClear(clear.primitive, m_clearcolor);
    m_gpu.chain(clear); m_gpu.chain(ot);
    if (m_uiSystem) m_uiSystem->renderText(m_gpu);
#ifdef PSXSPLASH_MEMOVERLAY
    if (m_memOverlay) m_memOverlay->renderText(m_gpu);
#endif
    m_frameCount++;
}

// ============================================================================
// Skinned mesh rendering - per-vertex bone transforms via rtps()
// ============================================================================

void psxsplash::Renderer::renderSkinnedObjects(
    eastl::vector<GameObject*>& objects,
    const psyqo::Vec3& cameraPosition,
    int32_t fogFarSZ,
    psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot,
    psyqo::BumpAllocator<BUMP_ALLOCATOR_SIZE>& balloc,
    const Frustum* frustum) {

    if (!m_skinSets || !m_skinStates || m_skinCount == 0) return;

    static psyqo::Matrix33 composedRots[SKINMESH_MAX_BONES];
    static psyqo::Vec3 composedTrans[SKINMESH_MAX_BONES];
    static BakedBoneMatrix lerpedBones[SKINMESH_MAX_BONES];

    for (int si = 0; si < m_skinCount; si++) {
        const SkinAnimSet& animSet = m_skinSets[si];
        const SkinAnimState& animState = m_skinStates[si];

        if (animSet.gameObjectIndex >= objects.size()) continue;
        GameObject* obj = objects[animSet.gameObjectIndex];
        if (!obj->isActive()) continue;
        if (animSet.clipCount == 0) continue;

        // Frustum cull using object AABB
        if (frustum) {
            BVHNode objBox;
            objBox.minX = obj->aabbMinX; objBox.minY = obj->aabbMinY; objBox.minZ = obj->aabbMinZ;
            objBox.maxX = obj->aabbMaxX; objBox.maxY = obj->aabbMaxY; objBox.maxZ = obj->aabbMaxZ;
            if (!frustum->testAABB(objBox)) continue;
        }

        // Get current clip and frame
        uint8_t clipIdx = animState.currentClip;
        if (clipIdx >= animSet.clipCount) clipIdx = 0;
        const SkinAnimClip& clip = animSet.clips[clipIdx];
        if (!clip.frames || clip.frameCount == 0) continue;

        uint16_t frame = animState.currentFrame;
        if (frame >= clip.frameCount) frame = clip.frameCount - 1;

        const BakedBoneMatrix* boneMatricesA = &clip.frames[(uint32_t)frame * animSet.boneCount];
        const BakedBoneMatrix* boneMatrices = boneMatricesA; // default: no interpolation

        // Interpolate between frames when subFrame > 0
        uint16_t sf = animState.subFrame;
        if (sf > 0 && frame + 1 < clip.frameCount) {
            const BakedBoneMatrix* boneMatricesB = &clip.frames[(uint32_t)(frame + 1) * animSet.boneCount];
            for (int bi = 0; bi < animSet.boneCount && bi < SKINMESH_MAX_BONES; bi++) {
                const BakedBoneMatrix& bA = boneMatricesA[bi];
                const BakedBoneMatrix& bB = boneMatricesB[bi];
                BakedBoneMatrix& out = lerpedBones[bi];
                for (int k = 0; k < 9; k++) {
                    int32_t a = bA.r[k], b = bB.r[k];
                    out.r[k] = (int16_t)(a + (((b - a) * sf) >> 12));
                }
                for (int k = 0; k < 3; k++) {
                    int32_t a = bA.t[k], b = bB.t[k];
                    out.t[k] = (int16_t)(a + (((b - a) * sf) >> 12));
                }
            }
            boneMatrices = lerpedBones;
        } else if (sf > 0 && (animState.loop || (clip.flags & 0x01)) && clip.frameCount > 1) {
            // Looping: interpolate last frame → first frame
            const BakedBoneMatrix* boneMatricesB = &clip.frames[0];
            for (int bi = 0; bi < animSet.boneCount && bi < SKINMESH_MAX_BONES; bi++) {
                const BakedBoneMatrix& bA = boneMatricesA[bi];
                const BakedBoneMatrix& bB = boneMatricesB[bi];
                BakedBoneMatrix& out = lerpedBones[bi];
                for (int k = 0; k < 9; k++) {
                    int32_t a = bA.r[k], b = bB.r[k];
                    out.r[k] = (int16_t)(a + (((b - a) * sf) >> 12));
                }
                for (int k = 0; k < 3; k++) {
                    int32_t a = bA.t[k], b = bB.t[k];
                    out.t[k] = (int16_t)(a + (((b - a) * sf) >> 12));
                }
            }
            boneMatrices = lerpedBones;
        }

        // Compose camera × object rotation
        psyqo::Matrix33 camObjRot;
        MatrixMultiplyGTE(m_currentCamera->GetRotation(), obj->rotation, &camObjRot);

        // Compute camera-space object position
        ::clear<Register::TRX, Safe>();
        ::clear<Register::TRY, Safe>();
        ::clear<Register::TRZ, Safe>();
        writeSafe<PseudoRegister::Rotation>(m_currentCamera->GetRotation());
        writeSafe<PseudoRegister::V0>(obj->position);
        Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
        psyqo::Vec3 objCamPos = readSafe<PseudoRegister::SV>();
        objCamPos.x += cameraPosition.x;
        objCamPos.y += cameraPosition.y;
        objCamPos.z += cameraPosition.z;

        // Pre-compose per-bone rotation matrices and translations
        for (int bi = 0; bi < animSet.boneCount && bi < SKINMESH_MAX_BONES; bi++) {
            const BakedBoneMatrix& bm = boneMatrices[bi];

            psyqo::Matrix33 boneRot;
            boneRot.vs[0].x.value = bm.r[0]; boneRot.vs[0].y.value = bm.r[1]; boneRot.vs[0].z.value = bm.r[2];
            boneRot.vs[1].x.value = bm.r[3]; boneRot.vs[1].y.value = bm.r[4]; boneRot.vs[1].z.value = bm.r[5];
            boneRot.vs[2].x.value = bm.r[6]; boneRot.vs[2].y.value = bm.r[7]; boneRot.vs[2].z.value = bm.r[8];

            // composedRots[bi] = camObjRot × boneRot
            MatrixMultiplyGTE(camObjRot, boneRot, &composedRots[bi]);

            // composedTrans[bi] = objCamPos + camObjRot × boneTrans
            psyqo::Vec3 boneTrans;
            boneTrans.x.value = bm.t[0]; boneTrans.y.value = bm.t[1]; boneTrans.z.value = bm.t[2];

            writeSafe<PseudoRegister::Rotation>(camObjRot);
            ::clear<Register::TRX, Safe>();
            ::clear<Register::TRY, Safe>();
            ::clear<Register::TRZ, Safe>();
            writeSafe<PseudoRegister::V0>(boneTrans);
            Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
            psyqo::Vec3 rotatedTrans = readSafe<PseudoRegister::SV>();

            composedTrans[bi].x.value = objCamPos.x.value + rotatedTrans.x.value;
            composedTrans[bi].y.value = objCamPos.y.value + rotatedTrans.y.value;
            composedTrans[bi].z.value = objCamPos.z.value + rotatedTrans.z.value;
        }

        const uint8_t* boneIdx = animSet.boneIndices;
        for (int ti = 0; ti < animSet.polyCount; ti++) {
            Tri& tri = animSet.polygons[ti];

            uint8_t bA = boneIdx ? boneIdx[ti * 3 + 0] : 0;
            uint8_t bB = boneIdx ? boneIdx[ti * 3 + 1] : 0;
            uint8_t bC = boneIdx ? boneIdx[ti * 3 + 2] : 0;
            if (bA >= animSet.boneCount) bA = 0;
            if (bB >= animSet.boneCount) bB = 0;
            if (bC >= animSet.boneCount) bC = 0;

            // Per-vertex rtps (3 calls instead of rtpt) due to different bone matrices
            // Vertex A
            writeSafe<PseudoRegister::Rotation>(composedRots[bA]);
            writeSafe<PseudoRegister::Translation>(composedTrans[bA]);
            writeSafe<PseudoRegister::V0>(tri.v0);
            Kernels::rtps();
            uint32_t sz0Raw; read<Register::SZ3>(&sz0Raw);
            psyqo::Vertex projected0;
            read<Register::SXY2>(&projected0.packed);

            // Vertex B
            writeSafe<PseudoRegister::Rotation>(composedRots[bB]);
            writeSafe<PseudoRegister::Translation>(composedTrans[bB]);
            writeSafe<PseudoRegister::V0>(tri.v1);
            Kernels::rtps();
            uint32_t sz1Raw; read<Register::SZ3>(&sz1Raw);
            psyqo::Vertex projected1;
            read<Register::SXY2>(&projected1.packed);

            // Vertex C
            writeSafe<PseudoRegister::Rotation>(composedRots[bC]);
            writeSafe<PseudoRegister::Translation>(composedTrans[bC]);
            writeSafe<PseudoRegister::V0>(tri.v2);
            Kernels::rtps();
            uint32_t sz2Raw; read<Register::SZ3>(&sz2Raw);
            psyqo::Vertex projected2;
            read<Register::SXY2>(&projected2.packed);

            int32_t sz0 = (int32_t)sz0Raw, sz1 = (int32_t)sz1Raw, sz2 = (int32_t)sz2Raw;

            // All behind camera → invisible
            if (sz0 <= 0 && sz1 <= 0 && sz2 <= 0) continue;
            // Beyond fog wall → invisible
            if (fogFarSZ > 0 && sz0 > fogFarSZ && sz1 > fogFarSZ && sz2 > fogFarSZ) continue;

            int32_t zIndex = eastl::max(eastl::max(sz0, sz1), sz2);
            if (zIndex >= (int32_t)ORDERING_TABLE_SIZE) continue;
            if (zIndex < 1) zIndex = 1;

            // Skip near-plane vertices (no subdivision for skinned meshes)
            static constexpr int32_t NEAR_SZ_THRESHOLD = 4;
            if (sz0 < NEAR_SZ_THRESHOLD || sz1 < NEAR_SZ_THRESHOLD ||
                sz2 < NEAR_SZ_THRESHOLD) continue;

            // Off-screen reject
            if (isCompletelyOutside(projected0, projected1, projected2)) continue;

            // Backface cull (SXY FIFO holds A,B,C after 3 sequential rtps calls)
            Kernels::nclip();
            int32_t mac0 = 0;
            read<Register::MAC0>(reinterpret_cast<uint32_t*>(&mac0));
            if (mac0 <= 0) continue;

            clampForRasterizer(projected0);
            clampForRasterizer(projected1);
            clampForRasterizer(projected2);

            // Per-vertex fog
            psyqo::Color cA = tri.colorA, cB = tri.colorB, cC = tri.colorC;
            bool hasFog = false;
            int32_t fogIR[3] = {0, 0, 0};
            if (m_fog.enabled && fogFarSZ > 0) {
                int32_t fogNear = fogFarSZ >> 3;
                int32_t range = fogFarSZ - fogNear;
                if (range < 1) range = 1;
                int32_t szArr[3] = {sz0, sz1, sz2};
                for (int vi = 0; vi < 3; vi++) {
                    if (szArr[vi] <= fogNear) fogIR[vi] = 0;
                    else if (szArr[vi] >= fogFarSZ) fogIR[vi] = 4096;
                    else fogIR[vi] = ((szArr[vi] - fogNear) * 4096) / range;
                }
                hasFog = (fogIR[0] > 0 || fogIR[1] > 0 || fogIR[2] > 0);
            }

            // Emit GPU primitives
            if (tri.isUntextured()) {
                if (hasFog) {
                    cA = fogBlend(cA, fogIR[0], m_fog.color);
                    cB = fogBlend(cB, fogIR[1], m_fog.color);
                    cC = fogBlend(cC, fogIR[2], m_fog.color);
                }
                auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
                p.primitive.pointA = projected0; p.primitive.pointB = projected1; p.primitive.pointC = projected2;
                p.primitive.setColorA(cA); p.primitive.setColorB(cB); p.primitive.setColorC(cC);
                p.primitive.setOpaque();
                ot.insert(p, zIndex);
            } else if (hasFog) {
                psyqo::Color fogA = fogBaseColor(fogIR[0], m_fog.color);
                psyqo::Color fogB = fogBaseColor(fogIR[1], m_fog.color);
                psyqo::Color fogC = fogBaseColor(fogIR[2], m_fog.color);
                psyqo::Color texA = fogTexColor(cA, fogIR[0]);
                psyqo::Color texB = fogTexColor(cB, fogIR[1]);
                psyqo::Color texC = fogTexColor(cC, fogIR[2]);

                auto& fogP = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
                fogP.primitive.pointA = projected0; fogP.primitive.pointB = projected1; fogP.primitive.pointC = projected2;
                fogP.primitive.setColorA(fogA); fogP.primitive.setColorB(fogB); fogP.primitive.setColorC(fogC);
                fogP.primitive.setSemiTrans();
                ot.insert(fogP, zIndex);

                auto& texP = balloc.allocateFragment<psyqo::Prim::GouraudTexturedTriangle>();
                texP.primitive.pointA = projected0; texP.primitive.pointB = projected1; texP.primitive.pointC = projected2;
                texP.primitive.uvA = tri.uvA; texP.primitive.uvB = tri.uvB;
                texP.primitive.uvC.u = tri.uvC.u; texP.primitive.uvC.v = tri.uvC.v;
                texP.primitive.tpage = tri.tpage;
                texP.primitive.tpage.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
                psyqo::PrimPieces::ClutIndex clut(tri.clutX, tri.clutY);
                texP.primitive.clutIndex = clut;
                texP.primitive.setColorA(texA); texP.primitive.setColorB(texB); texP.primitive.setColorC(texC);
                texP.primitive.setOpaque();
                ot.insert(texP, zIndex);
            } else {
                auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTexturedTriangle>();
                p.primitive.pointA = projected0; p.primitive.pointB = projected1; p.primitive.pointC = projected2;
                p.primitive.uvA = tri.uvA; p.primitive.uvB = tri.uvB;
                p.primitive.uvC.u = tri.uvC.u; p.primitive.uvC.v = tri.uvC.v;
                p.primitive.tpage = tri.tpage;
                p.primitive.tpage.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
                psyqo::PrimPieces::ClutIndex clut(tri.clutX, tri.clutY);
                p.primitive.clutIndex = clut;
                p.primitive.setColorA(cA); p.primitive.setColorB(cB); p.primitive.setColorC(cC);
                p.primitive.setOpaque();
                ot.insert(p, zIndex);
            }
        }
    }
}

void psxsplash::Renderer::VramUpload(const uint16_t* imageData, int16_t posX,
                                     int16_t posY, int16_t width, int16_t height) {
    psyqo::Rect uploadRect{.a = {.x = posX, .y = posY}, .b = {width, height}};
    m_gpu.uploadToVRAM(imageData, uploadRect);
}
