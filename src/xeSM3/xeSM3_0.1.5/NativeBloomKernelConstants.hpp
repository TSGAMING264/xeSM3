#pragma once
#include <cstdint>

namespace NativeBloomKernel
{
    struct Constants { float vs[4] = {}; float ps[7][4] = {}; };
    inline bool Build(uint32_t width, uint32_t height, Constants& out)
    {
        out = Constants{};
        if (!width || !height || width > 4096u || height > 4096u) return false;
        const float x = 1.0f / static_cast<float>(width), y = 1.0f / static_cast<float>(height);
        out.vs[0] = -5.0f * x; out.vs[2] = 5.0f * x;
        const float xs[6] = { -x, x, 4.0f*x, -4.0f*x, 6.0f*x, -6.0f*x };
        for (uint32_t n=0u; n<6u; ++n)
        { out.ps[n][0]=xs[n]; out.ps[n][1]=-y; out.ps[n][2]=xs[n]; out.ps[n][3]=y; }
        out.ps[6][0]=0.375f; out.ps[6][1]=0.25f; out.ps[6][2]=0.0625f;
        return true;
    }
}
