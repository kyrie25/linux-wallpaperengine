#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "WallpaperEngine/Render/TextureAnimation.h"

#include <limits>
#include <utility>
#include <vector>

using namespace WallpaperEngine::Data::Assets;
using WallpaperEngine::Render::frameAtTime;
using WallpaperEngine::Render::framePhaseAtTime;

TEST_CASE ("Texture animation uses its own unequal frame durations and page indices", "[render][texture][animation]") {
    std::vector<FrameSharedPtr> frames;
    for (const auto [page, seconds] : {std::pair {2u, 0.25f}, {0u, 0.5f}, {1u, 0.75f}}) {
        auto frame = std::make_shared<Frame> ();
        frame->frameNumber = page;
        frame->frametime = seconds;
        frames.push_back (std::move (frame));
    }
    REQUIRE (frameAtTime (frames, 0.0)->frameNumber == 2);
    REQUIRE (frameAtTime (frames, 0.25)->frameNumber == 2);
    REQUIRE (frameAtTime (frames, 0.251)->frameNumber == 0);
    REQUIRE (frameAtTime (frames, 0.75)->frameNumber == 0);
    REQUIRE (frameAtTime (frames, 0.751)->frameNumber == 1);
    REQUIRE (frameAtTime (frames, 1.5)->frameNumber == 2);
    REQUIRE (frameAtTime (frames, 1.751)->frameNumber == 0);
    REQUIRE (frameAtTime (frames, -0.1)->frameNumber == 1);
    REQUIRE (framePhaseAtTime (frames, 0.125)->ordinal == 0);
    REQUIRE (framePhaseAtTime (frames, 0.125)->fraction == Catch::Approx (0.5));
    REQUIRE (framePhaseAtTime (frames, 0.25)->ordinal == 0);
    REQUIRE (framePhaseAtTime (frames, 0.25)->fraction == Catch::Approx (1.0));
    REQUIRE (framePhaseAtTime (frames, 0.5)->ordinal == 1);
    REQUIRE (framePhaseAtTime (frames, 0.5)->fraction == Catch::Approx (0.5));
    REQUIRE (framePhaseAtTime (frames, 1.125)->ordinal == 2);
    REQUIRE (framePhaseAtTime (frames, 1.125)->fraction == Catch::Approx (0.5));

    // Pass input 0 can change independently of the renderable's primary image.
    // The selector has no primary-image duration argument to contaminate its clock.
    frames[1]->frametime = 0.1f;
    REQUIRE (frameAtTime (frames, 0.5)->frameNumber == 1);
}

TEST_CASE ("Invalid frame timing fails without undefined page selection", "[render][texture][animation]") {
    std::vector<FrameSharedPtr> frames;
    REQUIRE (frameAtTime (frames, 0.0) == nullptr);
    auto frame = std::make_shared<Frame> ();
    frames.push_back (frame);
    REQUIRE (frameAtTime (frames, 1.0) == nullptr);
    frame->frametime = 0.1f;
    REQUIRE (frameAtTime (frames, std::numeric_limits<double>::quiet_NaN ()) == nullptr);
}

namespace {
std::vector<FrameSharedPtr> imageAnimationFrames () {
    std::vector<FrameSharedPtr> frames;
    for (float duration : {0.25f, 0.5f, 0.75f}) {
        auto frame = std::make_shared<Frame> ();
        frame->frametime = duration;
        frame->frameNumber = static_cast<uint32_t> (frames.size ());
        frames.push_back (frame);
    }
    return frames;
}
}

TEST_CASE ("Image animation controls detach and rejoin a shared texture timeline",
           "[render][texture][animation][script]") {
    using WallpaperEngine::Render::ImageTextureAnimation;
    const auto frames = imageAnimationFrames ();
    auto shared = std::make_shared<WallpaperEngine::Render::SharedTextureAnimation> (frames);
    ImageTextureAnimation source (frames, shared), clone (frames, shared);
    shared->sample (0.375f, 0);
    REQUIRE (source.getFrame () == 1);
    REQUIRE (clone.getFrame () == 1);
    REQUIRE (source.frameCount () == 3);
    REQUIRE (source.duration () == 1.5f);
    REQUIRE (source.rate () == 1.0f);
    REQUIRE (source.isPlaying ());
    REQUIRE_FALSE (source.hasOverride ());

    source.pause ();
    shared->sample (0.625f, 1);
    source.advance (0.4f);
    REQUIRE (source.getFrame () == 1);
    REQUIRE (clone.getFrame () == 2);
    REQUIRE_FALSE (source.isPlaying ());
    // Pause copied the shared intra-frame progress (.125), not just ordinal.
    source.play ();
    source.advance (0.4f);
    REQUIRE (source.getFrame () == 2);
    REQUIRE (clone.getFrame () == 2);

    source.stop ();
    REQUIRE (source.getFrame () == 0);
    source.setFrame (1);
    source.advance (1.0f);
    REQUIRE_FALSE (source.isPlaying ());
    REQUIRE (source.getFrame () == 1);
    source.setRate (9.0f);
    source.join ();
    REQUIRE (source.isPlaying ());
    REQUIRE (source.getFrame () == 2);
    REQUIRE (source.rate () == 9.0f);
    source.setFrame (0);
    REQUIRE (source.isPlaying ());
    REQUIRE (source.hasOverride ());
    // Controls never mutate shared frame metadata or another image's state.
    REQUIRE (frames.front ()->frametime == 0.25f);
    REQUIRE_FALSE (clone.hasOverride ());
}

