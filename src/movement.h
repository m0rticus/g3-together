// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
class eCEntity;

namespace g3t::movement {
long long EntryTime();
void Begin(long long entered);
void End();
void OnWorldChange();
void Touch(eCEntity* entity, char const* kind, char const* value, void* caller = nullptr);
void KeyDispatch(int key, bool blocked);

class Stage {
public:
    explicit Stage(char const* name);
    ~Stage();
    Stage(Stage const&) = delete;
    Stage& operator=(Stage const&) = delete;
private:
    char const* before;
    long long started;
    unsigned beforeCalls = 0;
};
}
