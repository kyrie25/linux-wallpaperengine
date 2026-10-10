#include "PuppetMeshParser.h"

#include <bit>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>

using namespace WallpaperEngine::Render::Objects;

namespace {
class Cursor {
public:
    explicit Cursor (std::span<const uint8_t> bytes, size_t offset = 0) : m_bytes (bytes), m_offset (offset) {
	if (offset > bytes.size ()) {
	    throw std::runtime_error ("Invalid MDLV cursor offset");
	}
    }

    size_t offset () const { return m_offset; }
    size_t remaining () const { return m_bytes.size () - m_offset; }

    std::span<const uint8_t> take (size_t count) {
	if (count > remaining ()) {
	    throw std::runtime_error ("Truncated MDLV mesh data");
	}
	const auto result = m_bytes.subspan (m_offset, count);
	m_offset += count;
	return result;
    }

    uint32_t u32 () {
	const auto bytes = take (4);
	return uint32_t (bytes[0]) | (uint32_t (bytes[1]) << 8) | (uint32_t (bytes[2]) << 16)
	    | (uint32_t (bytes[3]) << 24);
    }

    uint64_t u64 () {
	const uint64_t low = u32 ();
	return low | (uint64_t (u32 ()) << 32);
    }

    uint8_t u8 () { return take (1)[0]; }

    uint16_t u16 () {
	const auto bytes = take (2);
	return uint16_t (bytes[0]) | (uint16_t (bytes[1]) << 8);
    }

