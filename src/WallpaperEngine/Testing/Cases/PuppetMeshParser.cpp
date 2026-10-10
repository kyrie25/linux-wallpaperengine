#include "WallpaperEngine/Render/Objects/PuppetMeshParser.h"
#include "WallpaperEngine/Render/Objects/PuppetClipping.h"
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cstdint>
#include <limits>
#include <string>
using namespace WallpaperEngine::Render::Objects;
namespace {
void u32 (std::string& out, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
	out.push_back (char (value >> shift));
    }
}
void u64 (std::string& out, uint64_t value) {
    u32 (out, uint32_t (value));
    u32 (out, uint32_t (value >> 32));
}
void f32 (std::string& out, float value) { u32 (out, std::bit_cast<uint32_t> (value)); }
void cstr (std::string& out, const std::string& value) {
    out += value;
    out.push_back ('\0');
}
void patch32 (std::string& out, size_t at, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
	out[at + shift / 8] = char (value >> shift);
    }
}
struct Fixture {
    std::string bytes;
    size_t vertexLength = 0;
    size_t vertexPayload = 0;
    size_t indexLength = 0;
    size_t indexPayload = 0;
};
Fixture makeFixture (
    int version = 23, uint32_t headerFlags = 0x01800009, uint32_t vertexMask = 0x0180000f, uint32_t meshCount = 1,
    uint32_t meshFlags = 2, uint32_t materialCount = 1
) {
    Fixture f;
    cstr (f.bytes, (version < 10 ? "MDLV000" : "MDLV00") + std::to_string (version));
    u32 (f.bytes, headerFlags);
    u32 (f.bytes, materialCount);
    u32 (f.bytes, meshCount);
    if (materialCount) {
	cstr (f.bytes, "material/MDLS_embedded");
    }
    u32 (f.bytes, meshFlags);
    if (meshFlags & 2) {
	u32 (f.bytes, 1); // Native channel-map row count.
    }
    if (version >= 17) {
	for (int i = 0; i < 6; ++i) {
	    f32 (f.bytes, float (i));
	}
    }
    if (version >= 15) {
	u32 (f.bytes, vertexMask);
    }
    const size_t blendIndexOffset
	= ((vertexMask & 0x10000) ? 16 : 12) + ((vertexMask & 2) ? 12 : 0) + ((vertexMask & 4) ? 16 : 0);
    const bool hasBlendIndices = (vertexMask & 0x00800000) != 0;
    const bool hasBlendWeights = (vertexMask & 0x01000000) != 0;
    const size_t blendWeightOffset = blendIndexOffset + (hasBlendIndices ? 16 : 0);
    const size_t uvOffset = blendWeightOffset + (hasBlendWeights ? 16 : 0);
    const bool uv4 = (vertexMask & 0x20) != 0;
    const size_t stride = uvOffset + (uv4 ? 16 : 8);
    f.vertexLength = f.bytes.size ();
    u32 (f.bytes, uint32_t (3 * stride));
    f.vertexPayload = f.bytes.size ();
    for (int i = 0; i < 3; ++i) {
	std::string vertex (stride, '\0');
	patch32 (vertex, 0, std::bit_cast<uint32_t> (float (i + 1)));
	patch32 (vertex, 4, std::bit_cast<uint32_t> (float (i + 2)));
	patch32 (vertex, 8, std::bit_cast<uint32_t> (float (i + 3)));
	vertex.replace (20, 4, "MDLS");
	if (hasBlendIndices) {
	    patch32 (vertex, blendIndexOffset, 0);
	    patch32 (vertex, blendIndexOffset + 4, 1);
	}
	if (hasBlendWeights) {
	    patch32 (vertex, blendWeightOffset, std::bit_cast<uint32_t> (0.25f));
	    patch32 (vertex, blendWeightOffset + 4, std::bit_cast<uint32_t> (0.75f));
	}
	patch32 (vertex, uvOffset, std::bit_cast<uint32_t> (float (i) / 4));
	patch32 (vertex, uvOffset + 4, std::bit_cast<uint32_t> (float (i) / 2));
	if (uv4) {
	    patch32 (vertex, uvOffset + 8, std::bit_cast<uint32_t> (float (i) / 8));
	    patch32 (vertex, uvOffset + 12, std::bit_cast<uint32_t> (float (i) / 16));
	}
	f.bytes += vertex;
    }
    f.indexLength = f.bytes.size ();
    u32 (f.bytes, 6);
    f.indexPayload = f.bytes.size ();
    f.bytes += std::string { char (0), char (0), char (1), char (0), char (2), char (0) };
    if (version >= 21) {
	f.bytes.append (2, '\0');
    }
    if (version >= 23) {
	u32 (f.bytes, 0);
    }
    cstr (f.bytes, "MDLS0004"); // Opaque tail sentinel, not a complete native tail.
    return f;
}
auto parse (const std::string& bytes) {
    return parsePuppetMeshes ({ reinterpret_cast<const uint8_t*> (bytes.data ()), bytes.size () }).meshes.at (0);
}
auto parseMeshes (const std::string& bytes) {
    return parsePuppetMeshes ({ reinterpret_cast<const uint8_t*> (bytes.data ()), bytes.size () });
}
}

