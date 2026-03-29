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

void psxsplash::Renderer::SetCamera(psxsplash::Camera& camera) { m_currentCamera = &camera; }

void psxsplash::Renderer::SetFog(const FogConfig& fog) {
    m_fog = fog;
    // Always use fog color as the GPU clear/back color
    m_clearcolor = fog.color;
    if (fog.enabled) {
        write<Register::RFC, Unsafe>(static_cast<uint32_t>(fog.color.r) << 4);
        write<Register::GFC, Unsafe>(static_cast<uint32_t>(fog.color.g) << 4);
        write<Register::BFC, Safe>(static_cast<uint32_t>(fog.color.b) << 4);
        m_fog.fogFarSZ = 8000 / fog.density;
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

// Per-vertex fog blend: result = vertexColor * (4096 - ir0) / 4096 + fogColor * ir0 / 4096
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

// ============================================================================
// Core triangle pipeline (Bandwidth's proven approach + fog)
// rtpt -> nclip -> backface cull -> SZ depth -> SXY -> screen clip -> emit
// ============================================================================

void psxsplash::Renderer::processTriangle(
    Tri& tri, int32_t fogFarSZ,
    psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot,
    psyqo::BumpAllocator<BUMP_ALLOCATOR_SIZE>& balloc) {

    writeSafe<PseudoRegister::V0>(tri.v0);
    writeSafe<PseudoRegister::V1>(tri.v1);
    writeSafe<PseudoRegister::V2>(tri.v2);

    Kernels::rtpt();

    uint32_t u0, u1, u2;
    read<Register::SZ1>(&u0);
    read<Register::SZ2>(&u1);
    read<Register::SZ3>(&u2);
    int32_t sz0 = (int32_t)u0, sz1 = (int32_t)u1, sz2 = (int32_t)u2;

    if (sz0 < 1 && sz1 < 1 && sz2 < 1) return;
    if (fogFarSZ > 0 && sz0 > fogFarSZ && sz1 > fogFarSZ && sz2 > fogFarSZ) return;

    int32_t zIndex = eastl::max(eastl::max(sz0, sz1), sz2);
    if (zIndex < 0 || zIndex >= (int32_t)ORDERING_TABLE_SIZE) return;

    // Per-vertex fog: compute fog factor for each vertex individually based on
    // its SZ depth. The GPU then interpolates the fogged colors smoothly across
    // the triangle surface, eliminating the per-triangle tiling artifacts that
    // occur when a single IR0 is used for the whole triangle.
    //
    // fogIR[i] = 0 means no fog (original color), 4096 = full fog (fog color).
    // Quadratic ease-in curve: fog dominates over baked lighting quickly.
    int32_t fogIR[3] = {0, 0, 0};
    if (fogFarSZ > 0) {
        int32_t fogNear = fogFarSZ / 4;
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
                // Linear 0..4096 over [fogNear, fogFarSZ]
                int32_t t = ((szArr[vi] - fogNear) * 4096) / range;
                // Quadratic ease-in: t^2 / 4096
                ir = (t * t) >> 12;
            }
            fogIR[vi] = ir;
        }
    }

    psyqo::Vertex projected[3];
    read<Register::SXY0>(&projected[0].packed);
    read<Register::SXY1>(&projected[1].packed);
    read<Register::SXY2>(&projected[2].packed);

    if (isCompletelyOutside(projected[0], projected[1], projected[2])) return;

    // Triangles that need clipping skip nclip entirely.
    // nclip with GTE-clamped screen coords gives wrong results for edge triangles.
    // The clipper handles them directly - no backface cull needed since the
    // clipper preserves winding and degenerate triangles produce zero-area output.
    if (needsClipping(projected[0], projected[1], projected[2])) {
        ClipVertex cv0 = {(int16_t)projected[0].x, (int16_t)projected[0].y, (int16_t)sz0,
                          tri.uvA.u, tri.uvA.v, tri.colorA.r, tri.colorA.g, tri.colorA.b};
        ClipVertex cv1 = {(int16_t)projected[1].x, (int16_t)projected[1].y, (int16_t)sz1,
                          tri.uvB.u, tri.uvB.v, tri.colorB.r, tri.colorB.g, tri.colorB.b};
        ClipVertex cv2 = {(int16_t)projected[2].x, (int16_t)projected[2].y, (int16_t)sz2,
                          tri.uvC.u, tri.uvC.v, tri.colorC.r, tri.colorC.g, tri.colorC.b};
        ClipResult clipResult;
        int clippedCount = clipTriangle(cv0, cv1, cv2, clipResult);
        for (int ct = 0; ct < clippedCount; ct++) {
            const ClipVertex& a = clipResult.verts[ct*3];
            const ClipVertex& b = clipResult.verts[ct*3+1];
            const ClipVertex& c = clipResult.verts[ct*3+2];
            // For clipped vertices, use per-triangle fog (max SZ) since
            // clipped vertex Z values may not map cleanly to the original SZs.
            psyqo::Color ca = {a.r, a.g, a.b}, cb = {b.r, b.g, b.b}, cc = {c.r, c.g, c.b};
            if (m_fog.enabled) {
                int32_t maxIR = eastl::max(eastl::max(fogIR[0], fogIR[1]), fogIR[2]);
                ca = fogBlend(ca, maxIR, m_fog.color);
                cb = fogBlend(cb, maxIR, m_fog.color);
                cc = fogBlend(cc, maxIR, m_fog.color);
            }
            if (tri.isUntextured()) {
                auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
                p.primitive.pointA.x = a.x; p.primitive.pointA.y = a.y;
                p.primitive.pointB.x = b.x; p.primitive.pointB.y = b.y;
                p.primitive.pointC.x = c.x; p.primitive.pointC.y = c.y;
                p.primitive.setColorA(ca); p.primitive.setColorB(cb); p.primitive.setColorC(cc);
                p.primitive.setOpaque();
                ot.insert(p, zIndex);
            } else {
                auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTexturedTriangle>();
                p.primitive.pointA.x = a.x; p.primitive.pointA.y = a.y;
                p.primitive.pointB.x = b.x; p.primitive.pointB.y = b.y;
                p.primitive.pointC.x = c.x; p.primitive.pointC.y = c.y;
                p.primitive.uvA.u = a.u; p.primitive.uvA.v = a.v;
                p.primitive.uvB.u = b.u; p.primitive.uvB.v = b.v;
                p.primitive.uvC.u = c.u; p.primitive.uvC.v = c.v;
                p.primitive.tpage = tri.tpage;
                psyqo::PrimPieces::ClutIndex clut(tri.clutX, tri.clutY);
                p.primitive.clutIndex = clut;
                p.primitive.setColorA(ca); p.primitive.setColorB(cb); p.primitive.setColorC(cc);
                p.primitive.setOpaque();
                ot.insert(p, zIndex);
            }
        }
        return;
    }

    // Normal path: triangle is fully inside clip region with safe deltas.
    // nclip is reliable here since screen coords aren't clamped.
    Kernels::nclip();
    int32_t mac0 = 0;
    read<Register::MAC0>(reinterpret_cast<uint32_t*>(&mac0));
    if (mac0 <= 0) return;

    // Per-vertex fog: blend each vertex color toward fog color based on its depth.
    // GPU interpolates these smoothly across the triangle - no tiling artifacts.
    psyqo::Color cA = tri.colorA, cB = tri.colorB, cC = tri.colorC;
    if (m_fog.enabled) {
        cA = fogBlend(cA, fogIR[0], m_fog.color);
        cB = fogBlend(cB, fogIR[1], m_fog.color);
        cC = fogBlend(cC, fogIR[2], m_fog.color);
    }

    if (tri.isUntextured()) {
        auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
        p.primitive.pointA = projected[0]; p.primitive.pointB = projected[1]; p.primitive.pointC = projected[2];
        p.primitive.setColorA(cA); p.primitive.setColorB(cB); p.primitive.setColorC(cC);
        p.primitive.setOpaque();
        ot.insert(p, zIndex);
    } else {
        auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTexturedTriangle>();
        p.primitive.pointA = projected[0]; p.primitive.pointB = projected[1]; p.primitive.pointC = projected[2];
        p.primitive.uvA = tri.uvA; p.primitive.uvB = tri.uvB; p.primitive.uvC = tri.uvC;
        p.primitive.tpage = tri.tpage;
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
    uint8_t parity = m_gpu.getParity();
    auto& ot = m_ots[parity]; auto& clear = m_clear[parity]; auto& balloc = m_ballocs[parity];
    balloc.reset();
    // Set dithering draw mode at the back of the OT so it fires before any geometry.
    auto& ditherCmd = balloc.allocateFragment<psyqo::Prim::TPage>();
    ditherCmd.primitive.attr.setDithering(true);
    ot.insert(ditherCmd, ORDERING_TABLE_SIZE - 1);

    psyqo::Vec3 cameraPosition = computeCameraViewPos();
    int32_t fogFarSZ = m_fog.fogFarSZ;
    for (auto& obj : objects) {
        if (!obj->isActive()) continue;
        setupObjectTransform(obj, cameraPosition);
        for (int i = 0; i < obj->polyCount; i++)
            processTriangle(obj->polygons[i], fogFarSZ, ot, balloc);
    }
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
    uint8_t parity = m_gpu.getParity();
    auto& ot = m_ots[parity]; auto& clear = m_clear[parity]; auto& balloc = m_ballocs[parity];
    balloc.reset();
    auto& ditherCmd2 = balloc.allocateFragment<psyqo::Prim::TPage>();
    ditherCmd2.primitive.attr.setDithering(true);
    ot.insert(ditherCmd2, ORDERING_TABLE_SIZE - 1);

    Frustum frustum; m_currentCamera->ExtractFrustum(frustum);
    int visibleCount = bvh.cullFrustum(frustum, m_visibleRefs, MAX_VISIBLE_TRIANGLES);
    psyqo::Vec3 cameraPosition = computeCameraViewPos();
    int32_t fogFarSZ = m_fog.fogFarSZ;
    int16_t lastObjectIndex = -1;
    for (int i = 0; i < visibleCount; i++) {
        const TriangleRef& ref = m_visibleRefs[i];
        if (ref.objectIndex >= objects.size()) continue;
        GameObject* obj = objects[ref.objectIndex];
        if (!obj->isActive()) continue;
        if (ref.triangleIndex >= obj->polyCount) continue;
        if (ref.objectIndex != lastObjectIndex) {
            lastObjectIndex = ref.objectIndex;
            setupObjectTransform(obj, cameraPosition);
        }
        processTriangle(obj->polygons[ref.triangleIndex], fogFarSZ, ot, balloc);
    }
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

// Project a camera-space point to screen coordinates.
// Returns false if behind near plane.
static inline bool projectToScreen(int32_t vx, int32_t vy, int32_t vz,
    int16_t& sx, int16_t& sy) {
    if (vz <= 0) return false;
    constexpr int32_t H = 120;
    int32_t vzs = vz >> 4; if (vzs <= 0) vzs = 1;
    sx = (int16_t)((vx >> 4) * H / vzs + 160);
    sy = (int16_t)((vy >> 4) * H / vzs + 120);
    return true;
}

// Project a portal quad to a screen-space AABB.
// Computes the 4 corners, transforms to camera space, clips against the near plane,
// projects visible points to screen, and returns the bounding rect.
static bool projectPortalRect(const psxsplash::PortalData& portal,
    int32_t camX, int32_t camY, int32_t camZ, const psyqo::Matrix33& camRot, ScreenRect& outRect) {

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

    if (behindCount == 4) {
        // All corners behind camera. Only allow if camera is very close to portal.
        int32_t vx, vy, vz;
        worldToCamera(cx, cy, cz, camX, camY, camZ, camRot, vx, vy, vz);
        int32_t portalExtent = portal.halfW > portal.halfH ? portal.halfW : portal.halfH;
        if (-vz > portalExtent * 2) return false;
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
            if (projectToScreen(cv[i].x, cv[i].y, cv[i].z, sx, sy)) {
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
            if (projectToScreen(clipX, clipY, NEAR_Z, sx, sy)) {
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

void psxsplash::Renderer::RenderWithRooms(eastl::vector<GameObject*>& objects,
    const RoomData* rooms, int roomCount, const PortalData* portals, int portalCount,
    const TriangleRef* roomTriRefs, int cameraRoom) {
    psyqo::Kernel::assert(m_currentCamera != nullptr, "PSXSPLASH: Tried to render without an active camera");
    if (roomCount == 0 || rooms == nullptr) { Render(objects); return; }

    uint8_t parity = m_gpu.getParity();
    auto& ot = m_ots[parity]; auto& clear = m_clear[parity]; auto& balloc = m_ballocs[parity];
    balloc.reset();
    auto& ditherCmd3 = balloc.allocateFragment<psyqo::Prim::TPage>();
    ditherCmd3.primitive.attr.setDithering(true);
    ot.insert(ditherCmd3, ORDERING_TABLE_SIZE - 1);

    psyqo::Vec3 cameraPosition = computeCameraViewPos();
    int32_t fogFarSZ = m_fog.fogFarSZ;
    int32_t camX = m_currentCamera->GetPosition().x.raw();
    int32_t camY = m_currentCamera->GetPosition().y.raw();
    int32_t camZ = m_currentCamera->GetPosition().z.raw();
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

    auto renderRoom = [&](int ri) {
        const RoomData& rm = rooms[ri];
        int16_t lastObj = -1;
        for (int ti = 0; ti < rm.triRefCount; ti++) {
            const TriangleRef& ref = roomTriRefs[rm.firstTriRef + ti];
            if (ref.objectIndex >= objects.size()) continue;
            GameObject* obj = objects[ref.objectIndex];
            if (!obj->isActive()) continue;
            if (ref.triangleIndex >= obj->polyCount) continue;
            if (ref.objectIndex != lastObj) { lastObj = ref.objectIndex; setupObjectTransform(obj, cameraPosition); }
            processTriangle(obj->polygons[ref.triangleIndex], fogFarSZ, ot, balloc);
        }
    };

    // Always render catch-all room (geometry not assigned to any specific room)
    renderRoom(catchAllIdx);

    if (cameraRoom >= 0) {
        ScreenRect full = {-512, -512, 832, 752};
        if (cameraRoom < 32) visited |= (1u << cameraRoom);
        stack[top++] = {cameraRoom, 0, full};
        while (top > 0) {
            Entry e = stack[--top];
            renderRoom(e.room);
            if (e.depth >= 8) continue;  // Depth limit prevents infinite loops
            for (int p = 0; p < portalCount; p++) {
                int other = -1;
                if (portals[p].roomA == e.room) other = portals[p].roomB;
                else if (portals[p].roomB == e.room) other = portals[p].roomA;
                else continue;
                if (other < 0 || other >= roomCount) continue;
                if (other < 32 && (visited & (1u << other))) continue;

                // Backface cull: skip portals that face away from the camera.
                // The portal normal points from roomA toward roomB (4.12 fp).
                // dot(normal, cam - portalCenter) > 0 means the portal faces us when
                // traversing A->B; the sign flips when traversing B->A.
                {
                    int32_t dx = camX - portals[p].centerX;
                    int32_t dy = camY - portals[p].centerY;
                    int32_t dz = camZ - portals[p].centerZ;
                    int64_t dot = (int64_t)dx * portals[p].normalX +
                                  (int64_t)dy * portals[p].normalY +
                                  (int64_t)dz * portals[p].normalZ;
                    // Allow a small negative threshold so nearly-edge-on portals still pass.
                    const int64_t BACKFACE_THRESHOLD = -4096;
                    if (portals[p].roomA == e.room) {
                        if (dot < BACKFACE_THRESHOLD) continue;
                    } else {
                        if (dot > -BACKFACE_THRESHOLD) continue;
                    }
                }

                // Phase 4: Frustum-cull the destination room's AABB.
                // If the room is entirely behind the camera, skip.
                if (!isRoomPotentiallyVisible(rooms[other], camX, camY, camZ, camRot)) {
                    continue;
                }

                // Phase 2: Project actual portal quad corners to screen.
                ScreenRect pr;
                if (!projectPortalRect(portals[p], camX, camY, camZ, camRot, pr)) {
                    continue;
                }
                ScreenRect isect;
                if (!intersectRect(e.clip, pr, isect)) {
                    continue;
                }
                if (other < 32) visited |= (1u << other);
                if (top < 64) stack[top++] = {other, e.depth + 1, isect};
            }
        }
    } else {
        // Camera room unknown - render ALL rooms as safety fallback.
        // This guarantees no geometry disappears, at the cost of no culling.
        for (int r = 0; r < roomCount; r++) if (r != catchAllIdx) renderRoom(r);
    }

#ifdef PSXSPLASH_ROOM_DEBUG
    // ================================================================
    // Debug overlay: room status bars + portal outlines
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

        // Room status bars at top of screen
        for (int r = 0; r < roomCount && r < 8; r++) {
            bool rendered = (visited & (1u << r)) != 0;
            bool isCamRoom = (r == cameraRoom);
            auto& tile = balloc.allocateFragment<psyqo::Prim::FastFill>();
            int16_t x = r * 18 + 2;
            tile.primitive.setColor(rendered ?
                roomColors[r] : psyqo::Color{.r = 40, .g = 40, .b = 40});
            tile.primitive.rect = psyqo::Rect{
                .a = {.x = x, .y = (int16_t)2},
                .b = {.w = 14, .h = (int16_t)(isCamRoom ? 12 : 6)}
            };
            ot.insert(tile, 0);
        }

        // Portal outlines: project portal quad and draw edges as thin lines.
        // Lines are drawn at OT front (depth 0) so they show through walls.
        for (int p = 0; p < portalCount; p++) {
            const PortalData& portal = portals[p];

            // Compute portal corners in world space
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

            // Project corners to screen
            int16_t sx[4], sy[4];
            bool vis[4];
            int visCount = 0;
            for (int i = 0; i < 4; i++) {
                int32_t vx, vy, vz;
                worldToCamera(corners[i].wx, corners[i].wy, corners[i].wz,
                              camX, camY, camZ, camRot, vx, vy, vz);
                vis[i] = projectToScreen(vx, vy, vz, sx[i], sy[i]);
                if (vis[i]) visCount++;
            }
            if (visCount < 2) continue;  // Can't draw edges with <2 visible corners

            // Draw each edge as a degenerate triangle (line).
            // Color: orange for portal between visible rooms, dim for invisible.
            bool portalActive = (visited & (1u << portal.roomA)) || (visited & (1u << portal.roomB));
            psyqo::Color lineColor = portalActive ?
                psyqo::Color{.r = 255, .g = 160, .b = 0} :
                psyqo::Color{.r = 80, .g = 60, .b = 0};

            for (int i = 0; i < 4; i++) {
                int j = (i + 1) % 4;
                if (!vis[i] || !vis[j]) continue;
                // Clamp to screen to avoid GPU issues
                int16_t x0 = sx[i], y0 = sy[i], x1 = sx[j], y1 = sy[j];
                if (x0 < 0) x0 = 0; if (x0 > 319) x0 = 319;
                if (y0 < 0) y0 = 0; if (y0 > 239) y0 = 239;
                if (x1 < 0) x1 = 0; if (x1 > 319) x1 = 319;
                if (y1 < 0) y1 = 0; if (y1 > 239) y1 = 239;

                // Draw line as degenerate triangle (A=B=start, C=end gives a 1px line)
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

        // Room AABB outlines: project the 8 corners of each room's AABB and draw edges.
        for (int r = 0; r < roomCount - 1 && r < 8; r++) {
            bool rendered = (visited & (1u << r)) != 0;
            psyqo::Color boxColor = rendered ?
                roomColors[r] : psyqo::Color{.r = 60, .g = 60, .b = 60};

            const RoomData& rm = rooms[r];
            int32_t bmin[3] = {rm.aabbMinX, rm.aabbMinY, rm.aabbMinZ};
            int32_t bmax[3] = {rm.aabbMaxX, rm.aabbMaxY, rm.aabbMaxZ};

            // 8 corners of the AABB
            int16_t csx[8], csy[8];
            bool cvis[8];
            int cvisCount = 0;
            for (int i = 0; i < 8; i++) {
                int32_t wx = (i & 1) ? bmax[0] : bmin[0];
                int32_t wy = (i & 2) ? bmax[1] : bmin[1];
                int32_t wz = (i & 4) ? bmax[2] : bmin[2];
                int32_t vx, vy, vz;
                worldToCamera(wx, wy, wz, camX, camY, camZ, camRot, vx, vy, vz);
                cvis[i] = projectToScreen(vx, vy, vz, csx[i], csy[i]);
                if (cvis[i]) cvisCount++;
            }
            if (cvisCount < 2) continue;

            // Draw 12 AABB edges
            static const int edges[12][2] = {
                {0,1},{2,3},{4,5},{6,7},  // X-axis edges
                {0,2},{1,3},{4,6},{5,7},  // Y-axis edges
                {0,4},{1,5},{2,6},{3,7},  // Z-axis edges
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

void psxsplash::Renderer::VramUpload(const uint16_t* imageData, int16_t posX,
                                     int16_t posY, int16_t width, int16_t height) {
    psyqo::Rect uploadRect{.a = {.x = posX, .y = posY}, .b = {width, height}};
    m_gpu.uploadToVRAM(imageData, uploadRect);
}
