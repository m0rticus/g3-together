// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace g3t::pose {
constexpr int MaxBones = 256;
constexpr int HistorySize = 128;
constexpr float BasisScale = 4096.0f;
constexpr float PositionScale = 4.0f;
constexpr uint8_t Facial = 1;

#pragma pack(push, 1)
struct Bone {
    uint32_t name = 0;
    int16_t values[12] = {};
};
#pragma pack(pop)
static_assert(sizeof(Bone) == 28, "Bone poses use an affine matrix without discarding shear");

struct Transform {
    uint32_t name = 0;
    float values[12] = {};
};

inline bool Encode(Transform const& source, Bone& target)
{
    if (!source.name) return false;
    Bone result;
    result.name = source.name;
    for (int i = 0; i < 12; ++i) {
        const float value = std::round(source.values[i] * (i < 9 ? BasisScale : PositionScale));
        if (!std::isfinite(value) || value < -32768.0f || value > 32767.0f) return false;
        result.values[i] = static_cast<int16_t>(value);
    }
    target = result;
    return true;
}

inline Transform Decode(Bone const& bone)
{
    Transform result;
    result.name = bone.name;
    for (int i = 0; i < 12; ++i) result.values[i] = bone.values[i] / (i < 9 ? BasisScale : PositionScale);
    return result;
}

class History {
    struct Frame {
        uint32_t incarnation = 0, sent = 0;
        uint8_t flags = 0;
        std::vector<Bone> bones;
    };
    std::vector<Frame> frames;

public:
    bool Push(uint32_t incarnation, uint32_t sent, Bone const* bones, int count, uint8_t flags = 0)
    {
        if (!incarnation || !bones || count <= 0 || count > MaxBones || (flags & ~Facial)) return false;
        if (!frames.empty()) {
            const auto& last = frames.back();
            if (static_cast<int32_t>(incarnation - last.incarnation) < 0 ||
                (incarnation == last.incarnation && static_cast<int32_t>(sent - last.sent) <= 0)) return false;
            if (incarnation != last.incarnation) frames.clear();
        }
        if (frames.size() == HistorySize) frames.erase(frames.begin());
        frames.push_back({incarnation, sent, flags, std::vector<Bone>(bones, bones + count)});
        return true;
    }

    bool Sample(uint32_t incarnation, uint32_t sent, std::vector<Transform>& result, uint8_t* flags = nullptr) const
    {
        if (frames.empty() || frames.front().incarnation != incarnation ||
            static_cast<int32_t>(sent - frames.front().sent) < 0 ||
            static_cast<int32_t>(sent - frames.back().sent) > 0) return false;
        const Frame* a = &frames.back();
        const Frame* b = a;
        for (size_t i = 1; i < frames.size(); ++i) {
            if (static_cast<int32_t>(sent - frames[i].sent) > 0) continue;
            a = &frames[i - 1];
            b = &frames[i];
            break;
        }
        const uint32_t span = b->sent - a->sent;
        const float fraction = span ? static_cast<float>(sent - a->sent) / span : 0.0f;
        if (fraction >= 1.0f) a = b;
        if (flags) *flags = fraction < 1.0f ? a->flags : b->flags;
        bool compatible = a->flags == b->flags && a->bones.size() == b->bones.size();
        for (size_t i = 0; compatible && i < a->bones.size(); ++i)
            compatible = a->bones[i].name == b->bones[i].name;
        result.resize(a->bones.size());
        for (size_t i = 0; i < result.size(); ++i) {
            result[i] = Decode(a->bones[i]);
            if (!compatible) continue;
            const auto end = Decode(b->bones[i]);
            for (int k = 0; k < 12; ++k)
                result[i].values[k] += (end.values[k] - result[i].values[k]) * fraction;
        }
        return true;
    }
};
}
