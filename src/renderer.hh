#pragma once

#include "EASTL/array.h"
#include "gameobject.hh"
#include "psyqo/fragments.hh"
#include "psyqo/gpu.hh"
#include "psyqo/ordering-table.hh"
#include "psyqo/primitives/common.hh"
#include "psyqo/primitives/misc.hh"
#include "psyqo/bump-allocator.hh"
#include "psyqo/trigonometry.hh"

namespace psxsplash {

class Renderer final {
    static constexpr size_t ORDERING_TABLE_SIZE = 1024;
    static constexpr size_t BUMP_ALLOCATOR_SIZE = 100000;
    
    psyqo::GPU& m_gpu;
    psyqo::Trig<> m_trig;
    
    public:
    psyqo::OrderingTable<ORDERING_TABLE_SIZE> m_ots[2];
    psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill> m_clear[2];
    psyqo::Color m_clearcolor = {.r = 63, .g = 63, .b = 63};
    psyqo::BumpAllocator<BUMP_ALLOCATOR_SIZE> m_ballocs[2];

    Renderer(psyqo::GPU& gpuInstance) : m_gpu(gpuInstance) {}
    void initialize();
    void render(eastl::array<GameObject>& objects);
};

}