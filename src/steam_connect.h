// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy for the invite-driven Steam connection (steam.cpp drives the Steam API, lobby.cpp shows the state).
//
// Nothing connects by itself. The host publishes rich presence `connect = +g3t_connect <host SteamID64> <session_id>` and
// stays silent. A client connects only after its player accepts something in Steam: GameRichPresenceJoinRequested_t (Join
// Game / invite while the game runs), the same connect string on the command line (Steam starts the game with it),
// GameLobbyJoinRequested_t / `+connect_lobby <id>` (a lobby invite), or an explicit manual INI override. The accepted
// connect string makes the client send a join request over ISteamNetworkingMessages (control channel, plain ASCII frame,
// no relay envelope because no lobby exists yet). The host checks friendship, session id and protocol, creates its
// friends-only lobby on demand and answers with the lobby id; the client joins it and the existing relay hello/welcome
// handshake follows. The ENet protocol and the relay envelope are untouched.
//
// Both flows are pure state machines: every event returns the effects the caller must perform. Times are milliseconds
// from any monotonic clock (wrap-safe).
#pragma once

#include "relaywire.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace g3t::steam::connect {

constexpr uint32_t RequestWaitMs = 20000;   // client: no answer to the join request
constexpr uint32_t RequestResendMs = 4000;  // client: the request is sent again (idempotent on the host) until answered
constexpr uint32_t LobbyWaitMs = 15000;     // client: lobby metadata / JoinLobby answer
constexpr uint32_t LobbyMetadataWaitMs = 8000;  // client: Steam has not delivered both lobby data keys
constexpr uint32_t ImmediateLinkEndMs = 5000;
inline bool LinkEndedImmediately(bool hadLink, uint32_t elapsedMs)
{
    return hadLink && elapsedMs < ImmediateLinkEndMs;
}
constexpr uint32_t HelloWaitMs = 20000;     // client: lobby entered, the host has not welcomed us
constexpr uint32_t CreateWaitMs = 15000;    // host: CreateLobby has not answered
constexpr uint32_t OfferWaitMs = 60000;     // host: the invited partner has not said hello
constexpr unsigned MaxPartners = 1;         // one client at a time in invite mode (the lobby holds host + partner)

enum class Mode : uint8_t { Invite, Auto };  // connection = invite (default) | auto (legacy friends-list discovery)