TEST_CASE ("Native shared image animation samples once per engine frame and retains its cursor",
           "[render][texture][animation][script]") {
    using WallpaperEngine::Render::ImageTextureAnimation;
    using WallpaperEngine::Render::SharedTextureAnimation;
    const auto frames = imageAnimationFrames ();
    auto shared = std::make_shared<SharedTextureAnimation> (frames);
    ImageTextureAnimation first (frames, shared), second (frames, shared);
    // Loading and querying a handle does not consume wall time or advance
    // the texture. The initial render consumes only this frame's delta.
    REQUIRE (first.getFrame () == 0);
    REQUIRE (second.getFrame () == 0);
    shared->sample (0.125f, 100);
    REQUIRE (shared->cursor ().subframe == 0.125f);
    // Several passes and a second image may sample the same cached texture.
    shared->sample (3.0f, 100);
    shared->sample (0.25f, 100);
    REQUIRE (first.getFrame () == 0);
    REQUIRE (second.getFrame () == 0);
    REQUIRE (shared->cursor ().subframe == 0.125f);
    shared->sample (3.0f, 101);
    REQUIRE (first.getFrame () == 1);
    REQUIRE (second.getFrame () == 1);
    REQUIRE (shared->cursor ().subframe == 0.5f);
    shared->sample (0.001f, 102);
    REQUIRE (first.getFrame () == 2);
    REQUIRE (shared->cursor ().subframe == Catch::Approx (0.001f).margin (1e-7f));

    first.pause ();
    second.stop ();
    // A private override does not sample/advance the shared clock. When all
    // images override or are not drawn, its last cursor remains available.
    first.advance (2.0f);
    second.advance (2.0f);
    REQUIRE (shared->getFrame () == 2);
    REQUIRE (shared->cursor ().subframe == Catch::Approx (0.001f).margin (1e-7f));
    first.join ();
    REQUIRE (first.getFrame () == 2);
    REQUIRE (first.isPlaying ());
    shared->sample (0.8f, 105);
    REQUIRE (first.getFrame () == 0);
    REQUIRE (second.getFrame () == 0);
    REQUIRE_FALSE (second.isPlaying ());
}

TEST_CASE ("Native short-duration shared atlases advance one authored frame per render tick",
           "[render][texture][animation]") {
    // Native 147-frame .03 atlas at 30fps: +12 frames per .4 seconds,
    // including invisibility/alpha controls on another private instance.
    std::vector<FrameSharedPtr> frames;
    for (int i = 0; i < 147; ++i) {
        auto frame = std::make_shared<Frame> ();
        frame->frametime = 0.03f;
        frames.push_back (frame);
    }
    WallpaperEngine::Render::SharedTextureAnimation shared (frames);
    for (uint32_t tick = 0; tick < 12; ++tick) {
        shared.sample (1.0f / 30.0f, tick);
        shared.sample (1.0f / 30.0f, tick);
    }
    REQUIRE (shared.getFrame () == 12);
    shared.sample (4.0f, 12);
    REQUIRE (shared.getFrame () == 13);
    REQUIRE (shared.cursor ().subframe == 0.03f);
    shared.sample (0.001f, 13);
    REQUIRE (shared.getFrame () == 14);
    REQUIRE (shared.cursor ().subframe == Catch::Approx (0.001f).margin (1e-7f));
}

TEST_CASE ("Image animation updates follow native single-step duration clamps",
           "[render][texture][animation][script]") {
    using WallpaperEngine::Render::ImageTextureAnimation;
    ImageTextureAnimation state (imageAnimationFrames ());
    state.setFrame (0);
    state.advance (3.0f);
    REQUIRE (state.getFrame () == 1);
    state.advance (0.001f);
    REQUIRE (state.getFrame () == 2);
    state.advance (0.749f);
    REQUIRE (state.getFrame () == 0);

    state.setFrame (0);
    state.setRate (-1.0f);
    state.advance (3.0f);
    REQUIRE (state.getFrame () == 2);
    state.advance (0.001f);
    REQUIRE (state.getFrame () == 1);
    state.advance (0.499f);
    REQUIRE (state.getFrame () == 0);

    // Native retains raw seeks while rate zero. Playback performs its own
    // one-step correction only when the first fallback duration elapses.
    state.setRate (0.0f);
    state.setFrame (147);
    state.advance (1.0f);
    REQUIRE (state.getFrame () == 147);
    state.setRate (1.0f);
    state.advance (0.125f);
    REQUIRE (state.getFrame () == 147);
    state.advance (0.125f);
    REQUIRE (state.getFrame () == 0);
}
