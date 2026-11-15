// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/shutdown_policy.h"

#include <array>
#include <cassert>

using namespace g3t::shutdown;

int main()
{
    std::array<uint8_t, 15> device = {0x8b, 0x46, 0x08, 0x8b, 0x08, 0x8b, 0x51, 0x08,
                                     0x50, 0xff, 0xd2, 0x85, 0xc0, 0x77, 0xf1};
    assert(DeviceReleaseSite(device.data(), device.size()));
    assert(!DeviceReleaseSite(device.data(), device.size() - 1));
    device[13] = 0x74;
    assert(!DeviceReleaseSite(device.data(), device.size()));
    std::array<uint8_t, 18> driver = {0x68, 0xe8, 0x03, 0x00, 0x00, 0x53, 0x55, 0xff, 0x15,
                                     0x54, 0xd3, 0xd9, 0x10, 0x39, 0x7e, 0x48, 0x72, 0xee};
    assert(DriverWaitSite(driver.data(), driver.size()));
    driver[9] = 0x12; // IAT slot relocates; the runtime additionally verifies its API target.
    assert(DriverWaitSite(driver.data(), driver.size()));
    driver[17] = 0xed;
    assert(!DriverWaitSite(driver.data(), driver.size()));

    constexpr uint32_t condition = 0x762fbf20, amd = 0x7194cae1, release = 0x0131b132;
    std::array<uint32_t, 10> stack = {0, condition, 0, amd, 0x069ede48, 0x069ede4c, 1000, 0, release, 0};
    assert(WaitingObject(stack.data(), stack.size(), condition, amd, release) == 0x069ede40);
    assert(!WaitingObject(stack.data(), 6, condition, amd, release));
    assert(!WaitingObject(stack.data(), stack.size(), 0, amd, release));
    stack[6] = UINT32_MAX;
    assert(!WaitingObject(stack.data(), stack.size(), condition, amd, release));
    stack[6] = 1000;
    stack[5] += 4;
    assert(!WaitingObject(stack.data(), stack.size(), condition, amd, release));
    stack[5] -= 4;
    stack[8] = 0;
    stack[0] = release; // A stale device address before the nested wait is insufficient.
    assert(!WaitingObject(stack.data(), stack.size(), condition, amd, release));
    stack[0] = 0;
    stack[8] = release;
    stack[4] = UINT32_MAX;
    assert(!WaitingObject(stack.data(), stack.size(), condition, amd, release));

    GraphicsWait wait{0x069ede40, 2, 5, 1};
    assert(wait.Pending());
    assert(!(GraphicsWait{0x069ede40, 0, 1, 2}.Pending()));
    ExitPolicy normal;
    assert(!normal.Observe(0, true, 0, true, wait));
    assert(!normal.Observe(GraphicsStallMs, true, 0, true, wait));
    assert(!normal.Observe(GraphicsGraceMs - 1, true, 0, true, wait));
    assert(normal.Observe(GraphicsGraceMs, true, 0, true, wait));

    ExitPolicy confirmation;
    assert(!confirmation.Observe(2000, true, 0, true, wait));
    assert(!confirmation.Observe(2999, true, 0, true, wait));
    assert(confirmation.Observe(3000, true, 0, true, wait));

    for (int field = 0; field != 4; ++field) {
        ExitPolicy progress;
        assert(!progress.Observe(2000, true, 0, true, wait));
        auto changed = wait;
        if (field == 0) changed.object += 0x100;
        if (field == 1) ++changed.completed;
        if (field == 2) ++changed.produced;
        if (field == 3) --changed.retired;
        assert(!progress.Observe(3000, true, 0, true, changed));
        assert(!progress.Observe(3999, true, 0, true, changed));
        assert(progress.Observe(4000, true, 0, true, changed));
    }
    for (int gate = 0; gate != 4; ++gate) {
        ExitPolicy reset;
        assert(!reset.Observe(2000, true, 0, true, wait));
        auto cleared = wait;
        if (gate == 2) cleared.object = 0;
        if (gate == 3) cleared.completed = 4;
        assert(!reset.Observe(3000, gate != 0, 0, gate != 1, cleared));
        assert(!reset.Observe(3100, true, 0, true, wait));
        assert(!reset.Observe(4099, true, 0, true, wait));
        assert(reset.Observe(4100, true, 0, true, wait));
    }
    ExitPolicy cleanup;
    assert(!cleanup.Observe(20000, false, 0, true, wait));
    assert(!cleanup.Observe(20000, true, 20000, true, wait));
    assert(!cleanup.Observe(21499, true, 20000, true, wait));
    assert(cleanup.Observe(21500, true, 20000, true, wait));

    ExitPolicy wrap;
    constexpr uint32_t start = UINT32_MAX - 1000;
    assert(!wrap.Observe(start, true, start, true, wait));
    assert(!wrap.Observe(start + GraphicsGraceMs - 1, true, start, true, wait));
    assert(wrap.Observe(start + GraphicsGraceMs, true, start, true, wait));
}