    std::string cstring () {
	const size_t start = m_offset;
	while (m_offset < m_bytes.size () && m_bytes[m_offset] != 0) {
	    ++m_offset;
	}
	if (m_offset == m_bytes.size ()) {
	    throw std::runtime_error ("Unterminated MDLV string");
	}
	const auto length = m_offset++ - start;
	return std::string (reinterpret_cast<const char*> (m_bytes.data () + start), length);
    }

private:
    std::span<const uint8_t> m_bytes;
    size_t m_offset = 0;
};

uint32_t readU32 (std::span<const uint8_t> bytes, size_t offset) {
    return uint32_t (bytes[offset]) | (uint32_t (bytes[offset + 1]) << 8) | (uint32_t (bytes[offset + 2]) << 16)
	| (uint32_t (bytes[offset + 3]) << 24);
}

float readFloat (std::span<const uint8_t> bytes, size_t offset) {
    return std::bit_cast<float> (readU32 (bytes, offset));
}
PuppetMeshData parseMesh (Cursor& cursor, int version, uint32_t headerFlags, uint32_t materialCount) {
    PuppetMeshData result;
    result.version = version;
    for (uint32_t i = 0; i < materialCount; ++i) {
	const std::string material = cursor.cstring ();
	if (i == 0) {
	    result.material = material;
	}
	result.materials.push_back (material);
    }
    // Native 140261880 reads this per-mesh field at v4+, an extra uint32 when
    // bit 1 is set, then six floats at v17+ before the attribute mask.
    const uint32_t meshFlags = cursor.u32 ();
    result.meshFlags = meshFlags;
    if (meshFlags & 2) {
	result.blendRowCount = cursor.u32 ();
    }
    if (version >= 17) {
	const auto bounds = cursor.take (6 * sizeof (float));
	result.authoredBounds.emplace ();
	for (size_t lane = 0; lane < 6; ++lane) {
	    (*result.authoredBounds)[lane] = readFloat (bounds, lane * 4);
	}
    }
    result.vertexMask = version >= 15 ? cursor.u32 () : headerFlags;
    // Native 1400d7f90's attribute table orders these six semantics:
    // position12, optional normal12/tangent16, optional indices16/weights16,
    // then TEXCOORD float2 (mask8) or float4 (mask0x20).
    constexpr uint32_t knownMask = 0x0181002f;
    const uint32_t texcoordMask = result.vertexMask & 0x28;
    if (((result.vertexMask & 0x10001) != 1 && (result.vertexMask & 0x10001) != 0x10000)
	|| (texcoordMask != 8 && texcoordMask != 0x20) || (result.vertexMask & ~knownMask) != 0) {
	throw std::runtime_error ("Unsupported MDLV vertex attribute mask " + std::to_string (result.vertexMask));
    }
    const bool hasBlendIndices = (result.vertexMask & 0x00800000) != 0;
    const bool hasBlendWeights = (result.vertexMask & 0x01000000) != 0;
    const size_t positionBytes = (result.vertexMask & 0x10000) ? 16 : 12;
    const size_t blendIndexOffset
	= positionBytes + ((result.vertexMask & 2) ? 12 : 0) + ((result.vertexMask & 4) ? 16 : 0);
    const size_t blendWeightOffset = blendIndexOffset + (hasBlendIndices ? 16 : 0);
    const size_t uvOffset = blendWeightOffset + (hasBlendWeights ? 16 : 0);
    const size_t texcoordComponents = texcoordMask == 8 ? 2 : 4;
    const size_t stride = uvOffset + texcoordComponents * sizeof (float);

    const uint32_t vertexBytes = cursor.u32 ();
    if (vertexBytes == 0 || vertexBytes % stride != 0) {
	throw std::runtime_error ("Invalid MDLV vertex byte count");
    }
    const auto vertices = cursor.take (vertexBytes);
    const size_t vertexCount = vertexBytes / stride;
    if (vertexCount > UINT16_MAX + size_t (1)) {
	throw std::runtime_error ("MDLV mesh exceeds 16-bit index range");
    }
    result.positions.reserve (vertexCount);
    result.normals.reserve (vertexCount);
    result.tangents.reserve (vertexCount);
    result.blendIndices.reserve (vertexCount);
    result.blendWeights.reserve (vertexCount);
    result.texcoords.reserve (vertexCount);
    result.texcoordsFull.reserve (vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
	const size_t offset = i * stride;
	const std::array<float, 3> position { readFloat (vertices, offset), readFloat (vertices, offset + 4),
					      readFloat (vertices, offset + 8) };
	std::array<float, 3> normal {};
	if (result.vertexMask & 2) {
	    for (size_t lane = 0; lane < 3; ++lane) {
		normal[lane] = readFloat (vertices, offset + positionBytes + lane * 4);
	    }
	}
	std::array<float, 4> tangent {};
	if (result.vertexMask & 4) {
	    for (size_t lane = 0; lane < 4; ++lane) {
		tangent[lane]
		    = readFloat (vertices, offset + positionBytes + ((result.vertexMask & 2) ? 12 : 0) + lane * 4);
	    }
	}
	std::array<float, 4> uvFull {};
	for (size_t lane = 0; lane < texcoordComponents; ++lane) {
	    uvFull[lane] = readFloat (vertices, offset + uvOffset + lane * 4);
	}
	const std::array<float, 2> uv { uvFull[0], uvFull[1] };
	std::array<uint32_t, 4> blendIndices {};
	std::array<float, 4> blendWeights {};
	for (size_t lane = 0; lane < 4; ++lane) {
	    if (hasBlendIndices) {
		blendIndices[lane] = readU32 (vertices, offset + blendIndexOffset + lane * 4);
	    }
	    if (hasBlendWeights) {
		blendWeights[lane] = readFloat (vertices, offset + blendWeightOffset + lane * 4);
		if (!std::isfinite (blendWeights[lane])) {
		    throw std::runtime_error ("Nonfinite MDLV blend weight");
		}
	    }
	}
	if (!std::isfinite (position[0]) || !std::isfinite (position[1]) || !std::isfinite (position[2])
	    || !std::isfinite (normal[0]) || !std::isfinite (normal[1]) || !std::isfinite (normal[2])
	    || !std::isfinite (tangent[0]) || !std::isfinite (tangent[1]) || !std::isfinite (tangent[2])
	    || !std::isfinite (tangent[3]) || !std::isfinite (uvFull[0]) || !std::isfinite (uvFull[1])
	    || !std::isfinite (uvFull[2]) || !std::isfinite (uvFull[3])) {
	    throw std::runtime_error ("Nonfinite MDLV position or UV");
	}
	result.positions.push_back (position);
	const float morphIndex = (result.vertexMask & 0x10000) ? readFloat (vertices, offset + 12) : 0;
	if (!std::isfinite (morphIndex) || static_cast<double> (morphIndex) > UINT32_MAX) {
	    throw std::runtime_error ("Invalid MDLV morph texel index");
	}
	result.morphIndices.push_back (morphIndex);
	result.normals.push_back (normal);
	result.tangents.push_back (tangent);
	result.blendIndices.push_back (blendIndices);
	result.blendWeights.push_back (blendWeights);
	result.texcoords.push_back (uv);
	result.texcoordsFull.push_back (uvFull);
    }

    const uint32_t indexBytes = cursor.u32 ();
    if (indexBytes == 0 || indexBytes % (sizeof (uint16_t) * 3) != 0) {
	throw std::runtime_error ("Invalid MDLV triangle index byte count");
    }
    const auto indices = cursor.take (indexBytes);
    result.indices.reserve (indexBytes / 2);
    for (size_t i = 0; i < indices.size (); i += 2) {
	const uint16_t index = uint16_t (indices[i]) | (uint16_t (indices[i + 1]) << 8);
	if (index >= vertexCount) {
	    throw std::runtime_error ("MDLV triangle index out of range");
	}
	result.indices.push_back (index);
    }
    result.payloadEndOffset = cursor.offset ();
    return result;
}

void parseMeshTail (Cursor& cursor, PuppetMeshData& mesh) {
    if (mesh.version >= 21) {
	if (cursor.u8 () != 0) {
	    mesh.optionalPositionStreamCount = cursor.u32 ();
	    const uint32_t length = cursor.u32 ();
	    const size_t vertexCount = mesh.positions.size ();
	    if (mesh.optionalPositionStreamCount != 1 || vertexCount > UINT32_MAX / (3 * sizeof (float))
		|| length != vertexCount * 3 * sizeof (float)) {
		throw std::runtime_error ("Unsupported MDLV optional position stream layout");
	    }
	    const auto payload = cursor.take (length);
	    mesh.optionalPositions.reserve (vertexCount);
	    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
		std::array<float, 3> position {};
		for (size_t lane = 0; lane < 3; ++lane) {
		    position[lane] = readFloat (payload, (vertex * 3 + lane) * sizeof (float));
		    if (!std::isfinite (position[lane])) {
			throw std::runtime_error ("Nonfinite MDLV optional position");
		    }
		}
		mesh.optionalPositions.push_back (position);
	    }
	}
	if (cursor.u8 () != 0) {
	    const uint32_t length = cursor.u32 ();
	    if (length % 16 != 0) {
		throw std::runtime_error ("Unsupported MDLV bone-range record layout");
	    }
	    const auto payload = cursor.take (length);
	    mesh.boneRanges.reserve (length / 16);
	    for (size_t offset = 0; offset < length; offset += 16) {
		const PuppetMeshData::BoneRange range { readU32 (payload, offset), readU32 (payload, offset + 4),
							readU32 (payload, offset + 8), readU32 (payload, offset + 12) };
		if (range.firstIndex > mesh.indices.size ()
		    || range.indexCount > mesh.indices.size () - range.firstIndex || range.firstIndex % 3 != 0
		    || range.indexCount % 3 != 0) {
		    throw std::runtime_error ("Invalid MDLV bone triangle range");
		}
		mesh.boneRanges.push_back (range);
	    }
	}
    }
    if (mesh.version >= 23) {
	const uint32_t records = cursor.u32 ();
	for (uint32_t record = 0; record < records; record++) {
	    cursor.u64 ();
	    cursor.cstring ();
	    cursor.u32 ();
	    for (int list = 0; list < 2; list++) {
		const uint32_t count = cursor.u32 ();
		if (count > cursor.remaining () / 4) {
		    throw std::runtime_error ("Truncated MDLV clipping record");
		}
		cursor.take (static_cast<size_t> (count) * 4);
	    }
	}
    }
}

} // namespace