namespace detail {
inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
inline bool EqualsNoCase(char const* a, char const* b)
{
    for (; *a && *b; ++a, ++b)
        if (Lower(*a) != Lower(*b)) return false;
    return *a == *b;
}
inline bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline bool Reached(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }
// Unsigned decimal, no sign, no zero, no overflow.
inline bool ParseU64(char const* text, uint64_t* out)
{
    if (!text || !*text) return false;
    uint64_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        const unsigned digit = static_cast<unsigned>(*text - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (out) *out = value;
    return value != 0;
}
// One whitespace-separated token of a command line or connect string, without its quotes. False at the end.
inline bool NextToken(char const*& p, char* out, size_t size)
{
    while (*p && Space(*p)) ++p;
    if (!*p) return false;
    size_t n = 0;
    bool cut = false;
    for (; *p && !Space(*p); ++p) {
        if (n + 1 < size) out[n++] = *p;
        else cut = true;
    }
    out[n] = '\0';
    if (cut) {  // a token that does not fit is never a usable key or number: do not match its prefix
        out[0] = '\0';
        return true;
    }
    size_t start = 0;
    while (out[start] == '"') ++start;
    while (n > start && out[n - 1] == '"') --n;
    std::memmove(out, out + start, n - start);
    out[n - start] = '\0';
    return true;
}
}

inline bool ParseMode(char const* text, Mode* mode)
{
    if (detail::EqualsNoCase(text ? text : "", "invite")) { if (mode) *mode = Mode::Invite; return true; }
    if (detail::EqualsNoCase(text ? text : "", "auto")) { if (mode) *mode = Mode::Auto; return true; }
    return false;
}

// ---- the connect string (rich presence value, callback 337 payload, command line) ----

constexpr char HostKey[] = "g3t_connect";      // +g3t_connect <host SteamID64> <session_id>
constexpr char LobbyKey[] = "connect_lobby";   // +connect_lobby <lobby id>: Steam's own lobby invite / launch argument

struct Target
{
    enum class Kind : uint8_t { None, Host, Lobby };
    Kind kind = Kind::None;
    uint64_t id = 0;                              // Host: the host's SteamID64; Lobby: the lobby id
    char session[RelayMaxSessionSize + 1] = {};   // Host: the session_id the host published
    bool malformed = false;                       // one of our keys with unusable values
};

// The first G3 Together key anywhere in `text` (a whole command line or just the connect value). Other text is ignored.
inline Target ParseConnect(char const* text)
{
    Target target;
    if (!text) return target;
    char token[96], first[96], second[96];
    char const* p = text;
    while (detail::NextToken(p, token, sizeof(token))) {
        char const* key = token;
        while (*key == '+' || *key == '-' || *key == '/') ++key;
        const bool host = detail::EqualsNoCase(key, HostKey), lobby = detail::EqualsNoCase(key, LobbyKey);
        if (!host && !lobby) continue;
        uint64_t id = 0;
        if (!detail::NextToken(p, first, sizeof(first)) || !detail::ParseU64(first, &id)) { target.malformed = true; return target; }
        if (lobby) {
            target.kind = Target::Kind::Lobby;
            target.id = id;
            return target;
        }
        if (!detail::NextToken(p, second, sizeof(second)) || !ValidRelaySession(second)) { target.malformed = true; return target; }
        target.kind = Target::Kind::Host;
        target.id = id;
        std::strcpy(target.session, second);
        return target;
    }
    return target;
}

// "+g3t_connect <host id> <session>": what the host publishes. Returns the length, 0 if it does not fit or the session is invalid.
inline size_t FormatConnect(char* out, size_t size, uint64_t hostId, char const* session)
{
    if (!out || !size || !hostId || !ValidRelaySession(session)) return 0;
    const int n = std::snprintf(out, size, "+%s %llu %s", HostKey, static_cast<unsigned long long>(hostId), session);
    return n > 0 && static_cast<size_t>(n) < size ? static_cast<size_t>(n) : 0;
}

// ---- why a connection did not happen ----

enum class Why : uint8_t {
    None,
    NotInvite,      // the accepted text is not a G3 Together connect string
    SelfInvite,     // the invite names this very account
    WrongSession,   // session_id differs (arg: the other side's id is in the text field)
    WrongProtocol,  // G3 Together version differs (arg: the host's protocol)
    Busy,           // the host already has its partner
    NotFriends,     // the host does not have this account on its Steam friends list
    HostError,      // the host could not open the lobby (arg: Steam EResult, 0 = no answer)
    NoAnswer,       // nothing came back in time
    Unreachable,    // Steam could not reach the host (offline / game not running)
    LobbyGone,      // the lobby is closed or full
    NotG3T,         // the lobby is not a G3 Together game of this session/version
    MetadataTimeout, // Steam did not deliver the lobby data in time
};
inline char const* WhyName(Why why)
{
    switch (why) {
    case Why::None: return "none";
    case Why::NotInvite: return "not a G3 Together invite";
    case Why::SelfInvite: return "invite names this account";
    case Why::WrongSession: return "different session_id";
    case Why::WrongProtocol: return "different protocol";
    case Why::Busy: return "host already has a partner";
    case Why::NotFriends: return "not on the host's friends list";
    case Why::HostError: return "host could not open the lobby";
    case Why::NoAnswer: return "no answer";
    case Why::Unreachable: return "host unreachable over Steam";
    case Why::LobbyGone: return "lobby closed or full";
    case Why::NotG3T: return "lobby is not this G3 Together session";
    case Why::MetadataTimeout: return "Steam did not deliver lobby data in time";
    }
    return "?";
}

// ---- negotiation frames: ASCII on the control channel, outside the relay envelope ----
//   G3T1J <session> <protocol>   client -> host   "I accepted your invite"
//   G3T1L <lobby> <session>      host -> client   "the lobby is open"
//   G3T1N <code> <arg>           host -> client   S: arg = host session, P: arg = host protocol, B/F: arg = -, E: arg = EResult

constexpr char FramePrefix[] = "G3T1";
constexpr size_t FramePrefixSize = 4;
constexpr size_t FrameMax = 128;

struct Frame
{
    char type = 0;                                // 'J', 'L' or 'N'
    char session[RelayMaxSessionSize + 1] = {};   // J, L; N with code S
    unsigned protocol = 0;                        // J
    uint64_t lobby = 0;                           // L
    char code = 0;                                // N
    uint64_t number = 0;                          // N with code P or E
};

inline char DeclineCode(Why why)
{
    switch (why) {
    case Why::WrongSession: return 'S';
    case Why::WrongProtocol: return 'P';
    case Why::Busy: return 'B';
    case Why::NotFriends: return 'F';
    case Why::HostError: return 'E';
    default: return 0;
    }
}
inline Why DeclineWhy(char code)
{
    switch (code) {
    case 'S': return Why::WrongSession;
    case 'P': return Why::WrongProtocol;
    case 'B': return Why::Busy;
    case 'F': return Why::NotFriends;
    case 'E': return Why::HostError;
    default: return Why::None;
    }
}

inline size_t EncodeJoin(char* out, size_t size, char const* session, unsigned protocol)
{
    if (!out || !ValidRelaySession(session)) return 0;
    const int n = std::snprintf(out, size, "%s%c %s %u", FramePrefix, 'J', session, protocol);
    return n > 0 && static_cast<size_t>(n) < size && static_cast<size_t>(n) <= FrameMax ? static_cast<size_t>(n) : 0;
}
inline size_t EncodeOffer(char* out, size_t size, uint64_t lobby, char const* session)
{
    if (!out || !lobby || !ValidRelaySession(session)) return 0;
    const int n = std::snprintf(out, size, "%s%c %llu %s", FramePrefix, 'L', static_cast<unsigned long long>(lobby), session);
    return n > 0 && static_cast<size_t>(n) < size && static_cast<size_t>(n) <= FrameMax ? static_cast<size_t>(n) : 0;
}
// Why::WrongSession needs `session` (the host's), WrongProtocol/HostError `number`; Busy/NotFriends carry neither.
inline size_t EncodeDecline(char* out, size_t size, Why why, char const* session, uint64_t number)
{
    const char code = DeclineCode(why);
    if (!out || !code) return 0;
    int n = 0;
    if (code == 'S') {
        if (!ValidRelaySession(session)) return 0;
        n = std::snprintf(out, size, "%s%c %c %s", FramePrefix, 'N', code, session);
    } else if (code == 'P' || code == 'E') {
        n = std::snprintf(out, size, "%s%c %c %llu", FramePrefix, 'N', code, static_cast<unsigned long long>(number));
    } else {
        n = std::snprintf(out, size, "%s%c %c -", FramePrefix, 'N', code);
    }
    return n > 0 && static_cast<size_t>(n) < size && static_cast<size_t>(n) <= FrameMax ? static_cast<size_t>(n) : 0;
}

// A frame is printable ASCII, so its 9th byte is below 0x80; a relay envelope has the marker 0x80|length there (relaywire.h).
inline bool IsFrame(void const* data, size_t size)
{
    return data && size > FramePrefixSize + 4 && size <= FrameMax && std::memcmp(data, FramePrefix, FramePrefixSize) == 0 &&
           static_cast<unsigned char const*>(data)[RelayLobbySize] < 0x80;
}

// Strict: exact fields, single spaces, printable ASCII, nothing after the last field.
inline bool DecodeFrame(void const* data, size_t size, Frame* out)
{
    if (!out || !IsFrame(data, size)) return false;
    char text[FrameMax + 1];
    std::memcpy(text, data, size);
    text[size] = '\0';
    for (size_t i = 0; i < size; ++i)
        if (text[i] < ' ' || text[i] > '~') return false;
    Frame frame;
    frame.type = text[FramePrefixSize];
    if (text[FramePrefixSize + 1] != ' ') return false;
    char* fields[3] = {};
    size_t count = 0;
    for (char* cursor = text + FramePrefixSize + 2; cursor;) {
        if (count == 3 || !*cursor || *cursor == ' ') return false;
        fields[count++] = cursor;
        char* space = std::strchr(cursor, ' ');
        if (space) *space = '\0';
        cursor = space ? space + 1 : nullptr;
    }
    uint64_t value = 0;
    if (frame.type == 'J') {
        if (count != 2 || !ValidRelaySession(fields[0]) || !detail::ParseU64(fields[1], &value) || value > 0xffffu) return false;
        std::strcpy(frame.session, fields[0]);
        frame.protocol = static_cast<unsigned>(value);
    } else if (frame.type == 'L') {
        if (count != 2 || !detail::ParseU64(fields[0], &frame.lobby) || !ValidRelaySession(fields[1])) return false;
        std::strcpy(frame.session, fields[1]);
    } else if (frame.type == 'N') {
        if (count != 2 || fields[0][1] != '\0' || DeclineWhy(fields[0][0]) == Why::None) return false;
        frame.code = fields[0][0];
        if (frame.code == 'S') {
            if (!ValidRelaySession(fields[1])) return false;
            std::strcpy(frame.session, fields[1]);
        } else if (frame.code == 'P' || frame.code == 'E') {
            if (fields[1][0] == '0' && !fields[1][1]) value = 0;
            else if (!detail::ParseU64(fields[1], &value)) return false;
            frame.number = value;
        } else if (std::strcmp(fields[1], "-") != 0) {
            return false;
        }
    } else {
        return false;
    }
    if (out) *out = frame;
    return true;
}

// ---- the client ----

enum class LobbyMetadata : uint8_t { Pending, Match, Different };

inline LobbyMetadata CheckLobbyMetadata(char const* tag, char const* version, char const* session, char const* ours)
{
    if (!tag || !*tag || !session || !*session) return LobbyMetadata::Pending;
    return std::strcmp(tag, version) == 0 && std::strcmp(session, ours) == 0
        ? LobbyMetadata::Match : LobbyMetadata::Different;
}

class ClientFlow
{
  public:
    enum class State : uint8_t {
        Starting,    // Steam is not ready
        Waiting,     // ready, nothing accepted: waiting for an invite (nothing is searched, nothing is sent)
        Requesting,  // accepted a host's connect string; the join request is out
        Joining,     // lobby known (offered by the host, or named by a lobby invite): joining it, hello pending
        Linked,      // the host welcomed us: the relay is up
        Declined,    // the attempt ended: why() says why; nothing happens until the next accept
        Lost,        // the link ended after it was up; nothing happens until the next accept
    };
    enum class Source : uint8_t { RichJoin, CommandLine, LobbyJoin, Manual };
    enum Fx : unsigned {
        None = 0,
        SendRequest = 1,   // send the join request to host()
        InspectLobby = 2,  // read lobby() metadata, answer with LobbyChecked
        JoinLobby = 4,     // JoinLobby(lobby())
        Teardown = 8,      // leave the lobby, close the session with the abandoned host, forget half-open state
        Ignored = 16,      // the event changed nothing
        AwaitMetadata = 32, // joined, but Steam has not delivered both lobby data keys
        HelloReady = 64,   // lobby entered and metadata verified; start the relay hello
    };

