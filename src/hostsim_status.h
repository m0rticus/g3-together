// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free snapshot of the host's simulation around the other players (host_sim = spheres), for diagnostics only.
#pragma once

namespace g3t::world {
struct HostSimStatus {
    bool hooked = false, paused = false, resumable = false, spheresActive = false;
    unsigned spheres = 0, anchors = 0, regions = 0, regionsFailed = 0;
    unsigned regionCells = 0, regionStatics = 0, regionDynamics = 0;  // entities the host loaded for remote players
    unsigned processed = 0, stops = 0, resumes = 0;                   // processing list size, native Stops, gate resumes
    float nativeRoiCm = 0.0f;
};
}
