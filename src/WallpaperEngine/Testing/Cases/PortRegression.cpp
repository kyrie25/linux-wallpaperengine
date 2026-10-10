#include "WallpaperEngine/Data/Model/EffectConditions.h"
#include "WallpaperEngine/Application/LiveControl.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/EffectParser.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Utils/MemoryStream.h"
#include "WallpaperEngine/Render/Objects/PuppetRig.h"
#include "WallpaperEngine/Render/WallpaperState.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cstring>
#include <limits>

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Render::Objects;
extern float g_Time;

TEST_CASE ("Memory streams reject out of bounds seeks without losing their cursor", "[port][stream]") {
    auto bytes = std::make_unique<char[]> (4);
    std::memcpy (bytes.get (), "abcd", 4);
    WallpaperEngine::Data::Utils::MemoryStream stream (std::move (bytes), 4);
    stream.seekg (2);
    REQUIRE (stream.get () == 'c');
    for (auto offset : {std::streamoff (-5), std::streamoff (5), std::numeric_limits<std::streamoff>::max ()}) {
        stream.seekg (offset, std::ios::beg);
        REQUIRE (stream.fail ());
        stream.clear ();
        REQUIRE (stream.tellg () == 3);
    }
    stream.seekg (-1, std::ios::end);
    REQUIRE (stream.get () == 'd');
    stream.seekg (0);
    REQUIRE (stream.get () == 'a');
    stream.seekg (4);
    REQUIRE (stream.tellg () == 4);
}

TEST_CASE ("Effect conditions use instance combos and AND comparisons", "[port][effect]") {
    using WallpaperEngine::Data::Parsers::EffectParser;
    using WallpaperEngine::Data::JSON::JSON;
    const auto conditions = EffectParser::parseConditions (
        JSON::array ({{{"A", {{"op", "ge"}, {"value", 2}}}, {"B", {{"op", "lt"}, {"value", 4}}}}, {{"C", 0}}}));
    REQUIRE (effectConditionsMatch (conditions, {{"A", 2}, {"B", 3}}));
    REQUIRE_FALSE (effectConditionsMatch (conditions, {{"A", 1}, {"B", 3}}));
    REQUIRE_FALSE (effectConditionsMatch (conditions, {{"A", 2}, {"B", 4}}));
    REQUIRE_FALSE (effectConditionsMatch (conditions, {{"A", 2}, {"B", 3}, {"C", 1}}));
    REQUIRE (effectConditionsMatch ({}, {}));
}

TEST_CASE ("Puppet layers blend independently and preserve bind pose for unanimated bones", "[port][puppet]") {
    PuppetRig rig;
    rig.bones.resize (1);
    rig.bindModel.emplace_back (1.0f);
    PuppetAnimationClip clip;
    clip.id = 42;
    clip.fps = 1;
    clip.frameCount = 1;
    clip.mode = "loop";
    clip.boneAnimated = {true};
    clip.boneFlags = {0};
    clip.boneTracks.resize (1);
    clip.boneTracks[0].resize (2);
    clip.boneTracks[0][0].position = glm::vec3 (10, 0, 0);
    clip.boneTracks[0][1].position = glm::vec3 (20, 0, 0);
    rig.clips.push_back (clip);
    const Project project {};
    auto layer = WallpaperEngine::Data::Parsers::ObjectParser::parseAnimationLayer (
        WallpaperEngine::Data::JSON::JSON {{"animation", 42}, {"blend", 0.5}, {"visible", true}}, project);
    REQUIRE (rig.addLayer (*layer, nullptr, 0, false));
    g_Time = 0;
    rig.updatePose (glm::mat4 (1.0f));
    REQUIRE (rig.skinMatrices ().at (0)[3].x == Catch::Approx (5));
    g_Time = 0.5f;
    rig.updatePose (glm::mat4 (1.0f));
    REQUIRE (rig.skinMatrices ().at (0)[3].x == Catch::Approx (7.5));
    layer->visible->value->update (false, DynamicValue::UpdateSource::Script);
    g_Time = 0.6f;
    rig.updatePose (glm::mat4 (1.0f));
    REQUIRE (rig.skinMatrices ().at (0)[3].x == Catch::Approx (0));
}