    void Configure(uint64_t self, char const* session)
    {
        self_ = self;
        std::strncpy(session_, session ? session : "", sizeof(session_) - 1);
        session_[sizeof(session_) - 1] = '\0';
    }
    State state() const { return state_; }
    Why why() const { return why_; }
    uint64_t arg() const { return arg_; }
    uint64_t host() const { return host_; }
    uint64_t lobby() const { return lobby_; }
    uint64_t inviter() const { return inviter_; }        // the friend whose invite is being followed (display label only)
    uint64_t lobbyInvitedBy() const { return invitedBy_; }  // a lobby invite arrived that the player has not accepted yet
    char const* hostSession() const { return hostSession_; }  // WrongSession: the session_id the other side uses
    bool Active() const { return state_ == State::Requesting || state_ == State::Joining || state_ == State::Linked; }
    bool Idle() const { return state_ == State::Waiting || state_ == State::Declined || state_ == State::Lost; }

    unsigned Ready()
    {
        if (state_ != State::Starting) return Ignored;
        state_ = State::Waiting;
        return None;
    }

    // The player accepted something. friendId: the friend Steam names (0 when it names nobody, e.g. the command line).
    unsigned Accept(Source, uint64_t friendId, Target const& target, uint32_t now)
    {
        if (state_ == State::Starting) return Ignored;
        const bool busy = state_ == State::Requesting || state_ == State::Joining;
        const bool live = busy || state_ == State::Linked;
        if (target.kind == Target::Kind::None) return live ? Ignored : Decline(friendId, Why::NotInvite, 0);
        if (target.kind == Target::Kind::Host) {
            if (target.id == self_) return live ? Ignored : Decline(friendId ? friendId : target.id, Why::SelfInvite, 0);
            if (std::strcmp(target.session, session_) != 0) {
                if (live) return Ignored;
                std::strcpy(hostSession_, target.session);
                return Decline(friendId ? friendId : target.id, Why::WrongSession, 0);
            }
        }
        if (state_ == State::Linked) return Ignored;  // already connected: the live link is never disturbed
        if (busy) {
            if (target.kind == Target::Kind::Host && (target.id == host_ || (inviter_ && target.id == inviter_))) return Ignored;
            if (target.kind == Target::Kind::Lobby && target.id == lobby_) return Ignored;
            return Teardown | Begin(friendId, target, now);  // two different invites: the newest accepted one wins
        }
        return Begin(friendId, target, now);
    }

