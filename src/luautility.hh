#pragma once

#include <psyqo/matrix.hh>
#include <psyqo/vector.hh>
#include "gameobject.hh"
#include <psyqo/soft-math.hh>
#include <psyqo/fixed-point.hh>


namespace psxsplash { 

class LuaUtility {
  public:
    static psyqo::Vec3 GetCorrectedDirectionVector(psyqo::Matrix33 rotation, uint8_t directionIndex);
    static psyqo::Vec3 GetForward(psyqo::Matrix33 rotation);
    static psyqo::Vec3 GetBackward(psyqo::Matrix33 rotation);
    static psyqo::Vec3 GetLeft(psyqo::Matrix33 rotation);
    static psyqo::Vec3 GetRight(psyqo::Matrix33 rotation);
    static psyqo::Vec3 GetUp(psyqo::Matrix33 rotation);
    static psyqo::Vec3 GetDown(psyqo::Matrix33 rotation);

    static void SetPosition(GameObject* go, psyqo::FixedPoint<12> x, psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z);
    static void SetPosition(GameObject* go, psyqo::Vec3 newPos);

    static psyqo::Angle FastAtan2(int32_t sinVal, int32_t cosVal);
    static psyqo::FixedPoint<12> ToFp12(psyqo::Angle a);
    static psyqo::Vec3 MatrixToEuler(psyqo::Matrix33 m);

  private:
};

}

