// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Game.h>
#include <Script.h>

#include <cstddef>
#include <cstdint>

namespace g3t::guid {
bool Parse(char const* text, ::_GUID& out);
bool Parse(char const* text, uint8_t out[16]);
bool ParseExact(char const* text, ::_GUID& out);
bool ParseExact(char const* text, uint8_t out[16]);

void CopyEntity(::eCEntity* entity, uint8_t out[16]);
void Format(::_GUID const& guid, char* out, size_t size);
void Format(uint8_t const guid[16], char* out, size_t size);
void FormatEntity(::eCEntity* entity, char* out, size_t size);

::Entity Resolve(::_GUID const& guid);
::Entity Resolve(uint8_t const guid[16]);
::bCPropertyID Property(::_GUID const& guid);
}
