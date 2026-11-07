// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace g3t::worldentry {
constexpr uint32_t ReportIntervalMs = 5000;

struct Step {
    char const* name = nullptr;
    uint32_t since = 0;
};

// One engine-thread writer; the transport worker only reads static-lifetime names.
class Progress {
    std::atomic<uint32_t> revision{0}, since{0};
    std::atomic<char const*> name{nullptr};

  public:
    bool Read(Step& step) const
    {
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const uint32_t before = revision.load();
            if (before & 1) continue;
            const Step result{name.load(), since.load()};
            if (revision.load() == before) { step = result; return true; }
        }
        return false;
    }

    Step Read() const
    {
        Step step;
        Read(step);
        return step;
    }

    void Restore(Step step)
    {
        revision.fetch_add(1);
        name.store(step.name);
        since.store(step.since);
        revision.fetch_add(1);
    }

    Step Set(char const* step, uint32_t now)
    {
        const Step previous = Read();
        if (previous.name != step) Restore({step, now});
        return previous;
    }
};

class Reports {
    uint32_t last = 0;
    bool reported = false;

  public:
    bool Due(uint32_t now, Step operation, Step gate)
    {
        if ((!operation.name || now - operation.since < ReportIntervalMs) &&
            (!gate.name || now - gate.since < ReportIntervalMs)) return false;
        if (reported && now - last < ReportIntervalMs) return false;
        last = now;
        reported = true;
        return true;
    }
};
}