TEST_CASE ("Versioned MDLV first mesh follows fields instead of embedded markers", "[puppet][mesh]") {
    for (int version : { 13, 14, 16, 17, 19, 21, 23 }) {
	const std::vector<uint32_t> headers
	    = version < 15 ? std::vector<uint32_t> { 0x01800009u } : std::vector<uint32_t> { 0u, 0x01800009u };
	for (uint32_t headerFlags : headers) {
	    const auto fixture = makeFixture (version, headerFlags, version <= 16 ? 0x01800009 : 0x0180000f);
	    const auto mesh = parse (fixture.bytes);
	    REQUIRE (mesh.version == version);
	    REQUIRE (mesh.vertexMask == (version <= 16 ? 0x01800009u : 0x0180000fu));
	    REQUIRE (mesh.material == "material/MDLS_embedded");
	    REQUIRE (mesh.positions.size () == 3);
	    REQUIRE (mesh.positions[2][0] == 3.0f);
	    REQUIRE (mesh.texcoords[2][0] == 0.5f);
	    REQUIRE (mesh.blendIndices[2][1] == 1);
	    REQUIRE (mesh.blendWeights[2][0] == 0.25f);
	    REQUIRE (mesh.blendWeights[2][1] == 0.75f);
	    REQUIRE (mesh.indices == std::vector<uint16_t> { 0, 1, 2 });
	    REQUIRE (mesh.payloadEndOffset == fixture.indexPayload + 6);
	    REQUIRE (fixture.bytes.compare (parseMeshes (fixture.bytes).sectionEndOffset, 8, "MDLS0004") == 0);
	}
    }
    const auto unskinned = parse (makeFixture (13, 0x0000000f, 0x0000000f).bytes);
    REQUIRE (unskinned.vertexMask == 0x0000000fu);
    REQUIRE (unskinned.positions.size () == 3);
    REQUIRE (unskinned.texcoords[2][1] == 1.0f);
    REQUIRE (unskinned.blendWeights[0][0] == 0.0f);
    const auto channelMap = parse (makeFixture (17, 0, 0x00800021).bytes);
    REQUIRE (channelMap.blendIndices[2][1] == 1);
    REQUIRE (channelMap.blendWeights[2][0] == 0.0f);
    REQUIRE (channelMap.texcoordsFull[2][2] == 0.25f);
    REQUIRE (channelMap.texcoordsFull[2][3] == 0.125f);
    REQUIRE (parse (makeFixture (17, 0, 0x0180000f, 1, 0, 0).bytes).material.empty ());
}

TEST_CASE (
    "MDLV v17 authored bounds retain header values independently of triangle positions",
    "[puppet][mesh][collision-model]"
) {
    for (const int version : { 16, 17, 23 }) {
	const auto mesh = parse (makeFixture (version).bytes);
	if (version < 17) {
	    REQUIRE_FALSE (mesh.authoredBounds);
	} else {
	    REQUIRE (mesh.authoredBounds);
	    REQUIRE (*mesh.authoredBounds == std::array<float, 6> { 0, 1, 2, 3, 4, 5 });
	    REQUIRE (mesh.positions.front () != std::array<float, 3> { 0, 1, 2 });
	}
    }
}

