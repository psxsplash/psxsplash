#include "luautility.hh"
#include <psyqo/xprintf.h>
#include <psyqo/soft-math.hh>
#include <psyqo/trigonometry.hh>
#include <psyqo/fixed-point.hh>
#include "gtemath.hh"
#include <math.h>

namespace psxsplash {

psyqo::Vec3 LuaUtility::GetCorrectedDirectionVector(psyqo::Matrix33 rotation, uint8_t directionIndex)
{
    psyqo::Vec3 directionVector;

    psyqo::Matrix33 correctedRotation;
    correctedRotation = psxsplash::transposeMatrix33(rotation);

    directionVector.x = correctedRotation.vs[directionIndex].x;
    directionVector.y = correctedRotation.vs[directionIndex].y;
    directionVector.z = correctedRotation.vs[directionIndex].z;

    return directionVector;
}

psyqo::Vec3 LuaUtility::GetForward(psyqo::Matrix33 rotation) {
    return GetCorrectedDirectionVector(rotation,2);
}

psyqo::Vec3 LuaUtility::GetBackward(psyqo::Matrix33 rotation) {
    return GetCorrectedDirectionVector(rotation,2) * -1;
}

psyqo::Vec3 LuaUtility::GetLeft(psyqo::Matrix33 rotation) {
    return GetCorrectedDirectionVector(rotation,0) * -1;
}

psyqo::Vec3 LuaUtility::GetRight(psyqo::Matrix33 rotation) {
    return GetCorrectedDirectionVector(rotation,0);
}

psyqo::Vec3 LuaUtility::GetUp(psyqo::Matrix33 rotation) {
    return GetCorrectedDirectionVector(rotation,1) * -1;
}

psyqo::Vec3 LuaUtility::GetDown(psyqo::Matrix33 rotation) {
    return GetCorrectedDirectionVector(rotation,1);
}


void LuaUtility::SetPosition(GameObject* go, psyqo::FixedPoint<12> x, psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z) {
        // Compute position delta to shift the world-space AABB
    int32_t dx = x.value - go->position.x.value;
    int32_t dy = y.value - go->position.y.value;
    int32_t dz = z.value - go->position.z.value;
    
    go->position.x = x;
    go->position.y = y;
    go->position.z = z;
    
    // Shift AABB by the position delta so frustum culling uses correct bounds
    go->aabbMinX += dx; go->aabbMaxX += dx;
    go->aabbMinY += dy; go->aabbMaxY += dy;
    go->aabbMinZ += dz; go->aabbMaxZ += dz;
    
    // Mark as dynamically moved so the renderer knows to bypass BVH for this object
    go->setDynamicMoved(true);
}

void LuaUtility::SetPosition(GameObject* go, psyqo::Vec3 newPos) {
     SetPosition(go, newPos.x,newPos.y,newPos.z);
}

psyqo::Angle LuaUtility::FastAtan2(int32_t sinVal, int32_t cosVal) {
    psyqo::Angle result;
    if (cosVal == 0 && sinVal == 0) { result.value = 0; return result; }

    int32_t abs_s = sinVal < 0 ? -sinVal : sinVal;
    int32_t abs_c = cosVal < 0 ? -cosVal : cosVal;

    int32_t minV = abs_s < abs_c ? abs_s : abs_c;
    int32_t maxV = abs_s > abs_c ? abs_s : abs_c;
    int32_t angle = (minV * 256) / maxV;

    if (abs_s > abs_c) angle = 512 - angle;
    if (cosVal < 0) angle = 1024 - angle;
    if (sinVal < 0) angle = -angle;

    result.value = angle;
    return result;
}

psyqo::FixedPoint<12> LuaUtility::ToFp12(psyqo::Angle a)
{
    psyqo::FixedPoint<12> fp;
    fp.value = a.value << 2;
    return fp;
}

}