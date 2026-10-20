#include "BloomSceneAlphaRoute.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace BloomRestoration {
namespace {

using Byte = std::uint8_t;

constexpr std::array<Sm3BloomSceneAlphaDescriptor,
                     kSm3BloomSceneAlphaShaderCount>
    kDescriptors{{
        {
            Sm3BloomSceneAlphaShader::SkyOther,
            0x00697D10,
            0x00CFBD6C,
            936,
            0xBB41AA467AD9FC4DULL,
        },
        {
            Sm3BloomSceneAlphaShader::SkyMode2,
            0x006980B8,
            0x00CFBD78,
            1908,
            0x8693227C5668DB7AULL,
        },
        {
            Sm3BloomSceneAlphaShader::SkyDefault,
            0x00698830,
            0x00CFBD70,
            968,
            0xF76A8AF63CC287AFULL,
        },
        {
            Sm3BloomSceneAlphaShader::BuildingLod,
            0x007072B0,
            0x00CFC0E8,
            2740,
            0x3D095B054B75C5E5ULL,
        },
        {
            Sm3BloomSceneAlphaShader::CharacterNormal,
            0x006A97A0,
            0x00CFBE10,
            5372,
            0xA6450F2B3D797D5EULL,
        },
        {
            Sm3BloomSceneAlphaShader::PaletteCharacterNormal,
            0x006CB770,
            0x00CFBFA8,
            5700,
            0xD1EDE4F3FED6A5A4ULL,
        },
    }};

static_assert(kSm3BloomSceneAlphaShaderCount <= 32);

constexpr std::array<std::size_t, 3> kSkyAlphaOffsets{
    0x1A4,
    0x320,
    0x1E8,
};
constexpr std::uint32_t kAuthoredPcSkyAlpha = 0x3EB33333U;

constexpr std::size_t kBuildingDefinitionInsert = 0x480;
constexpr std::size_t kBuildingHighlightEnd = 0x9B8;
constexpr std::size_t kBuildingAlphaStart = 0xA8C;
constexpr std::size_t kBuildingAlphaEnd = 0xAA0;

constexpr std::array<std::uint32_t, 4> kBuildingHighlightInstruction{
    0x03000005,
    0x80070000,
    0x80AA0000,
    0x80E40003,
};
constexpr std::array<std::uint32_t, 5> kBuildingPcAlphaInstruction{
    0x04000004,
    0x80080800,
    0xA0000007,
    0x80FF0000,
    0x80FF0001,
};
constexpr std::array<std::uint32_t, 6> kBuildingLumaDefinition{
    0x05000051,
    0xA00F0004,
    0x3DE147AE,
    0x3E99999A,
    0x3F170A3D,
    0x3F000000,
};
constexpr std::array<std::uint32_t, 3> kBuildingPreserveHighlight{
    0x02000001,
    0x80070006,
    0x80E40000,
};
constexpr std::array<std::uint32_t, 17> kBuildingXboxAlpha{
    0x03000008,
    0x80080006,
    0x80E40006,
    0xA0E40004,
    0x03000005,
    0x80080006,
    0x80FF0006,
    0xA0FF0004,
    0x03000002,
    0x80080002,
    0x81FF0006,
    0xA0000000,
    0x04000004,
    0x80080800,
    0xA0000007,
    0x80FF0002,
    0x80FF0006,
};

struct CharacterRewriteLayout {
    std::size_t descriptorIndex = 0;
    std::size_t definitionInsert = 0;
    std::size_t alphaStart = 0;
    std::size_t alphaEnd = 0;
    std::uint32_t lumaConstantDestination = 0;
    std::uint32_t strengthConstantDestination = 0;
    std::uint32_t finalRgbConstant = 0;
    std::uint32_t translucentFlag = 0;
};

constexpr CharacterRewriteLayout kCharacterNormalLayout{
    4,
    0x6E0,
    0x14D4,
    0x14E8,
    0xA00F0033,
    0xA00F0034,
    0xA0AA002F,
    0xA000001F,
};
constexpr CharacterRewriteLayout kPaletteCharacterNormalLayout{
    5,
    0x6C0,
    0x161C,
    0x1630,
    0xA00F0037,
    0xA00F0038,
    0xA0AA0034,
    0xA000001E,
};

constexpr std::array<std::uint32_t, 5> kCharacterPcAlphaPrefix{
    0x04000004,
    0x80080800,
    0,
    0x80FF0000,
    0x80FF0001,
};
constexpr std::array<std::uint32_t, 4> kCharacterPcRgbPrefix{
    0x03000002,
    0x80070800,
    0x80E40000,
    0,
};

template <std::size_t Size>
bool MatchesDwords(
    std::span<const Byte> bytes,
    std::size_t offset,
    const std::array<std::uint32_t, Size>& expected) noexcept {
    const std::size_t byteCount = expected.size() * sizeof(std::uint32_t);
    return offset <= bytes.size() && byteCount <= bytes.size() - offset &&
        std::memcmp(bytes.data() + offset, expected.data(), byteCount) == 0;
}

template <std::size_t Size>
void AppendDwords(
    std::vector<Byte>& destination,
    const std::array<std::uint32_t, Size>& source) {
    const auto* first = reinterpret_cast<const Byte*>(source.data());
    destination.insert(
        destination.end(),
        first,
        first + source.size() * sizeof(std::uint32_t));
}

bool RewriteSky(
    std::size_t alphaOffset,
    std::span<const Byte> original,
    std::vector<Byte>& replacement) {
    if (alphaOffset > original.size() ||
        sizeof(std::uint32_t) > original.size() - alphaOffset) {
        return false;
    }
    std::uint32_t alpha = 0;
    std::memcpy(&alpha, original.data() + alphaOffset, sizeof(alpha));
    if (alpha != kAuthoredPcSkyAlpha) {
        return false;
    }
    std::size_t occurrences = 0;
    for (std::size_t offset = 0;
         offset + sizeof(std::uint32_t) <= original.size(); ++offset) {
        std::uint32_t candidate = 0;
        std::memcpy(&candidate, original.data() + offset, sizeof(candidate));
        if (candidate == kAuthoredPcSkyAlpha) {
            ++occurrences;
        }
    }
    if (occurrences != 1) {
        return false;
    }
    replacement.assign(original.begin(), original.end());
    constexpr std::uint32_t kZero = 0;
    std::memcpy(
        replacement.data() + alphaOffset,
        &kZero,
        sizeof(kZero));
    return true;
}

bool RewriteBuilding(
    std::span<const Byte> original,
    std::vector<Byte>& replacement) {
    if (original.size() != kDescriptors[3].expectedSize ||
        kBuildingDefinitionInsert > kBuildingHighlightEnd ||
        kBuildingHighlightEnd > kBuildingAlphaStart ||
        kBuildingAlphaEnd > original.size() ||
        !MatchesDwords(
            original,
            kBuildingHighlightEnd -
                kBuildingHighlightInstruction.size() *
                    sizeof(std::uint32_t),
            kBuildingHighlightInstruction) ||
        !MatchesDwords(
            original,
            kBuildingAlphaStart,
            kBuildingPcAlphaInstruction)) {
        return false;
    }

    replacement.clear();
    replacement.reserve(
        original.size() +
        (kBuildingLumaDefinition.size() +
         kBuildingPreserveHighlight.size() +
         kBuildingXboxAlpha.size() -
         kBuildingPcAlphaInstruction.size()) * sizeof(std::uint32_t));
    replacement.insert(
        replacement.end(),
        original.begin(),
        original.begin() + kBuildingDefinitionInsert);
    AppendDwords(replacement, kBuildingLumaDefinition);
    replacement.insert(
        replacement.end(),
        original.begin() + kBuildingDefinitionInsert,
        original.begin() + kBuildingHighlightEnd);
    AppendDwords(replacement, kBuildingPreserveHighlight);
    replacement.insert(
        replacement.end(),
        original.begin() + kBuildingHighlightEnd,
        original.begin() + kBuildingAlphaStart);
    AppendDwords(replacement, kBuildingXboxAlpha);
    replacement.insert(
        replacement.end(),
        original.begin() + kBuildingAlphaEnd,
        original.end());
    return replacement.size() == 2824;
}

bool RewriteCharacterMaterial(
    const CharacterRewriteLayout& layout,
    std::span<const Byte> original,
    std::vector<Byte>& replacement,
    float entityStrength) {
    if (layout.descriptorIndex >= kDescriptors.size() ||
        !std::isfinite(entityStrength) ||
        original.size() !=
            kDescriptors[layout.descriptorIndex].expectedSize ||
        layout.definitionInsert > layout.alphaStart ||
        layout.alphaEnd > original.size()) {
        return false;
    }

    auto expectedAlpha = kCharacterPcAlphaPrefix;
    expectedAlpha[2] = layout.translucentFlag;
    auto expectedRgb = kCharacterPcRgbPrefix;
    expectedRgb[3] = layout.finalRgbConstant;
    if (!MatchesDwords(original, layout.alphaStart, expectedAlpha) ||
        !MatchesDwords(original, layout.alphaEnd, expectedRgb)) {
        return false;
    }

    const std::array<std::uint32_t, 6> lumaDefinition{
        0x05000051,
        layout.lumaConstantDestination,
        0x3DE147AE,
        0x3E99999A,
        0x3F170A3D,
        0x3F000000,
    };
    const std::array<std::uint32_t, 6> strengthDefinition{
        0x05000051,
        layout.strengthConstantDestination,
        std::bit_cast<std::uint32_t>(entityStrength),
        0,
        0,
        0,
    };
    const std::uint32_t lumaConstant =
        layout.lumaConstantDestination & 0x7FFU;
    const std::uint32_t strengthConstant =
        layout.strengthConstantDestination & 0x7FFU;
    const std::array<std::uint32_t, 25> xboxAlpha{
        0x03000002,
        0x80070002,
        0x80E40000,
        layout.finalRgbConstant,
        0x03000008,
        0x80080002,
        0x80E40002,
        0xA0E40000U | lumaConstant,
        0x03000005,
        0x80080002,
        0x80FF0002,
        0xA0FF0000U | lumaConstant,
        0x03000002,
        0x80080003,
        0x81FF0002,
        layout.finalRgbConstant,
        0x04000004,
        0x80080003,
        layout.translucentFlag,
        0x80FF0003,
        0x80FF0002,
        0x03000005,
        0x80080800,
        0x80FF0003,
        0xA0000000U | strengthConstant,
    };

    replacement.clear();
    replacement.reserve(original.size() + 128);
    replacement.insert(
        replacement.end(), original.begin(),
        original.begin() + layout.definitionInsert);
    AppendDwords(replacement, lumaDefinition);
    AppendDwords(replacement, strengthDefinition);
    replacement.insert(
        replacement.end(),
        original.begin() + layout.definitionInsert,
        original.begin() + layout.alphaStart);
    AppendDwords(replacement, xboxAlpha);
    replacement.insert(
        replacement.end(),
        original.begin() + layout.alphaEnd,
        original.end());
    return replacement.size() == original.size() + 128;
}

} // namespace

