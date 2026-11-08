// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcstream_diag.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (false)

using namespace g3t::npcstream_diag;

struct Capture {
    std::vector<std::string> lines;
    bool terminated = true;

    void Receive(char const* line)
    {
        auto const* end = static_cast<char const*>(std::memchr(line, '\0', 1536));
        if (!end) {
            terminated = false;
            return;
        }
        lines.emplace_back(line, static_cast<std::size_t>(end - line));
    }
};

static bool Has(std::string const& line, char const* field)
{
    return line.find(field) != std::string::npos;
}

int main()
{
    const float client[3] = {74109.0f, 3552.0f, -15878.0f};
    Context context;
    context.hostValid = true;
    context.hostAt[0] = 87984.0f;
    context.hostAt[1] = 5145.0f;
    context.hostAt[2] = -10197.0f;
    context.roiCm = 8000.0f;
    context.sim.hooked = true;

    {
        Window window;
        Capture capture;
        CHECK(!window.Due(6000));
        for (uint32_t now = 1000; now <= 5000; now += 1000) {
            window.BeginTick(now);
            window.Candidates(24, 26, 26, 300);
            window.Player(2, true, true, 12, client);
            for (int i = 0; i < 26; ++i) {
                window.Reach(2, 15000.0f, true, 8000.0f, 8000.0f);
                window.Result(2, Drop::Outside);
            }
        }
        CHECK(!window.Due(5999));
        CHECK(window.Due(6000));
        window.Flush(6000, context, [&](char const* line) { capture.Receive(line); });
        CHECK(capture.terminated && capture.lines.size() == 1);
        std::string const& line = capture.lines[0];
        CHECK(line.size() < 1536);
        CHECK(Has(line, "npc-stream: recipient=2 "));
        CHECK(Has(line, "window_ms=5000 ticks=5"));
        CHECK(Has(line, "host=87984,5145,-10197 client=74109,3552,-15878 distance_cm=15077"));
        CHECK(Has(line, "state_age_ms=12 in_world=1"));
        CHECK(Has(line, "gate=open"));
        CHECK(Has(line, "processed_npc=26 interaction_npc=24 candidates=26"));
        CHECK(Has(line, "near_roi=0 processed_near_roi=0 nearest_cm=15000 in_interest=0"));
        CHECK(Has(line, "considered=26.0 sent=0.0 released=0.0"));
        CHECK(Has(line, "top_drop=outside:26.0 drops=outside:26.0"));

        capture.lines.clear();
        for (uint32_t now : {7000u, 9000u}) {
            window.BeginTick(now);
            window.Candidates(1, 2, 3, 4);
            window.Player(2, true, true, 5, client);
            window.Reach(2, 300.0f, true, 8000.0f, 8000.0f);
            window.Result(2, Drop::Outside);
            window.Sent(2);
            window.Result(7, Drop::Away);
            window.Sent(7);
        }
        window.Candidate(Drop::Pending);
        CHECK(!window.Due(10999));
        CHECK(window.Due(11000));
        window.Flush(11000, context, [&](char const* value) { capture.Receive(value); });
        CHECK(capture.terminated && capture.lines.size() == 1);
        CHECK(Has(capture.lines[0], "npc-stream: recipient=2 window_ms=5000 ticks=2"));
        CHECK(Has(capture.lines[0], "processed_npc=2 interaction_npc=1 candidates=3"));
        CHECK(Has(capture.lines[0], "near_roi=1 processed_near_roi=1 nearest_cm=300 in_interest=1"));
        CHECK(Has(capture.lines[0], "considered=1.0 sent=0.4 released=0.0"));
        CHECK(Has(capture.lines[0], "top_drop=outside:0.4 drops=pending:0.2,outside:0.4"));
    }

    {
        Window window;
        Capture capture;
        Context paused = context;
        paused.sim.paused = true;
        paused.sim.resumable = true;
        paused.sim.stops = 7;
        paused.sim.resumes = 6;
        paused.sim.spheres = 3;
        paused.sim.anchors = 4;
        paused.sim.regions = 5;
        paused.sim.regionsFailed = 2;
        paused.sim.regionCells = 9;
        paused.sim.regionStatics = 10;
        paused.sim.regionDynamics = 11;
        window.BeginTick(10);
        window.Player(2, true, true, 0, client);
        window.Flush(5010, paused, [&](char const* line) { capture.Receive(line); });
        CHECK(capture.terminated && capture.lines.size() == 1);
        CHECK(Has(capture.lines[0], "gate=paused resumable=1 stops=7 resumes=6 spheres=3 anchors=4"));
        CHECK(Has(capture.lines[0], "regions=5/2 region_cells=9 region_static=10 region_dynamic=11"));
        CHECK(Has(capture.lines[0], "top_drop=- drops=-"));
    }

    {
        Window window;
        Capture capture;
        for (uint32_t now = 0; now < 5000; now += 1000) {
            window.BeginTick(now);
            window.Candidates(5, 3, 5, 30);
            window.Player(2, true, true, 0, client);
            window.Reach(2, 0.0f, true, 8000.0f, 9000.0f);
            window.Reach(2, 8000.0f, false, 8000.0f, 9000.0f);
            window.Reach(2, 8001.0f, true, 8000.0f, 9000.0f);
            window.Reach(2, 9000.0f, false, 8000.0f, 9000.0f);
            window.Reach(2, 7500.0f, true, 8000.0f, 9000.0f);
            window.Sent(2);
            window.Released(2);
        }
        window.Flush(5000, context, [&](char const* line) { capture.Receive(line); });
        CHECK(capture.terminated && capture.lines.size() == 1);
        CHECK(Has(capture.lines[0], "near_roi=3 processed_near_roi=2 nearest_cm=0 in_interest=5"));
        CHECK(Has(capture.lines[0], "considered=2.0 sent=1.0 released=1.0 top_drop=- drops=-"));

        capture.lines.clear();
        window.BeginTick(6000);
        window.Player(2, true, true, 0, client);
        window.Flush(10000, context, [&](char const* line) { capture.Receive(line); });
        CHECK(capture.terminated && capture.lines.size() == 1);
        CHECK(Has(capture.lines[0], "near_roi=0 processed_near_roi=0 nearest_cm=- in_interest=0"));
        CHECK(Has(capture.lines[0], "considered=0.0 sent=0.0 released=0.0 top_drop=- drops=-"));
    }

    {
        Window window;
        Capture capture;
        window.BeginTick(1000);
        window.Player(2, true, true, 0, client);
        window.Player(3, true, true, 0, client);
        for (int i = 0; i < 6; ++i) window.Candidate(Drop::Range);
        for (int i = 0; i < 10; ++i) window.Candidate(Drop::Pending);
        for (int i = 0; i < 3; ++i) window.Result(2, Drop::Range);
        for (int i = 0; i < 5; ++i) window.Result(2, Drop::Pending);
        for (int i = 0; i < 4; ++i) window.Result(2, Drop::Outside);
        for (int i = 0; i < 16; ++i) window.Result(3, Drop::Outside);
        window.Flush(6000, context, [&](char const* line) { capture.Receive(line); });
        CHECK(capture.terminated && capture.lines.size() == 2);
        CHECK(Has(capture.lines[0], "recipient=2 "));
        CHECK(Has(capture.lines[0], "considered=5.6"));
        CHECK(Has(capture.lines[0], "top_drop=pending:3.0 drops=pending:3.0,range:1.8,outside:0.8"));
        CHECK(Has(capture.lines[1], "recipient=3 "));
        CHECK(Has(capture.lines[1], "top_drop=outside:3.2 drops=pending:2.0,range:1.2,outside:3.2"));

        capture.lines.clear();
        window.BeginTick(7000);
        window.Player(2, true, true, 0, client);
        window.Flush(11000, context, [&](char const* line) { capture.Receive(line); });
        CHECK(capture.terminated && capture.lines.size() == 1);
        CHECK(Has(capture.lines[0], "considered=0.0 sent=0.0 released=0.0 top_drop=- drops=-"));
    }

    {
        Window window;
        Capture capture;
        window.BeginTick(1000);
        window.Player(2, false, false, UINT32_MAX, nullptr);
        window.Reach(2, 1.0f, true, 8000.0f, 8000.0f);
        window.Flush(6000, context, [&](char const* line) { capture.Receive(line); });
        CHECK(capture.terminated && capture.lines.size() == 1);
        CHECK(Has(capture.lines[0], "client=- distance_cm=- state_age_ms=0 in_world=0"));
        CHECK(Has(capture.lines[0], "near_roi=0 processed_near_roi=0 nearest_cm=- in_interest=0"));
    }

    {
        Window window;
        Capture capture;
        window.BeginTick(1000);
        for (uint8_t id : {static_cast<uint8_t>(MaxRecipients), static_cast<uint8_t>(255)}) {
            window.Player(id, true, true, 0, nullptr);
            window.Reach(id, 1.0f, true, 8000.0f, 8000.0f);
            window.Result(id, Drop::Outside);
            window.Sent(id);
            window.Released(id);
        }
        window.Candidate(static_cast<Drop>(255));
        window.Result(7, static_cast<Drop>(255));
        window.Player(7, true, true, 0, client);
        window.Flush(6000, context, [&](char const* line) { capture.Receive(line); });
        CHECK(capture.terminated && capture.lines.size() == 1);
        CHECK(Has(capture.lines[0], "recipient=7 "));
        CHECK(Has(capture.lines[0], "considered=0.0 sent=0.0 released=0.0 top_drop=- drops=-"));
    }

    {
        Window window;
        Capture capture;
        std::string longMode(4096, 'x');
        Context longContext = context;
        longContext.mode = longMode.c_str();
        window.BeginTick(1000);
        window.Player(2, true, true, 0, client);
        window.Flush(6000, longContext, [&](char const* line) { capture.Receive(line); });
        CHECK(capture.terminated && capture.lines.size() == 1);
        CHECK(capture.lines[0].size() == 1535);
        CHECK(Has(capture.lines[0], "npc-stream: recipient=2 "));
    }
    return 0;
}
