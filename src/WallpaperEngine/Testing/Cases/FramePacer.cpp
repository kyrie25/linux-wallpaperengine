#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/Drivers/FramePacer.h"

#include <chrono>
#include <cstdint>
#include <vector>

using WallpaperEngine::Render::Drivers::FramePacer;
using namespace std::chrono_literals;

namespace {
// Runs the pacer with a fixed amount of work per frame and sleeps that last
// exactly as requested. Returns each frame's period in nanoseconds.
std::vector<int64_t> pace (int fps, std::chrono::nanoseconds work, int frames, std::vector<int64_t>* sleeps = nullptr) {
    FramePacer pacer (fps);
    auto now = FramePacer::Clock::time_point {} + 1s;
    auto previous = now;
    std::vector<int64_t> periods;
    for (int frame = 0; frame < frames; ++frame) {
	now += work;
	const int64_t milliseconds = pacer.sleepMilliseconds (now);
	if (sleeps) sleeps->push_back (milliseconds);
	const auto done = now;
	if (milliseconds > 0) now += std::chrono::milliseconds (milliseconds);
	pacer.woke (done, now);
	periods.push_back (std::chrono::duration_cast<std::chrono::nanoseconds> (now - previous).count ());
	previous = now;
    }
    return periods;
}
} // namespace

TEST_CASE ("Frame pacer spends each second's budget over fps frames like native 140110630", "[frame-pacer]") {
    // fps 4 without work: the first second only spans three frames (the
    // counter starts at zero and is incremented before the share), then every
    // four frames share one second exactly.
    std::vector<int64_t> sleeps;
    pace (4, 0ns, 11, &sleeps);
    REQUIRE (sleeps == std::vector<int64_t> {333, 333, 334, 250, 250, 250, 250, 250, 250, 250, 250});
}

TEST_CASE ("Frame pacer subtracts the frame's work and truncates to whole milliseconds", "[frame-pacer]") {
    std::vector<int64_t> sleeps;
    pace (30, 5ms, 3, &sleeps);
    // The first frame starts the schedule (no work yet): 1 s / 29 = 34.48 ms.
    // Then (1 s - 34 ms - 5 ms) / 28 - 5 ms = 29.32 ms and (961 ms - 29 ms - 5 ms) / 27 - 5 ms = 29.33 ms.
    REQUIRE (sleeps == std::vector<int64_t> {34, 29, 29});

    // Over many seconds the frames still average 1/fps.
    const auto periods = pace (30, 3ms, 30 * 20);
    int64_t total = 0;
    for (size_t frame = 30; frame < periods.size (); ++frame) total += periods[frame];
    const double average = static_cast<double> (total) / static_cast<double> (periods.size () - 30);
    REQUIRE (average > 33.0e6);
    REQUIRE (average < 33.7e6);
}

TEST_CASE ("Frame pacer skips the sleep when the frame overruns its budget", "[frame-pacer]") {
    std::vector<int64_t> sleeps;
    pace (30, 50ms, 4, &sleeps);
    // The first frame starts the schedule; every later frame does more work than its share.
    for (size_t frame = 1; frame < sleeps.size (); ++frame) REQUIRE (sleeps[frame] <= 0);
    REQUIRE (FramePacer (0).sleepMilliseconds (FramePacer::Clock::now ()) == 0);
}