    // The host answered the join request with its lobby.
    unsigned Offer(uint64_t from, uint64_t lobby, uint32_t now)
    {
        if (state_ != State::Requesting || from != host_ || !lobby) return Ignored;
        lobby_ = lobby;
        verified_ = true;
        state_ = State::Joining;
        deadline_ = now + LobbyWaitMs;
        return JoinLobby;
    }
    // The host declined the join request.
    unsigned HostDeclined(uint64_t from, Why why, uint64_t arg, char const* hostSession)
    {
        if (state_ != State::Requesting || from != host_ || why == Why::None) return Ignored;
        hostSession_[0] = '\0';
        if (hostSession) std::strncpy(hostSession_, hostSession, sizeof(hostSession_) - 1);
        Fail(why, arg);
        return Teardown;
    }
    // Both pre-join and post-join checks may have to wait for Steam to deliver metadata.
    unsigned LobbyChecked(uint64_t lobby, LobbyMetadata metadata, uint32_t now)
    {
        if (state_ != State::Joining || lobby != lobby_ || entered_ || (verified_ && !joinAnswered_)) return Ignored;
        if (metadataPending_ && detail::Reached(now, deadline_)) {
            Fail(Why::MetadataTimeout, 0);
            return Teardown;
        }
        if (metadata == LobbyMetadata::Pending) {
            if (!metadataPending_) {
                metadataPending_ = true;
                deadline_ = now + LobbyMetadataWaitMs;
            }
            return None;
        }
        metadataPending_ = false;
        if (metadata == LobbyMetadata::Different) {
            Fail(Why::NotG3T, 0);
            return Teardown;
        }
        if (joinAnswered_) {
            entered_ = true;
            deadline_ = now + HelloWaitMs;
            return HelloReady;
        }
        verified_ = true;
        deadline_ = now + LobbyWaitMs;
        return JoinLobby;
    }
    // Steam answered JoinLobby. Do not send hello until both metadata keys match.
    unsigned LobbyEntered(uint64_t lobby, bool joined, LobbyMetadata metadata, uint64_t owner, uint32_t now)
    {
        if (state_ != State::Joining || !verified_ || joinAnswered_ || lobby != lobby_) return Ignored;
        if (!joined || !owner) {
            Fail(Why::LobbyGone, 0);
            return Teardown;
        }
        if (metadata == LobbyMetadata::Different) {
            Fail(Why::NotG3T, 0);
            return Teardown;
        }
        host_ = owner;
        joinAnswered_ = true;
        if (metadata == LobbyMetadata::Pending) {
            metadataPending_ = true;
            deadline_ = now + LobbyMetadataWaitMs;
            return AwaitMetadata;
        }
        entered_ = true;
        deadline_ = now + HelloWaitMs;
        return HelloReady;
    }
    unsigned Welcomed(uint64_t from)
    {
        if (state_ != State::Joining || !entered_ || from != host_) return Ignored;
        state_ = State::Linked;
        return None;
    }
    // The relay link with `from` ended (bye, left the lobby, silent).
    unsigned LinkLost(uint64_t from)
    {
        if (!from || from != host_) return Ignored;
        if (state_ == State::Linked) {
            state_ = State::Lost;
            return Teardown;
        }
        if (state_ == State::Requesting || state_ == State::Joining) {
            Fail(state_ == State::Requesting ? Why::Unreachable : Why::LobbyGone, 0);
            return Teardown;
        }
        return Ignored;
    }
    // Steam could not open a session with `id` (SteamNetworkingMessagesSessionFailed_t).
    unsigned SessionFailed(uint64_t id)
    {
        if (!id || id != host_) return Ignored;
        if (state_ == State::Linked) {
            state_ = State::Lost;
            return Teardown;
        }
        if (state_ == State::Requesting || state_ == State::Joining) {
            Fail(Why::Unreachable, 0);
            return Teardown;
        }
        return Ignored;
    }
    // A lobby invite arrived (LobbyInvite_t); only the player's own accept in Steam connects. Just a note for the menu.
    unsigned LobbyInvite(uint64_t friendId, uint64_t lobby)
    {
        if (!Idle()) return Ignored;
        invitedBy_ = friendId;
        invitedLobby_ = lobby;
        return None;
    }
    unsigned Tick(uint32_t now)
    {
        if ((state_ != State::Requesting && state_ != State::Joining) || !detail::Reached(now, deadline_)) return None;
        Fail(metadataPending_ ? Why::MetadataTimeout : state_ == State::Requesting || entered_ ? Why::NoAnswer : Why::LobbyGone, 0);
        return Teardown;
    }
    bool ResendDue(uint32_t now)
    {
        if (state_ != State::Requesting || !detail::Reached(now, resendAt_)) return false;
        resendAt_ = now + RequestResendMs;
        return true;
    }
    uint64_t invitedLobby() const { return invitedLobby_; }