TEST_CASE ("Static MDLV v14 retains normal and tangent attribute lanes", "[puppet][mesh]") {
    auto fixture = makeFixture (14, 0x0000000f, 0x0000000f, 1, 0);
    for (size_t lane = 0; lane < 3; ++lane) {
	patch32 (
	    fixture.bytes, fixture.vertexPayload + 12 + lane * 4, std::bit_cast<uint32_t> (float (lane + 1) * 0.125f)
	);
    }
    for (size_t lane = 0; lane < 4; ++lane) {
	patch32 (
	    fixture.bytes, fixture.vertexPayload + 24 + lane * 4, std::bit_cast<uint32_t> (float (lane + 1) * -0.25f)
	);
    }
    const auto mesh = parseMeshes (fixture.bytes).meshes.at (0);
    REQUIRE (mesh.meshFlags == 0);
    REQUIRE (mesh.normals.size () == 3);
    REQUIRE (mesh.tangents.size () == 3);
    const std::array<float, 3> expectedNormal { 0.125f, 0.25f, 0.375f };
    const std::array<float, 4> expectedTangent { -0.25f, -0.5f, -0.75f, -1.0f };
    REQUIRE (mesh.normals[0] == expectedNormal);
    REQUIRE (mesh.tangents[0] == expectedTangent);
    patch32 (
	fixture.bytes, fixture.vertexPayload + 12, std::bit_cast<uint32_t> (std::numeric_limits<float>::quiet_NaN ())
    );
    REQUIRE_THROWS (parseMeshes (fixture.bytes));
}

TEST_CASE ("Legacy static MDLV v4 uses its header attribute mask", "[puppet][mesh]") {
    auto fixture = makeFixture (4, 0x0000000b, 0x0000000b, 1, 0);
    const auto model = parseMeshes (fixture.bytes);
    REQUIRE (model.version == 4);
    REQUIRE (model.meshes.size () == 1);
    REQUIRE (model.meshes[0].vertexMask == 0x0000000bu);
    REQUIRE (model.meshes[0].normals.size () == 3);
    REQUIRE (model.meshes[0].texcoords.size () == 3);
    REQUIRE (model.meshes[0].indices == std::vector<uint16_t> { 0, 1, 2 });
}

TEST_CASE ("MDLV first mesh rejects unsupported and truncated layouts", "[puppet][mesh]") {
    auto fixture = makeFixture ();
    REQUIRE_THROWS (parse (makeFixture (23, 0, 0x01800008).bytes));
    REQUIRE_THROWS (parse (makeFixture (13, 0, 0x01800009).bytes));
    REQUIRE_THROWS (parse (makeFixture (24).bytes));
    REQUIRE_THROWS (parse (makeFixture (23, 0, 0x0180000f, 2).bytes));
    for (size_t length : { size_t (0), size_t (4), fixture.vertexLength + 3, fixture.vertexPayload + 239,
			   fixture.indexLength + 3, fixture.indexPayload + 5 }) {
	REQUIRE_THROWS (parse (fixture.bytes.substr (0, length)));
    }
    patch32 (fixture.bytes, fixture.vertexLength, std::numeric_limits<uint32_t>::max ());
    REQUIRE_THROWS (parse (fixture.bytes));
    fixture = makeFixture ();
    patch32 (fixture.bytes, fixture.indexLength, std::numeric_limits<uint32_t>::max ());
    REQUIRE_THROWS (parse (fixture.bytes));
    fixture = makeFixture ();
    fixture.bytes[fixture.indexPayload + 4] = char (3);
    REQUIRE_THROWS (parse (fixture.bytes));
    fixture = makeFixture ();
    patch32 (fixture.bytes, fixture.vertexPayload, std::bit_cast<uint32_t> (std::numeric_limits<float>::infinity ()));
    REQUIRE_THROWS (parse (fixture.bytes));
    fixture = makeFixture ();
    patch32 (
	fixture.bytes, fixture.vertexPayload + 72, std::bit_cast<uint32_t> (std::numeric_limits<float>::quiet_NaN ())
    );
    REQUIRE_THROWS (parse (fixture.bytes));
    fixture = makeFixture ();
    patch32 (
	fixture.bytes, fixture.vertexPayload + 56, std::bit_cast<uint32_t> (std::numeric_limits<float>::infinity ())
    );
    REQUIRE_THROWS (parse (fixture.bytes));
    REQUIRE_THROWS (parse ("MDLV0023")); // Missing NUL and all fields.
}

