// SPDX-License-Identifier: GPL-3.0-or-later
#include "guid.h"
#include "guid_text.h"

#include <cstdio>
#include <cstring>

namespace g3t::guid {
bool Parse(char const* text, ::_GUID& out)
{
    uint8_t bytes[16];
    if (!ParseBytes(text, bytes)) return false;
    std::memcpy(&out, bytes, 16);
    return true;
}

bool Parse(char const* text, uint8_t out[16])
{
    return ParseBytes(text, out);
}

bool ParseExact(char const* text, ::_GUID& out)
{
    uint8_t bytes[16];
    if (!ParseBytesExact(text, bytes)) return false;
    std::memcpy(&out, bytes, 16);
    return true;
}

bool ParseExact(char const* text, uint8_t out[16])
{
    return ParseBytesExact(text, out);
}

void CopyEntity(::eCEntity* entity, uint8_t out[16])
{
    const eCEntityProxy proxy(entity);
    std::memcpy(out, &proxy.GetEntityID().GetCore(), 16);
}

void Format(::_GUID const& guid, char* out, size_t size)
{
    uint8_t bytes[16];
    std::memcpy(bytes, &guid, 16);
    FormatBytes(bytes, out, size);
}

void Format(uint8_t const guid[16], char* out, size_t size)
{
    FormatBytes(guid, out, size);
}

void FormatEntity(::eCEntity* entity, char* out, size_t size)
{
    uint8_t bytes[16];
    CopyEntity(entity, bytes);
    Format(bytes, out, size);
}

::Entity Resolve(_GUID const& guid) { return ::Entity(Property(guid)); }

::Entity Resolve(uint8_t const guid[16])
{
    _GUID raw{};
    std::memcpy(&raw, guid, 16);
    return Resolve(raw);
}

::bCPropertyID Property(_GUID const& guid) { return ::bCPropertyID(::bCGuid(guid)); }
}