  private:
    unsigned Decline(uint64_t friendId, Why why, uint64_t arg)
    {
        inviter_ = friendId;
        invitedBy_ = 0;
        Fail(why, arg);
        return None;
    }
    void Fail(Why why, uint64_t arg)
    {
        state_ = State::Declined;
        why_ = why;
        arg_ = arg;
        host_ = lobby_ = 0;
        verified_ = entered_ = joinAnswered_ = metadataPending_ = false;
    }
    unsigned Begin(uint64_t friendId, Target const& target, uint32_t now)
    {
        inviter_ = friendId;
        invitedBy_ = invitedLobby_ = 0;
        why_ = Why::None;
        arg_ = 0;
        hostSession_[0] = '\0';
        entered_ = joinAnswered_ = metadataPending_ = false;
        if (target.kind == Target::Kind::Host) {
            host_ = target.id;
            lobby_ = 0;
            verified_ = false;
            state_ = State::Requesting;
            deadline_ = now + RequestWaitMs;
            resendAt_ = now + RequestResendMs;
            return SendRequest;
        }
        host_ = 0;
        lobby_ = target.id;
        verified_ = false;
        state_ = State::Joining;
        deadline_ = now + LobbyWaitMs;
        return InspectLobby;
    }

    State state_ = State::Starting;
    Why why_ = Why::None;
    uint64_t arg_ = 0, self_ = 0, host_ = 0, lobby_ = 0, inviter_ = 0, invitedBy_ = 0, invitedLobby_ = 0;
    bool verified_ = false, entered_ = false, joinAnswered_ = false, metadataPending_ = false;
    uint32_t deadline_ = 0, resendAt_ = 0;
    char session_[RelayMaxSessionSize + 1] = {};
    char hostSession_[RelayMaxSessionSize + 1] = {};
};

// ---- the host ----

class HostFlow
{
  public:
    enum class State : uint8_t {
        Starting,  // Steam is not ready
        Waiting,   // rich presence published, no lobby, nobody connected
        Opening,   // a friend's valid join request arrived; the lobby is being created
        Offered,   // the lobby is open and its id went to the partner; waiting for the partner's hello
        Linked,    // the partner is linked
    };
    enum Fx : unsigned {
        None = 0,
        CreateLobby = 1,    // create the friends-only lobby (host + MaxPartners members)
        SendOffer = 2,      // send the lobby offer to partner()
        SendDecline = 4,    // send declineWhy()/declineArg() to declineTo()
        Teardown = 8,       // leave the lobby, drop the partner's relay (rich presence stays published)
        Ignored = 16,
        RotatePartner = 32, // drop the prior linked relay generation before re-offering the same partner
    };
    struct Join
    {
        uint64_t from = 0;
        bool friendOk = false;   // Steam says `from` is on this account's friends list
        bool sessionOk = false;  // the request names this host's session_id
        unsigned protocol = 0;
    };