TEST_CASE ("One MDLV version supports different declared vertex strides", "[puppet][mesh]") {
    for (uint32_t mask : { 0x01800009u, 0x0180000fu, 0x0181000eu }) {
	auto fixture = makeFixture (21, 0, mask);
	if (mask & 0x10000) {
	    patch32 (fixture.bytes, fixture.vertexPayload + 12, std::bit_cast<uint32_t> (1.0f));
	}
	const auto model = parseMeshes (fixture.bytes);
	const auto& mesh = model.meshes.at (0);
	REQUIRE (mesh.positions[0] == std::array<float, 3> { 1, 2, 3 });
	REQUIRE (mesh.blendWeights[0] == std::array<float, 4> { 0.25f, 0.75f, 0, 0 });
	REQUIRE (mesh.texcoords[2] == std::array<float, 2> { 0.5f, 1 });
	REQUIRE (mesh.morphIndices[0] == ((mask & 0x10000) ? 1.0f : 0.0f));
    }
    REQUIRE (puppetTexcoordScale (512, 300, 1024, 512, false) == std::array<float, 2> { 0.5f, 300.0f / 512 });
    REQUIRE (puppetTexcoordScale (512, 300, 1024, 512, true) == std::array<float, 2> { 1, 1 });
}

namespace {
std::string morphFixture (uint32_t flags) {
    std::string bytes;
    cstr (bytes, "MDMP0001");
    u32 (bytes, 0);
    bytes += std::string ("\1\0", 2);
    f32 (bytes, 8);
    u32 (bytes, 1);
    u64 (bytes, 99);
    cstr (bytes, "blink");
    u32 (bytes, 6);
    bytes += std::string ("\xff\x7f\0\x80\0\0", 6);
    for (const uint32_t flag : { 0x400u, 0x800u }) {
	if (flags & flag) {
	    u32 (bytes, 6);
	    bytes.append (6, '\0');
	}
    }
    if (flags & 0x1000) {
	u32 (bytes, 2);
	bytes += std::string ("\0\x40", 2);
    }
    if (flags & 0x2000) {
	u32 (bytes, 3);
	u32 (bytes, 2);
	f32 (bytes, 1);
	f32 (bytes, 4);
    }
    patch32 (bytes, 9, uint32_t (bytes.size ()));
    return bytes;
}
PuppetMorphData parseMorph (const std::string& bytes, uint32_t flags) {
    return parsePuppetMorph ({ reinterpret_cast<const uint8_t*> (bytes.data ()), bytes.size () }, 0, flags);
}
}

TEST_CASE ("MDMP position-only targets retain deltas and opaque alpha", "[puppet][morph]") {
    const auto morph = parseMorph (morphFixture (0), 0);
    REQUIRE (morph.scale == 8);
    REQUIRE (morph.vertexCount == 1);
    REQUIRE (morph.names == std::vector<std::string> { "blink" });
    REQUIRE (morph.sample (1, 0) == std::array<float, 4> { 1, -1, 0, 1 });
    REQUIRE (morph.sample (0, 0) == std::array<float, 4> { 0, 0, 0, 1 });
    REQUIRE (morph.sample (2, 0) == std::array<float, 4> { 0, 0, 0, 1 });
    REQUIRE (morph.sample (1, 1) == std::array<float, 4> { 0, 0, 0, 1 });
    REQUIRE_FALSE (morph.hasAlpha);
}

TEST_CASE ("MDMP consumes optional streams and target bone modifiers", "[puppet][morph]") {
    const auto morph = parseMorph (morphFixture (0x3c00), 0x3c00);
    REQUIRE (morph.sample (1, 0)[0] == 1);
    REQUIRE (morph.sample (1, 0)[3] == Catch::Approx (16384.0f / 32767));
    REQUIRE (morph.hasAlpha);
    REQUIRE (morph.boneRules.size () == 1);
    REQUIRE (morph.boneRules[0].bone == 3);
    REQUIRE (morph.boneRules[0].axis);
    REQUIRE (morph.boneRules[0].edge0 == 1);
    REQUIRE (morph.boneRules[0].edge1 == 4);
}

TEST_CASE ("MDMP rejects truncated sections and invalid payload sizes", "[puppet][morph]") {
    const auto fixture = morphFixture (0x3c00);
    for (size_t length = 0; length < fixture.size (); length++) {
	REQUIRE_THROWS (parseMorph (fixture.substr (0, length), 0x3c00));
    }
    auto bytes = fixture;
    patch32 (bytes, 19, UINT32_MAX);
    REQUIRE_THROWS (parseMorph (bytes, 0x3c00));
    bytes = fixture;
    patch32 (bytes, 15, std::bit_cast<uint32_t> (std::numeric_limits<float>::infinity ()));
    REQUIRE_THROWS (parseMorph (bytes, 0x3c00));
    bytes = fixture;
    patch32 (bytes, 9, uint32_t (bytes.size () - 1));
    REQUIRE_THROWS (parseMorph (bytes, 0x3c00));
    bytes = fixture;
    bytes[7] = '2';
    REQUIRE_THROWS (parseMorph (bytes, 0x3c00));
}

