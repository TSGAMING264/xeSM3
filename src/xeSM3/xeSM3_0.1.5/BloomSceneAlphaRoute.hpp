#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace BloomRestoration {

enum class Sm3BloomSceneAlphaShader : std::uint32_t {
    SkyOther,
    SkyMode2,
    SkyDefault,
    BuildingLod,
    CharacterNormal,
    PaletteCharacterNormal,
};

struct Sm3BloomSceneAlphaDescriptor {
    Sm3BloomSceneAlphaShader shader{};
    std::uint32_t bytecodeRva = 0;
    std::uint32_t handleSlotRva = 0;
    std::size_t expectedSize = 0;
    std::uint64_t expectedFnv1a64 = 0;
};

struct Sm3BloomSceneAlphaRewriteParameters {
    // 1.0 preserves the recovered Xbox character-material alpha selector.
    float entityStrength = 1.0F;
};

// The original four producers are required for the base restoration. Character
// material producers are prepared independently because those shader families
// may be initialized after the post-process graph on some level transitions.
inline constexpr std::size_t kSm3BloomRequiredSceneAlphaShaderCount = 4;
inline constexpr std::size_t kSm3BloomSceneAlphaShaderCount = 6;

[[nodiscard]] std::span<const Sm3BloomSceneAlphaDescriptor>
Sm3BloomSceneAlphaDescriptors() noexcept;

[[nodiscard]] std::uint64_t Sm3BloomShaderFnv1a64(
    std::span<const std::uint8_t> bytes) noexcept;

// Rewrites only the authenticated alpha-publication instructions. RGB and
// the original constant-table comment are retained byte-for-byte.
[[nodiscard]] bool RewriteSm3BloomSceneAlphaShader(
    Sm3BloomSceneAlphaShader shader,
    std::span<const std::uint8_t> original,
    std::vector<std::uint8_t>& replacement,
    const Sm3BloomSceneAlphaRewriteParameters& parameters = {}) noexcept;

} // namespace BloomRestoration
