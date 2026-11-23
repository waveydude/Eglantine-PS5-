// Host test for video_core/flash_counter.h:
//   c++ -std=c++20 -I<Eglantine>/src tools/test-flash-counter.cpp && ./a.out
#include "video_core/flash_counter.h"
#include <cstdio>
#include <vector>
using namespace VideoCommon::FlashCount;
using Frame = std::array<std::uint8_t, Samples>;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static Frame Flat(std::uint8_t v) { Frame f; f.fill(v); return f; }
// A textured picture shifted right by `shift` points (a camera pan).
static Frame Scene(int shift) {
    Frame f;
    for (std::uint32_t y = 0; y < Rows; ++y)
        for (std::uint32_t x = 0; x < Cols; ++x)
            f[y * Cols + x] = static_cast<std::uint8_t>(((x + shift) * 37 + y * 53) % 200 + 20);
    return f;
}
static int Count(const std::vector<Frame>& frames, std::vector<Event>* out = nullptr, std::uint64_t start = 0) {
    Detector d;
    int n = 0;
    for (std::size_t i = 0; i < frames.size(); ++i)
        if (auto e = d.Feed(start + i, frames[i])) { ++n; if (out) out->push_back(*e); }
    return n;
}
int main() {
    // Still picture: nothing.
    CHECK(Count(std::vector<Frame>(10, Scene(0))) == 0);
    // One white frame in a still scene: one flash, on that frame, brighter, whole screen.
    {
        std::vector<Frame> f(6, Scene(0)); f[3] = Flat(250);
        std::vector<Event> ev; CHECK(Count(f, &ev) == 1);
        CHECK(!ev.empty() && ev[0].frame == 3 && ev[0].brighter);
    }
    // One black frame: a flash too, darker.
    {
        std::vector<Frame> f(6, Flat(180)); f[2] = Flat(5);
        std::vector<Event> ev; CHECK(Count(f, &ev) == 1); CHECK(!ev.empty() && !ev[0].brighter);
    }
    // Camera pan: every frame differs from the last, but never back: nothing.
    { std::vector<Frame> f; for (int s = 0; s < 20; ++s) f.push_back(Scene(s)); CHECK(Count(f) == 0); }
    // Fade to white in big steps: nothing (A and C never agree).
    { std::vector<Frame> f; for (int v = 0; v <= 250; v += 50) f.push_back(Flat(static_cast<std::uint8_t>(v))); CHECK(Count(f) == 0); }
    // Scene cut A -> B -> C: nothing.
    { std::vector<Frame> f{Flat(30), Flat(30), Flat(120), Flat(220), Flat(220)}; CHECK(Count(f) == 0); }
    // A bright patch on the ground for one frame: 6x4 = 24 points (4.2%): a flash, boxed there.
    {
        std::vector<Frame> f(5, Scene(0)); f[2] = Scene(0);
        for (std::uint32_t y = 12; y < 16; ++y) for (std::uint32_t x = 10; x < 16; ++x) f[2][y * Cols + x] = 255;
        // the patch's points must start dark enough to jump 48
        for (auto* fr : {&f[0], &f[1], &f[3], &f[4]})
            for (std::uint32_t y = 12; y < 16; ++y) for (std::uint32_t x = 10; x < 16; ++x) (*fr)[y * Cols + x] = 60;
        std::vector<Event> ev; CHECK(Count(f, &ev) == 1);
        CHECK(!ev.empty() && ev[0].x0 == 10 && ev[0].x1 == 15 && ev[0].y0 == 12 && ev[0].y1 == 15 && ev[0].points == 24);
    }
    // A small sparkle (10 points, 1.7%): under the 4% share: nothing.
    {
        std::vector<Frame> f(5, Flat(60)); for (int i = 0; i < 10; ++i) f[2][i * 7] = 255; CHECK(Count(f) == 0);
    }
    // A jump smaller than 48: nothing.
    { std::vector<Frame> f(5, Flat(100)); f[2] = Flat(140); CHECK(Count(f) == 0); }
    // Exactly 48: counted (boundary).
    { std::vector<Frame> f(5, Flat(100)); f[2] = Flat(148); CHECK(Count(f) == 1); }
    // A and C 17 apart (over the settle of 16): not counted; 16 apart: counted.
    { std::vector<Frame> f{Flat(100), Flat(100), Flat(220), Flat(117), Flat(117)}; CHECK(Count(f) == 0); }
    { std::vector<Frame> f{Flat(100), Flat(100), Flat(220), Flat(116), Flat(116)}; CHECK(Count(f) == 1); }
    // A gap in the frame numbers starts over: A, [gap], B, A must not count B.
    {
        Detector d; int n = 0;
        if (d.Feed(1, Flat(100))) ++n; if (d.Feed(2, Flat(100))) ++n;
        if (d.Feed(4, Flat(250))) ++n; if (d.Feed(5, Flat(100))) ++n;
        CHECK(n == 0);
    }
    // Two flashes in a run, frames numbered from 1000.
    {
        std::vector<Frame> f(9, Flat(90)); f[2] = Flat(250); f[6] = Flat(0);
        std::vector<Event> ev; CHECK(Count(f, &ev, 1000) == 2);
        CHECK(ev.size() == 2 && ev[0].frame == 1002 && ev[1].frame == 1006);
    }
    // A two-frame flash (B B) is not a one-frame flash: nothing (documented limit).
    { std::vector<Frame> f{Flat(90), Flat(90), Flat(250), Flat(250), Flat(90), Flat(90)}; CHECK(Count(f) == 0); }
    std::printf(failures ? "%d FAILED\n" : "all flash counter checks passed\n", failures);
    return failures ? 1 : 0;
}