PuppetMeshesData WallpaperEngine::Render::Objects::parsePuppetMeshes (std::span<const uint8_t> bytes) {
    Cursor cursor (bytes);
    const std::string magic = cursor.cstring ();
    if (magic.size () != 8 || !std::string_view (magic).starts_with ("MDLV")) {
	throw std::runtime_error ("Invalid MDLV header");
    }
    int version = 0;
    for (size_t i = 4; i < 8; ++i) {
	if (magic[i] < '0' || magic[i] > '9') {
	    throw std::runtime_error ("Invalid MDLV version");
	}
	version = version * 10 + (magic[i] - '0');
    }
    if (version != 4 && version != 13 && version != 14 && version != 16 && version != 17 && version != 19
	&& version != 21 && version != 23) {
	throw std::runtime_error ("Unsupported MDLV version " + std::to_string (version));
    }

    const uint32_t headerFlags = cursor.u32 ();
    const uint32_t materialCount = cursor.u32 ();
    const uint32_t meshCount = cursor.u32 ();
    if (meshCount == 0 || meshCount > cursor.remaining () / 13) {
	throw std::runtime_error ("Invalid MDLV mesh count");
    }
    PuppetMeshesData result;
    result.version = version;
    result.meshes.reserve (meshCount);
    for (uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
	if (materialCount > cursor.remaining ()) {
	    throw std::runtime_error ("Truncated MDLV material list");
	}
	result.meshes.push_back (parseMesh (cursor, version, headerFlags, materialCount));
	parseMeshTail (cursor, result.meshes.back ());
    }
    result.sectionEndOffset = cursor.offset ();
    return result;
}