    State state() const { return state_; }
    Mode mode() const { return mode_; }
    uint64_t partner() const { return partner_; }
    uint64_t declineTo() const { return declineTo_; }
    Why declineWhy() const { return declineWhy_; }
    uint64_t declineArg() const { return declineArg_; }

    unsigned Ready(Mode mode, uint32_t now)
    {
        if (state_ != State::Starting) return Ignored;
        mode_ = mode;
        if (mode == Mode::Auto) {
            state_ = State::Opening;
            deadline_ = now;
            return CreateLobby;
        }
        state_ = State::Waiting;
        return None;
    }
    unsigned Request(Join const& join, unsigned ownProtocol, uint32_t now)
    {
        if (state_ == State::Starting || mode_ == Mode::Auto || !join.from) return Ignored;
        if (!join.friendOk) return Decline(join.from, Why::NotFriends, 0);
        if (!join.sessionOk) return Decline(join.from, Why::WrongSession, 0);
        if (join.protocol != ownProtocol) return Decline(join.from, Why::WrongProtocol, ownProtocol);
        switch (state_) {
        case State::Waiting:
            partner_ = join.from;
            state_ = State::Opening;
            deadline_ = now + CreateWaitMs;
            return CreateLobby;
        case State::Opening:
            return join.from == partner_ ? Ignored : Decline(join.from, Why::Busy, 0);
        case State::Offered:
            return join.from == partner_ ? SendOffer : Decline(join.from, Why::Busy, 0);
        case State::Linked:
            if (join.from != partner_) return Decline(join.from, Why::Busy, 0);
            state_ = State::Offered;
            deadline_ = now + OfferWaitMs;
            return RotatePartner | SendOffer;
        default:
            return Ignored;
        }
    }
    // CreateLobby answered. A lobby nobody is waiting for (late answer after a timeout) is torn down by the caller.
    unsigned LobbyCreated(bool ok, int result, uint32_t now)
    {
        if (state_ != State::Opening) return ok ? Teardown : Ignored;
        if (mode_ == Mode::Auto) {
            if (!ok) return Ignored;  // legacy: tried again every 15 s
            state_ = State::Offered;
            return None;
        }
        if (!ok) {
            const unsigned fx = Decline(partner_, Why::HostError, static_cast<uint64_t>(result < 0 ? 0 : result));
            state_ = State::Waiting;
            partner_ = 0;
            return fx;
        }
        state_ = State::Offered;
        deadline_ = now + OfferWaitMs;
        return SendOffer;
    }
    // Only the invited partner may link (a hello from anybody else in the lobby is refused).
    bool Admit(uint64_t from) const
    {
        if (state_ != State::Offered && state_ != State::Linked) return false;
        return mode_ == Mode::Auto || (from && from == partner_);
    }
    unsigned Linked(uint64_t from)
    {
        if (!Admit(from)) return Ignored;
        if (mode_ == Mode::Auto) return None;  // legacy: the lobby stays open for more friends
        state_ = State::Linked;
        return None;
    }
    unsigned LinkLost(uint64_t from)
    {
        if (mode_ == Mode::Auto || (state_ != State::Offered && state_ != State::Linked) || !from || from != partner_) return Ignored;
        state_ = State::Waiting;
        partner_ = 0;
        return Teardown;
    }
    unsigned Tick(uint32_t now)
    {
        if (mode_ == Mode::Auto) return None;
        if (state_ == State::Opening && detail::Reached(now, deadline_)) {
            declineTo_ = partner_;
            declineWhy_ = Why::HostError;
            declineArg_ = 0;
            state_ = State::Waiting;
            partner_ = 0;
            return SendDecline | Teardown;
        }
        if (state_ == State::Offered && detail::Reached(now, deadline_)) {
            state_ = State::Waiting;
            partner_ = 0;
            return Teardown;
        }
        return None;
    }