TEST_CASE ("Puppet draw order changes at baked frames and restores hidden tracks", "[port][puppet][order]") {
    PuppetRig rig;
    rig.bones.resize (2);
    rig.bindModel.assign (2, glm::mat4 (1));
    rig.boneDrawOrder = { 0, 1 };
    rig.drawOrderEnabled = true;
    PuppetAnimationClip clip;
    clip.id = 42;
    clip.fps = 2;
    clip.frameCount = 2;
    clip.mode = "loop";
    clip.boneAnimated = { true, true };
    clip.boneFlags = { 0, 0 };
    clip.boneTracks.resize (2, std::vector<PuppetKeyframe> (3));
    clip.drawOrderTracks = { { 2, 0, 2 }, { 0, 1, 0 } };
    rig.clips.push_back (std::move (clip));
    const Project project {};
    auto layer = WallpaperEngine::Data::Parsers::ObjectParser::parseAnimationLayer (
	WallpaperEngine::Data::JSON::JSON { { "animation", 42 }, { "blend", 1 }, { "visible", true } }, project
    );
    REQUIRE (rig.addLayer (*layer, nullptr, 0, false));
    g_Time = 0;
    rig.updatePose (glm::mat4 (1));
    REQUIRE (rig.drawOrder == std::vector<float> { 2, 0 });
    g_Time = .4f;
    rig.updatePose (glm::mat4 (1));
    REQUIRE (rig.drawOrder == std::vector<float> { 2, 0 });
    g_Time = .6f;
    rig.updatePose (glm::mat4 (1));
    REQUIRE (rig.drawOrder == std::vector<float> { 0, 1 });
    g_Time = 1.1f;
    rig.updatePose (glm::mat4 (1));
    REQUIRE (rig.drawOrder == std::vector<float> { 2, 0 });
    layer->visible->value->update (false, DynamicValue::UpdateSource::Script);
    rig.updatePose (glm::mat4 (1));
    REQUIRE_FALSE (rig.drawOrderTouched);
    REQUIRE (rig.drawOrder == std::vector<float> { 0, 1 });
}

TEST_CASE ("Live controls validate the whole update before applying it", "[port][control]") {
    using WallpaperEngine::Application::LiveControl;
    using WallpaperEngine::Data::JSON::JSON;
    const auto control = LiveControl::parse (JSON {{"fps", 60}, {"volume", 15},
        {"scaling", "fill"}, {"alignment", {0.0, 1.0}}});
    REQUIRE (control.fps == 60);
    REQUIRE (control.volume == 15);
    REQUIRE (*control.alignment == glm::vec2 (0, 1));
    REQUIRE (LiveControl::parse (JSON {{"fps", 0}}).fps == 1);
    REQUIRE (LiveControl::parse (JSON {{"volume", -10}}).volume == 0);
    REQUIRE (LiveControl::parse (JSON {{"volume", 500}}).volume == 128);
    REQUIRE_THROWS (LiveControl::parse (JSON {{"fps", 60}, {"scaling", "invalid"}}));
    REQUIRE_THROWS (LiveControl::parse (JSON {{"fps", "60"}}));
    REQUIRE_THROWS (LiveControl::parse (JSON {{"alignment", {1}}}));
    REQUIRE_THROWS (LiveControl::parse (JSON {{"antiAliasing", 4}}));
}

TEST_CASE ("Live scaling and alignment invalidate cached UVs", "[port][control]") {
    using WallpaperEngine::Render::WallpaperState;
    WallpaperState state (WallpaperState::TextureUVsScaling::ZoomFillUVs, 0);
    const glm::ivec4 viewport (0, 0, 1920, 1080);
    state.updateState (viewport, true, 3840, 1600);
    REQUIRE_FALSE (state.hasChanged (viewport, true, 3840, 1600));
    const float initial = state.getTextureUVs ().ustart;
    state.setAlignment ({0, 0.5f});
    REQUIRE (state.hasChanged (viewport, true, 3840, 1600));
    state.updateState (viewport, true, 3840, 1600);
    REQUIRE (state.getTextureUVs ().ustart < initial);
    state.setTextureUVsStrategy (WallpaperState::TextureUVsScaling::StretchUVs);
    REQUIRE (state.hasChanged (viewport, true, 3840, 1600));
    state.updateState (viewport, true, 3840, 1600);
    REQUIRE (state.getTextureUVs ().ustart == 0);
    REQUIRE (state.getTextureUVs ().uend == 1);
}
