// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace g3t {
// Opens <game dir>\<name> next to the game's scripts folder; lines are appended with a timestamp.
void OpenLog(const char* name);
void Log(const char* text);
void Logf(const char* format, ...);

// Monotonic per-process sequence numbers and recent-event ring retention for structured gameplay telemetry.
uint64_t NextTelemetrySeq();
uint64_t LastTelemetrySeq();
void LogTelemetryLine(uint64_t seq, const char* domainEvent, const char* fields);
size_t CopyRecentTelemetry(char* out, size_t size, size_t maxEntries = 32);

// Formats into a fixed buffer and cuts off what does not fit. sprintf_s ends the game instead (0xC0000409, no log line:
// the trader line of 22.10., QA findnpc 25./26.10.; review 26.10.: TALKSYNC, HITNPC and others close to their size).
// Returns the characters written, so `n += Format(line + n, sizeof(line) - n, ...)` stays inside the buffer.
int FormatV(char* out, size_t size, const char* format, va_list args);
int Format(char* out, size_t size, const char* format, ...);
template <size_t N>
int Format(char (&out)[N], const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int length = FormatV(out, N, format, args);
    va_end(args);
    return length;
}

// Appends a sanitized `key=value` token into a fixed char buffer without heap allocations.
// Empty/null strings emit `-`; strings containing whitespace, quotes, `=`, or control bytes are quoted and scrubbed.
class KvWriter {
public:
    KvWriter(char* buf, size_t cap) noexcept : buf_(buf), cap_(cap)
    {
        if (buf_ && cap_) buf_[0] = '\0';
    }
    template <size_t N>
    explicit KvWriter(char (&buf)[N]) noexcept : KvWriter(buf, N) {}

    char const* c_str() const noexcept { return buf_ ? buf_ : ""; }
    size_t size() const noexcept { return len_; }

    KvWriter& Str(char const* key, char const* val) noexcept
    {
        BeginKey(key);
        AppendSanitized(val);
        return *this;
    }
    KvWriter& Int(char const* key, int64_t val) noexcept
    {
        BeginKey(key);
        len_ += static_cast<size_t>(Format(buf_ + len_, Remaining(), "%lld", static_cast<long long>(val)));
        return *this;
    }
    KvWriter& Uint(char const* key, uint64_t val) noexcept
    {
        BeginKey(key);
        len_ += static_cast<size_t>(Format(buf_ + len_, Remaining(), "%llu", static_cast<unsigned long long>(val)));
        return *this;
    }
    KvWriter& Hex(char const* key, uint64_t val) noexcept
    {
        BeginKey(key);
        len_ += static_cast<size_t>(Format(buf_ + len_, Remaining(), "0x%llx", static_cast<unsigned long long>(val)));
        return *this;
    }
    KvWriter& Bool(char const* key, bool val) noexcept { return Uint(key, val ? 1u : 0u); }
    KvWriter& Float1(char const* key, double val) noexcept
    {
        BeginKey(key);
        len_ += static_cast<size_t>(Format(buf_ + len_, Remaining(), "%.1f", val));
        return *this;
    }
    KvWriter& Pos(char const* key, float x, float y, float z) noexcept
    {
        BeginKey(key);
        len_ += static_cast<size_t>(Format(buf_ + len_, Remaining(), "%.1f,%.1f,%.1f", x, y, z));
        return *this;
    }
    KvWriter& TransitionInt(char const* key, int64_t before, int64_t after) noexcept
    {
        BeginKey(key);
        len_ += static_cast<size_t>(
            Format(buf_ + len_, Remaining(), "%lld->%lld", static_cast<long long>(before), static_cast<long long>(after)));
        return *this;
    }
    KvWriter& TransitionStr(char const* key, char const* before, char const* after) noexcept
    {
        BeginKey(key);
        AppendSanitized(before);
        PutRaw("->");
        AppendSanitized(after);
        return *this;
    }

private:
    size_t Remaining() const noexcept { return (buf_ && len_ < cap_) ? (cap_ - len_) : 0; }
    void PutChar(char ch) noexcept
    {
        if (buf_ && len_ + 1 < cap_) {
            buf_[len_++] = ch;
            buf_[len_] = '\0';
        }
    }
    void PutRaw(char const* text) noexcept
    {
        if (!text) return;
        while (*text && len_ + 1 < cap_) PutChar(*text++);
    }
    void BeginKey(char const* key) noexcept
    {
        if (!key || !*key || !buf_ || cap_ < 2) return;
        if (len_ > 0) PutChar(' ');
        PutRaw(key);
        PutChar('=');
    }
    static bool NeedsQuotes(char const* text) noexcept
    {
        for (unsigned char const* p = reinterpret_cast<unsigned char const*>(text); *p; ++p) {
            if (*p <= 0x20 || *p == '"' || *p == '=' || *p == '[' || *p == ']') return true;
        }
        return false;
    }
    void AppendSanitized(char const* val) noexcept
    {
        if (!val || !*val) {
            PutChar('-');
            return;
        }
        if (!NeedsQuotes(val)) {
            PutRaw(val);
            return;
        }
        PutChar('"');
        for (unsigned char const* p = reinterpret_cast<unsigned char const*>(val); *p && len_ + 2 < cap_; ++p) {
            if (*p == '"') PutChar('\'');
            else if (*p < 0x20) PutChar(' ');
            else PutChar(static_cast<char>(*p));
        }
        PutChar('"');
    }

    char* buf_ = nullptr;
    size_t cap_ = 0;
    size_t len_ = 0;
};

using FormatLogFn = void (*)(char const*);
void FormatProbe(char const* argument, FormatLogFn log);  // QA: one line of <n> characters into a 64-byte buffer
void FormatStrict(FormatLogFn log);                       // QA negative control (formatstrict): sprintf_s as before 26.10.
}