PuppetMorphData WallpaperEngine::Render::Objects::parsePuppetMorph (
    std::span<const uint8_t> bytes, size_t sectionOffset, uint32_t meshFlags
) {
    Cursor header (bytes, sectionOffset);
    const auto tag = header.cstring ();
    if (tag != "MDMP0001") {
	throw std::runtime_error ("Unsupported MDMP header");
    }
    const size_t end = header.u32 ();
    if (end > bytes.size () || (end != 0 && end < header.offset ())) {
	throw std::runtime_error ("Invalid MDMP section end");
    }
    Cursor cursor (bytes.first (end == 0 ? bytes.size () : end), header.offset ());
    PuppetMorphData result;
    const uint16_t targets = cursor.u16 ();
    if (targets == 0) {
	return result;
    }
    result.scale = std::bit_cast<float> (cursor.u32 ());
    result.vertexCount = cursor.u32 ();
    if (!std::isfinite (result.scale) || result.vertexCount == 0 || result.vertexCount > cursor.remaining () / 6) {
	throw std::runtime_error ("Invalid MDMP scale or vertex count");
    }
    const auto snorm = [] (std::span<const uint8_t> data, size_t offset) {
	const auto raw = uint16_t (uint16_t (data[offset]) | (uint16_t (data[offset + 1]) << 8));
	return std::max (float (std::bit_cast<int16_t> (raw)) / 32767.0f, -1.0f);
    };
    const auto blob = [&] (size_t expected) {
	if (cursor.u32 () != expected) {
	    throw std::runtime_error ("MDMP payload does not match its vertex count");
	}
	return cursor.take (expected);
    };
    for (uint16_t target = 0; target < targets; target++) {
	cursor.u64 ();
	result.names.push_back (cursor.cstring ());
	const auto positions = blob (size_t (result.vertexCount) * 6);
	for (const uint32_t flag : { 0x400u, 0x800u }) {
	    if (meshFlags & flag) {
		blob (positions.size ());
	    }
	}
	const auto alphas = meshFlags & 0x1000 ? blob (size_t (result.vertexCount) * 2) : std::span<const uint8_t> {};
	for (size_t vertex = 0; vertex < result.vertexCount; vertex++) {
	    const float alpha = alphas.empty () ? 1.0f : snorm (alphas, vertex * 2);
	    result.values.push_back ({ snorm (positions, vertex * 6), snorm (positions, vertex * 6 + 2),
				      snorm (positions, vertex * 6 + 4), alpha });
	    result.hasAlpha = result.hasAlpha || alpha != 1.0f;
	}
	if (meshFlags & 0x2000) {
	    PuppetMorphData::BoneRule rule { cursor.u32 (), (cursor.u32 () & 2) != 0,
					   std::bit_cast<float> (cursor.u32 ()), std::bit_cast<float> (cursor.u32 ()) };
	    if (!std::isfinite (rule.edge0) || !std::isfinite (rule.edge1)) {
		throw std::runtime_error ("Nonfinite MDMP bone rule");
	    }
	    result.boneRules.push_back (rule);
	}
    }
    return result;
}

std::array<float, 4> PuppetMorphData::sample (uint32_t vertex, uint32_t target) const {
    // POSITION4.w uses one-based texel indices; zero leaves the vertex alone.
    if (vertex == 0 || vertex > vertexCount || target >= names.size ()) {
	return { 0, 0, 0, 1 };
    }
    return values[size_t (target) * vertexCount + vertex - 1];
}

void WallpaperEngine::Render::Objects::sortPuppetParts (
    std::span<const PuppetMeshData::BoneRange> parts, std::span<const int> boneOrder,
    std::span<const float> animatedOrder, std::vector<uint32_t>& order
) {
    if (order.size () != parts.size ()) {
	order.resize (parts.size ());
	for (uint32_t index = 0; index < order.size (); index++) order[index] = index;
    }
    std::vector<double> keys;
    keys.reserve (parts.size ());
    for (const auto& part : parts) {
	const double base = double (std::bit_cast<int32_t> (part.rawFlags))
	    + (part.boneIndex < boneOrder.size () ? boneOrder[part.boneIndex] : 0);
	const float animated = part.boneIndex < animatedOrder.size () ? animatedOrder[part.boneIndex] : 0;
	keys.push_back (std::trunc (base + (std::isfinite (animated) ? animated : 0)));
    }
    // Preserve the previous order for equal integer keys, including after a
    // track is hidden and the rig restores its rest orders.
    std::ranges::stable_sort (order, [&] (uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
}
