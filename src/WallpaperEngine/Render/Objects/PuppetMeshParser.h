#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace WallpaperEngine::Render::Objects {

// Bounded MDLV0004/0013/0014/0016/0017/0019/0021/0023 mesh subset. Later MDLS
// skeleton and animation records are parsed separately.
struct PuppetMeshData {
    struct BoneRange {
	uint32_t boneIndex = 0;
	uint32_t rawFlags = 0;
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
    };
    int version = 0;
    uint32_t vertexMask = 0;
    uint32_t meshFlags = 0; // Native per-mesh flags; bit 1 supplies an extra field.
    uint32_t blendRowCount = 0; // Present when meshFlags bit 1 is set.
    std::optional<std::array<float, 6>> authoredBounds; // MDLV17+: minXYZ, maxXYZ.
    std::string material;
    std::vector<std::string> materials; // Native reads this list for each mesh.
    std::vector<std::array<float, 3>> positions;
    std::vector<float> morphIndices; // POSITION4.w addresses the MDMP target texture.
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 4>> tangents;
    std::vector<std::array<uint32_t, 4>> blendIndices;
    std::vector<std::array<float, 4>> blendWeights;
    std::vector<std::array<float, 2>> texcoords;
    std::vector<std::array<float, 4>> texcoordsFull; // Preserves float4 TEXCOORD layouts.
    std::vector<uint16_t> indices;
    // MDLV0021+ optional vertex-position stream. Native reads a stream count
    // followed by a sized float3 payload; the observed assets contain one
    // position per mesh vertex. Keep it distinct from render positions.
    uint32_t optionalPositionStreamCount = 0;
    std::vector<std::array<float, 3>> optionalPositions;
    // The second sized stream holds per-bone triangle-index ranges in the
    // observed v21 models. Its raw flag is retained for later consumers.
    std::vector<BoneRange> boneRanges;
    size_t payloadEndOffset = 0; // Before version-specific tail records and MDLS.
};

struct PuppetMeshesData {
    int version = 0;
    std::vector<PuppetMeshData> meshes;
    size_t sectionEndOffset = 0; // Immediately before MDLS after all mesh tails.
};

// MDMP deltas are signed normalized int16 triples, scaled before skinning.
struct PuppetMorphData {
    struct BoneRule {
	uint32_t bone;
	bool axis;
	float edge0;
	float edge1;
    };
    float scale = 0;
    uint32_t vertexCount = 0;
    std::vector<std::string> names;
    std::vector<std::array<float, 4>> values;
    std::vector<BoneRule> boneRules;
    bool hasAlpha = false;

    [[nodiscard]] std::array<float, 4> sample (uint32_t vertex, uint32_t target) const;
};

PuppetMorphData parsePuppetMorph (std::span<const uint8_t> bytes, size_t sectionOffset, uint32_t meshFlags);

void sortPuppetParts (
    std::span<const PuppetMeshData::BoneRange> parts, std::span<const int> boneOrder,
    std::span<const float> animatedOrder, std::vector<uint32_t>& order
);

inline std::array<float, 2> puppetTexcoordScale (
    uint32_t realWidth, uint32_t realHeight, uint32_t storedWidth, uint32_t storedHeight, bool animated
) {
    if (animated || storedWidth == 0 || storedHeight == 0) {
	return { 1.0f, 1.0f };
    }
    return { static_cast<float> (realWidth) / storedWidth, static_cast<float> (realHeight) / storedHeight };
}

PuppetMeshesData parsePuppetMeshes (std::span<const uint8_t> bytes);
} // namespace WallpaperEngine::Render::Objects
