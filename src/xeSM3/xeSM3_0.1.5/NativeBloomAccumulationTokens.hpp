#pragma once
#include <cstdint>
namespace NativeBloomAccumulationTokens
{
// D3D9 translation of recovered Xbox Gaussian5x5_Pixel (82072280).
// Two CLAMP/LINEAR s0 fetches at v0.xy+c0.xy and v0.xy+c0.zw;
// shader result = .3125*left + .3125*right for every RGBA channel.
// Retained destination is supplied separately by fixed-function blending.
// These are generated PC tokens, not Xbox shader bytes or compiler output.
constexpr uint32_t kWords[] = {
    0xFFFF0300u,
    0x05000051u,0xA00F0001u,0x3EA00000u,0u,0u,0u,
    0x0200001Fu,0x80000005u,0x90030000u,
    0x0200001Fu,0x90000000u,0xA00F0800u,
    0x03000002u,0x80030000u,0x90E40000u,0xA0E40000u,
    0x03000042u,0x800F0000u,0x80E40000u,0xA0E40800u,
    0x03000005u,0x800F0001u,0x80E40000u,0xA0000001u,
    0x03000002u,0x80030000u,0x90E40000u,0xA0EE0000u,
    0x03000042u,0x800F0000u,0x80E40000u,0xA0E40800u,
    0x04000004u,0x800F0800u,0x80E40000u,0xA0000001u,0x80E40001u,
    0x0000FFFFu
};
}
