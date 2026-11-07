// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/worldentry.h"

#include <cassert>
#include <thread>

int main()
{
    using namespace g3t::worldentry;
    constexpr char nativeLoad[] = "native load";
    constexpr char cleanup[] = "world cleanup";
    constexpr char engine[] = "engine first frame";
    Progress progress;
    assert(!progress.Read().name);
    const Step empty = progress.Set(nativeLoad, 100);
    assert(!empty.name && progress.Read().since == 100);
    progress.Set(nativeLoad, 4100);
    assert(progress.Read().since == 100); // Repeated gates retain their original wait age.
    const Step outer = progress.Set(cleanup, 4300);
    assert(outer.name == nativeLoad && outer.since == 100);
    progress.Restore(outer);
    assert(progress.Read().name == nativeLoad && progress.Read().since == 100);
    progress.Set(engine, 5000);
    assert(progress.Read().since == 5000);
    progress.Set(nullptr, 6000);
    assert(!progress.Read().name);

    Reports reports;
    const Step gate{engine, 1000};
    assert(!reports.Due(5999, {}, gate));
    assert(reports.Due(6000, {}, gate));
    assert(!reports.Due(10999, {}, gate));
    assert(reports.Due(11000, {cleanup, 10900}, gate));
    assert(!reports.Due(12000, {nativeLoad, 0}, gate)); // A phase change cannot flood the worker log.
    assert(!reports.Due(16000, {}, {}));
    assert(reports.Due(16000, {nativeLoad, 10000}, {}));

    Reports wrapped;
    const uint32_t start = UINT32_MAX - 2000;
    assert(!wrapped.Due(start + 4999, {nativeLoad, start}, {}));
    assert(wrapped.Due(start + 5000, {nativeLoad, start}, {}));
    assert(!wrapped.Due(start + 9999, {nativeLoad, start}, {}));
    assert(wrapped.Due(start + 10000, {nativeLoad, start}, {}));

    Progress shared;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (uint32_t i = 0; i < 100000; ++i) {
            shared.Restore({nativeLoad, 1});
            shared.Restore({cleanup, 2});
        }
        done.store(true);
    });
    while (!done.load()) {
        Step step;
        if (!shared.Read(step)) continue;
        assert(!step.name || (step.name == nativeLoad && step.since == 1) ||
               (step.name == cleanup && step.since == 2));
    }
    writer.join();
}
