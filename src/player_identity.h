// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace g3t::identity {
inline bool SteamId(std::string const& text, uint64_t& output)
{
    if (text.empty()) return false;
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9' || value > ((std::numeric_limits<uint64_t>::max)() - (c - '0')) / 10) return false;
        value = value * 10 + (c - '0');
    }
    if (!value) return false;
    output = value;
    return true;
}
inline bool Admit(uint64_t account, uint64_t host, std::set<uint64_t> const& joined)
{
    return account && account != host && !joined.count(account);
}
inline bool Label(std::string const& name)
{
    return !name.empty() && name.size() <= 191 &&
        name.find_first_of("\r\n") == std::string::npos && name.find('\0') == std::string::npos;
}
inline bool Guid(std::string const& hex, std::string& key)
{
    if (hex.size() != 32 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
    auto digit = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    key.resize(16);
    for (size_t i = 0; i < 16; ++i) key[i] = static_cast<char>(digit(hex[2 * i]) * 16 + digit(hex[2 * i + 1]));
    return true;
}
inline std::string Hex(std::string const& key)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char c : key) { result += digits[c >> 4]; result += digits[c & 15]; }
    return result;
}
struct Owner {
    uint64_t account = 0;
    std::string legacyName;
    bool summoned = false;
};
enum class Row { Ignored, Accepted, Invalid };
enum class Decision { Bound, Unknown, Ambiguous, Explicit };
struct Companions {
    uint64_t host = 0;
    std::map<std::string, Owner> owners;
    std::map<uint64_t, std::string> labels;
    std::set<uint64_t> candidates;

    Row Read(std::string line)
    {
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream input(line);
        std::string tag, token, hex, key, name, extra;
        input >> tag;
        if (tag != "companion" && tag != "companion64" && tag != "summon64" && tag != "identitylabel") return Row::Ignored;
        if (tag == "identitylabel") {
            uint64_t account = 0;
            if (!(input >> token) || token != "1" || !(input >> token) || !SteamId(token, account) ||
                input.get() != ' ' || !std::getline(input, name) || !Label(name) || labels.count(account)) return Row::Invalid;
            labels[account] = name;
            candidates.insert(account);
            return Row::Accepted;
        }
        Owner owner;
        if (tag == "companion64" || tag == "summon64") {
            owner.summoned = tag == "summon64";
            if (!(input >> token) || token != "1" || !(input >> hex >> token) ||
                !SteamId(token, owner.account) || input >> extra) return Row::Invalid;
        } else {
            if (!(input >> hex) || input.get() != ' ' || !std::getline(input, owner.legacyName) ||
                !Label(owner.legacyName)) return Row::Invalid;
        }
        if (!Guid(hex, key) || !owners.emplace(key, owner).second) return Row::Invalid;
        if (owner.account) candidates.insert(owner.account);
        return Row::Accepted;
    }
    std::string Write() const
    {
        std::string text;
        for (auto const& [account, name] : labels)
            text += "identitylabel 1 " + std::to_string(account) + " " + name + "\n";
        for (auto const& [key, owner] : owners)
            text += owner.account ? std::string(owner.summoned ? "summon64 1 " : "companion64 1 ") +
                                      Hex(key) + " " + std::to_string(owner.account) + "\n"
                                  : "companion " + Hex(key) + " " + owner.legacyName + "\n";
        return text;
    }
    Decision Resolve(std::string const& name, uint64_t& account,
                     std::map<std::string, uint64_t> const& overrides) const
    {
        auto chosen = overrides.find(name);
        if (chosen != overrides.end()) {
            if (!chosen->second || chosen->second == host || !candidates.count(chosen->second)) return Decision::Ambiguous;
            account = chosen->second;
            return Decision::Explicit;
        }
        uint64_t match = 0;
        bool unknown = false;
        for (uint64_t candidate : candidates) {
            if (candidate == host) continue; // Legacy companion rows were only written for remote players.
            auto label = labels.find(candidate);
            if (label == labels.end()) { unknown = true; continue; }
            if (label->second == name) {
                if (match) return Decision::Ambiguous;
                match = candidate;
            }
        }
        if (!match || unknown) return Decision::Unknown;
        account = match;
        return Decision::Bound;
    }
    Decision Migrate(std::string const& key, std::map<std::string, uint64_t> const& overrides)
    {
        auto found = owners.find(key);
        if (found == owners.end() || found->second.account) return Decision::Unknown;
        uint64_t account = 0;
        const Decision decision = Resolve(found->second.legacyName, account, overrides);
        if (decision == Decision::Bound || decision == Decision::Explicit) found->second = {account, {}};
        return decision;
    }
};
}