std::span<const Sm3BloomSceneAlphaDescriptor>
Sm3BloomSceneAlphaDescriptors() noexcept {
    return kDescriptors;
}

std::uint64_t Sm3BloomShaderFnv1a64(
    std::span<const Byte> bytes) noexcept {
    std::uint64_t result = 14695981039346656037ULL;
    for (const Byte byte : bytes) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    return result;
}

bool RewriteSm3BloomSceneAlphaShader(
    Sm3BloomSceneAlphaShader shader,
    std::span<const Byte> original,
    std::vector<Byte>& replacement,
    const Sm3BloomSceneAlphaRewriteParameters& parameters) noexcept {
    try {
        replacement.clear();
        const auto descriptors = Sm3BloomSceneAlphaDescriptors();
        const auto found = std::find_if(
            descriptors.begin(),
            descriptors.end(),
            [shader](const Sm3BloomSceneAlphaDescriptor& candidate) {
                return candidate.shader == shader;
            });
        if (found == descriptors.end() ||
            original.size() != found->expectedSize ||
            Sm3BloomShaderFnv1a64(original) != found->expectedFnv1a64) {
            return false;
        }

        switch (shader) {
        case Sm3BloomSceneAlphaShader::SkyOther:
            return RewriteSky(kSkyAlphaOffsets[0], original, replacement);
        case Sm3BloomSceneAlphaShader::SkyMode2:
            return RewriteSky(kSkyAlphaOffsets[1], original, replacement);
        case Sm3BloomSceneAlphaShader::SkyDefault:
            return RewriteSky(kSkyAlphaOffsets[2], original, replacement);
        case Sm3BloomSceneAlphaShader::BuildingLod:
            return RewriteBuilding(original, replacement);
        case Sm3BloomSceneAlphaShader::CharacterNormal:
            return RewriteCharacterMaterial(
                kCharacterNormalLayout,
                original,
                replacement,
                parameters.entityStrength);
        case Sm3BloomSceneAlphaShader::PaletteCharacterNormal:
            return RewriteCharacterMaterial(
                kPaletteCharacterNormalLayout,
                original,
                replacement,
                parameters.entityStrength);
        }
    } catch (...) {
        replacement.clear();
        return false;
    }
    return false;
}

} // namespace BloomRestoration