TEST_CASE ("Puppet parts use stable integer draw keys and restore rest order", "[puppet][order]") {
    const std::array<PuppetMeshData::BoneRange, 3> parts { {
	{ 0, 0, 0, 3 }, { 1, 0, 3, 3 }, { 2, 0, 6, 3 }
    } };
    std::vector<uint32_t> order;
    const std::array<float, 3> animated { 2, 0, 1 };
    sortPuppetParts (parts, {}, animated, order);
    REQUIRE (order == std::vector<uint32_t> { 1, 2, 0 });
    const std::array<float, 3> tied { .9f, .8f, .7f };
    sortPuppetParts (parts, {}, tied, order);
    REQUIRE (order == std::vector<uint32_t> { 1, 2, 0 });
    const std::array<int, 3> rest { 0, 1, 2 };
    sortPuppetParts (parts, rest, {}, order);
    REQUIRE (order == std::vector<uint32_t> { 0, 1, 2 });
    const std::array<float, 3> invalid { std::numeric_limits<float>::infinity (), 0, 0 };
    sortPuppetParts (parts, rest, invalid, order);
    REQUIRE (order == std::vector<uint32_t> { 0, 1, 2 });
}

TEST_CASE ("Puppet clipping validates offsets and triangle ranges", "[puppet][clipping]") {
    std::string bytes ("\0\1", 2);
    u32 (bytes, 32);
    for (uint32_t part = 0; part < 2; part++) {
	u32 (bytes, part);
	u32 (bytes, 0);
	u32 (bytes, part * 3);
	u32 (bytes, 3);
    }
    u32 (bytes, 1);
    u64 (bytes, 99);
    cstr (bytes, "mask");
    u32 (bytes, PuppetClipping::HideSources);
    u32 (bytes, 1);
    u32 (bytes, 1);
    u32 (bytes, 1);
    u32 (bytes, 0);
    const auto read = [] (const std::string& data, size_t offset = 0) {
	return PuppetClipping::read (std::vector<char> (data.begin (), data.end ()), offset, 23, 6);
    };
    auto clipping = read (bytes);
    REQUIRE (clipping.has_value ());
    clipping->build ({ 0, 1, 2, 3, 4, 5 });
    REQUIRE (clipping->commands == std::vector<int> { PuppetClipping::Mask, 0 });
    REQUIRE (clipping->indices == std::vector<uint16_t> { 0, 1, 2, 3, 4, 5 });
    REQUIRE_THROWS (read (bytes, SIZE_MAX));
    REQUIRE_THROWS (read (bytes, bytes.size ()));
    for (size_t length = 0; length < bytes.size (); length++) {
	REQUIRE_THROWS (read (bytes.substr (0, length)));
    }
    auto invalid = bytes;
    patch32 (invalid, 2, 31);
    REQUIRE_THROWS (read (invalid));
    invalid = bytes;
    patch32 (invalid, 14, 1);
    REQUIRE_THROWS (read (invalid));
    invalid = bytes;
    patch32 (invalid, uint32_t (invalid.size () - 4), 2);
    REQUIRE_THROWS (read (invalid));
}

TEST_CASE ("Nested clipping resolves parents before or after child targets", "[puppet][clipping]") {
    PuppetClipping clipping;
    clipping.parts = { { 0, 0, 0, 3 }, { 1, 0, 3, 3 }, { 2, 0, 6, 3 } };
    clipping.records = { { "parent", PuppetClipping::HideSources, { 1 }, { 0 }, -1 },
			 { "child", PuppetClipping::HideSources, { 2 }, { 1 }, 0 } };
    clipping.order = { 2, 1, 0 };
    clipping.build ({ 0, 1, 2, 3, 4, 5, 6, 7, 8 });
    REQUIRE (clipping.commands[0] == PuppetClipping::NestedMask);
    REQUIRE (clipping.commands[1] == 1);
    REQUIRE (clipping.commands[2] == 0);
    const auto ancestor = clipping.draws.at (clipping.commands[3]);
    REQUIRE (ancestor.count == 3);
    REQUIRE (clipping.indices.at (ancestor.offset) == 0);
}