  private:
    unsigned Decline(uint64_t to, Why why, uint64_t arg)
    {
        declineTo_ = to;
        declineWhy_ = why;
        declineArg_ = arg;
        return SendDecline;
    }

    State state_ = State::Starting;
    Mode mode_ = Mode::Invite;
    uint64_t partner_ = 0, declineTo_ = 0, declineArg_ = 0;
    Why declineWhy_ = Why::None;
    uint32_t deadline_ = 0;
};

inline char const* StateName(ClientFlow::State s)
{
    switch (s) {
    case ClientFlow::State::Starting: return "Starting";
    case ClientFlow::State::Waiting: return "WaitingForInvite";
    case ClientFlow::State::Requesting: return "Requesting";
    case ClientFlow::State::Joining: return "Joining";
    case ClientFlow::State::Linked: return "Linked";
    case ClientFlow::State::Declined: return "Declined";
    case ClientFlow::State::Lost: return "Lost";
    }
    return "?";
}
inline char const* StateName(HostFlow::State s)
{
    switch (s) {
    case HostFlow::State::Starting: return "Starting";
    case HostFlow::State::Waiting: return "WaitingForInvite";
    case HostFlow::State::Opening: return "OpeningLobby";
    case HostFlow::State::Offered: return "LobbyOffered";
    case HostFlow::State::Linked: return "Linked";
    }
    return "?";
}
}
