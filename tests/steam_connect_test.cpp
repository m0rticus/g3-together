// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/steam_connect.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef NDEBUG
#error The production policy tests require active assertions.
#endif

using namespace g3t::steam::connect;

namespace {
using C = ClientFlow;
using H = HostFlow;
using CS = C::State;
using HS = H::State;
using M = LobbyMetadata;
constexpr uint64_t Self = 7, Host = 101, Other = 202, Lobby = 1001, OtherLobby = 2002;
constexpr uint32_t Start = 1000;
constexpr unsigned Protocol = 79;

Target HostTarget(uint64_t id = Host, char const* session = "default")
{
    Target t;
    t.kind = Target::Kind::Host;
    t.id = id;
    std::strcpy(t.session, session);
    return t;
}

Target LobbyTarget(uint64_t id = Lobby)
{
    Target t;
    t.kind = Target::Kind::Lobby;
    t.id = id;
    return t;
}

struct ClientSnapshot {
    CS state;
    Why why;
    uint64_t arg, host, lobby, inviter, invitedBy, invitedLobby;
    std::string hostSession;
    bool active, idle;
};

ClientSnapshot Snapshot(C const& c)
{
    return {c.state(), c.why(), c.arg(), c.host(), c.lobby(), c.inviter(),
            c.lobbyInvitedBy(), c.invitedLobby(), c.hostSession(), c.Active(), c.Idle()};
}

bool operator==(ClientSnapshot const& a, ClientSnapshot const& b)
{
    return a.state == b.state && a.why == b.why && a.arg == b.arg && a.host == b.host &&
           a.lobby == b.lobby && a.inviter == b.inviter && a.invitedBy == b.invitedBy &&
           a.invitedLobby == b.invitedLobby && a.hostSession == b.hostSession &&
           a.active == b.active && a.idle == b.idle;
}

enum class ClientFixture {
    Starting, Waiting, InviteNoted, Requesting, Offered, Invited, Checked, Entered, Linked, Declined, Lost
};

C MakeClient(ClientFixture fixture)
{
    C c;
    c.Configure(Self, "default");
    if (fixture == ClientFixture::Starting) return c;
    assert(c.Ready() == C::None);
    if (fixture == ClientFixture::Waiting) return c;
    if (fixture == ClientFixture::InviteNoted) {
        assert(c.LobbyInvite(Other, OtherLobby) == C::None);
        return c;
    }
    if (fixture == ClientFixture::Invited || fixture == ClientFixture::Checked) {
        assert(c.Accept(C::Source::LobbyJoin, Host, LobbyTarget(), Start) == C::InspectLobby);
        if (fixture == ClientFixture::Checked) assert(c.LobbyChecked(Lobby, M::Match, Start) == C::JoinLobby);
        return c;
    }
    assert(c.Accept(C::Source::RichJoin, Host, HostTarget(), Start) == C::SendRequest);
    if (fixture == ClientFixture::Requesting) return c;
    if (fixture == ClientFixture::Declined) {
        assert(c.HostDeclined(Host, Why::WrongProtocol, 88, "other") == C::Teardown);
        return c;
    }
    assert(c.Offer(Host, Lobby, Start) == C::JoinLobby);
    if (fixture == ClientFixture::Offered) return c;
    assert(c.LobbyEntered(Lobby, true, M::Match, Host, Start) == C::HelloReady);
    if (fixture == ClientFixture::Entered) return c;
    assert(c.Welcomed(Host) == C::None);
    if (fixture == ClientFixture::Lost) assert(c.LinkLost(Host) == C::Teardown);
    return c;
}

enum class CE {
    Ready, HostSame, HostOther, HostWrongSession, HostSelf, LobbySame, LobbyOther, Foreign, Malformed,
    OfferHost, OfferOther, OfferEmpty, DeclineHost, DeclineOther, DeclineNone,
    CheckGood, CheckBad, CheckOther, EnterGood, EnterFailed, EnterMismatch, EnterNoOwner, EnterOther,
    WelcomeHost, WelcomeOther, LostHost, LostOther, LostZero, FailedHost, FailedOther, Invite,
    TickBefore, TickAt, TickAfter, ResendBefore, ResendAt
};
struct ClientEvent { CE kind; C::Source source = C::Source::RichJoin; };

uint32_t ClientDeadline(ClientFixture f)
{
    if (f == ClientFixture::Requesting) return Start + RequestWaitMs;
    if (f == ClientFixture::Entered) return Start + HelloWaitMs;
    return Start + LobbyWaitMs;
}

unsigned Apply(C& c, ClientEvent e, ClientFixture f)
{
    switch (e.kind) {
    case CE::Ready: return c.Ready();
    case CE::HostSame: return c.Accept(e.source, Host, HostTarget(), Start);
    case CE::HostOther: return c.Accept(e.source, Other, HostTarget(Other), Start);
    case CE::HostWrongSession: return c.Accept(e.source, Host, HostTarget(Host, "other"), Start);
    case CE::HostSelf: return c.Accept(e.source, 0, HostTarget(Self), Start);
    case CE::LobbySame: return c.Accept(e.source, Host, LobbyTarget(), Start);
    case CE::LobbyOther: return c.Accept(e.source, Other, LobbyTarget(OtherLobby), Start);
    case CE::Foreign: return c.Accept(e.source, Other, ParseConnect("+connect 1.2.3.4:27015"), Start);
    case CE::Malformed: return c.Accept(e.source, Other, ParseConnect("+g3t_connect 7656"), Start);
    case CE::OfferHost: return c.Offer(Host, Lobby, Start);
    case CE::OfferOther: return c.Offer(Other, Lobby, Start);
    case CE::OfferEmpty: return c.Offer(Host, 0, Start);
    case CE::DeclineHost: return c.HostDeclined(Host, Why::Busy, 12, "remote");
    case CE::DeclineOther: return c.HostDeclined(Other, Why::Busy, 12, "remote");
    case CE::DeclineNone: return c.HostDeclined(Host, Why::None, 12, "remote");
    case CE::CheckGood: return c.LobbyChecked(Lobby, M::Match, Start);
    case CE::CheckBad: return c.LobbyChecked(Lobby, M::Different, Start);
    case CE::CheckOther: return c.LobbyChecked(OtherLobby, M::Match, Start);
    case CE::EnterGood: return c.LobbyEntered(Lobby, true, M::Match, Host, Start);
    case CE::EnterFailed: return c.LobbyEntered(Lobby, false, M::Match, Host, Start);
    case CE::EnterMismatch: return c.LobbyEntered(Lobby, true, M::Different, Host, Start);
    case CE::EnterNoOwner: return c.LobbyEntered(Lobby, true, M::Match, 0, Start);
    case CE::EnterOther: return c.LobbyEntered(OtherLobby, true, M::Match, Host, Start);
    case CE::WelcomeHost: return c.Welcomed(Host);
    case CE::WelcomeOther: return c.Welcomed(Other);
    case CE::LostHost: return c.LinkLost(Host);
    case CE::LostOther: return c.LinkLost(Other);
    case CE::LostZero: return c.LinkLost(0);
    case CE::FailedHost: return c.SessionFailed(Host);
    case CE::FailedOther: return c.SessionFailed(Other);
    case CE::Invite: return c.LobbyInvite(Other, OtherLobby);
    case CE::TickBefore: return c.Tick(ClientDeadline(f) - 1);
    case CE::TickAt: return c.Tick(ClientDeadline(f));
    case CE::TickAfter: return c.Tick(ClientDeadline(f) + 1);
    case CE::ResendBefore: return c.ResendDue(Start + RequestResendMs - 1) ? 1u : 0u;
    case CE::ResendAt: return c.ResendDue(Start + RequestResendMs) ? 1u : 0u;
    }
    assert(false);
    return 0;
}

// Oracle is stated from the section 4 state table and effect contracts, not from flow internals.
struct ClientExpected { ClientSnapshot snapshot; unsigned fx; };

void ClientState(ClientSnapshot& s, CS state)
{
    s.state = state;
    s.active = state == CS::Requesting || state == CS::Joining || state == CS::Linked;
    s.idle = state == CS::Waiting || state == CS::Declined || state == CS::Lost;
}

void ClientFail(ClientSnapshot& s, Why why, uint64_t arg = 0)
{
    ClientState(s, CS::Declined);
    s.why = why;
    s.arg = arg;
    s.host = s.lobby = 0;
}

ClientExpected Expected(ClientSnapshot s, ClientFixture f, ClientEvent e)
{
    unsigned fx = C::Ignored;
    const bool underway = f == ClientFixture::Requesting || f == ClientFixture::Offered ||
                          f == ClientFixture::Invited || f == ClientFixture::Checked ||
                          f == ClientFixture::Entered;
    const bool awaitingEntry = f == ClientFixture::Offered || f == ClientFixture::Checked;
    const bool isAccept = e.kind >= CE::HostSame && e.kind <= CE::Malformed;
    if (isAccept && f != ClientFixture::Starting && f != ClientFixture::Linked) {
        const bool invalid = e.kind == CE::Foreign || e.kind == CE::Malformed ||
                             e.kind == CE::HostSelf || e.kind == CE::HostWrongSession;
        if (invalid && !underway) {
            s.inviter = e.kind == CE::HostSelf ? Self :
                        e.kind == CE::HostWrongSession ? Host : Other;
            s.invitedBy = 0;
            if (e.kind == CE::HostWrongSession) s.hostSession = "other";
            ClientFail(s, e.kind == CE::HostSelf ? Why::SelfInvite :
                          e.kind == CE::HostWrongSession ? Why::WrongSession : Why::NotInvite);
            fx = C::None;
        } else if (!invalid) {
            const bool hostTarget = e.kind == CE::HostSame || e.kind == CE::HostOther;
            const uint64_t id = e.kind == CE::HostSame ? Host :
                                e.kind == CE::HostOther ? Other :
                                e.kind == CE::LobbySame ? Lobby : OtherLobby;
            const bool duplicate = underway && (hostTarget ? (id == s.host || (s.inviter && id == s.inviter))
                                                           : id == s.lobby);
            if (!duplicate) {
                s.inviter = e.kind == CE::HostSame || e.kind == CE::LobbySame ? Host : Other;
                s.invitedBy = s.invitedLobby = 0;
                s.why = Why::None;
                s.arg = 0;
                s.hostSession.clear();
                s.host = hostTarget ? id : 0;
                s.lobby = hostTarget ? 0 : id;
                ClientState(s, hostTarget ? CS::Requesting : CS::Joining);
                fx = (hostTarget ? C::SendRequest : C::InspectLobby) | (underway ? C::Teardown : C::None);
            }
        }
    } else if (e.kind == CE::Ready && f == ClientFixture::Starting) {
        ClientState(s, CS::Waiting);
        fx = C::None;
    } else if (e.kind == CE::OfferHost && f == ClientFixture::Requesting) {
        s.lobby = Lobby;
        ClientState(s, CS::Joining);
        fx = C::JoinLobby;
    } else if (e.kind == CE::DeclineHost && f == ClientFixture::Requesting) {
        s.hostSession = "remote";
        ClientFail(s, Why::Busy, 12);
        fx = C::Teardown;
    } else if ((e.kind == CE::CheckGood || e.kind == CE::CheckBad) && f == ClientFixture::Invited) {
        if (e.kind == CE::CheckGood) fx = C::JoinLobby;
        else { ClientFail(s, Why::NotG3T); fx = C::Teardown; }
    } else if (e.kind >= CE::EnterGood && e.kind <= CE::EnterNoOwner && awaitingEntry) {
        if (e.kind == CE::EnterGood) { s.host = Host; fx = C::HelloReady; }
        else {
            ClientFail(s, e.kind == CE::EnterMismatch ? Why::NotG3T : Why::LobbyGone);
            fx = C::Teardown;
        }
    } else if (e.kind == CE::WelcomeHost && f == ClientFixture::Entered) {
        ClientState(s, CS::Linked);
        fx = C::None;
    } else if ((e.kind == CE::LostHost || e.kind == CE::FailedHost) && s.host == Host &&
               (underway || f == ClientFixture::Linked)) {
        if (f == ClientFixture::Linked) ClientState(s, CS::Lost);
        else ClientFail(s, e.kind == CE::FailedHost || f == ClientFixture::Requesting
                              ? Why::Unreachable : Why::LobbyGone);
        fx = C::Teardown;
    } else if (e.kind == CE::Invite && s.idle) {
        s.invitedBy = Other;
        s.invitedLobby = OtherLobby;
        fx = C::None;
    } else if (e.kind >= CE::TickBefore && e.kind <= CE::TickAfter) {
        fx = C::None;
        if (underway && e.kind != CE::TickBefore) {
            ClientFail(s, f == ClientFixture::Requesting || f == ClientFixture::Entered
                              ? Why::NoAnswer : Why::LobbyGone);
            fx = C::Teardown;
        }
    } else if (e.kind == CE::ResendBefore || e.kind == CE::ResendAt) {
        fx = e.kind == CE::ResendAt && f == ClientFixture::Requesting ? 1u : 0u;
    }
    return {s, fx};
}

void ClientTable()
{
    const ClientFixture fixtures[] = {ClientFixture::Starting, ClientFixture::Waiting, ClientFixture::InviteNoted, ClientFixture::Requesting,
        ClientFixture::Offered, ClientFixture::Invited, ClientFixture::Checked, ClientFixture::Entered,
        ClientFixture::Linked, ClientFixture::Declined, ClientFixture::Lost};
    std::vector<ClientEvent> events;
    for (int i = static_cast<int>(CE::Ready); i <= static_cast<int>(CE::ResendAt); ++i)
        events.push_back({static_cast<CE>(i), C::Source::RichJoin});
    for (auto source : {C::Source::CommandLine, C::Source::LobbyJoin, C::Source::Manual})
        for (int i = static_cast<int>(CE::HostSame); i <= static_cast<int>(CE::Malformed); ++i)
            events.push_back({static_cast<CE>(i), source});
    unsigned cases = 0;
    for (auto fixture : fixtures) for (auto event : events) {
        C c = MakeClient(fixture);
        const auto before = Snapshot(c);
        const auto expected = Expected(before, fixture, event);
        const unsigned actual = Apply(c, event, fixture);
        if (actual != expected.fx || !(Snapshot(c) == expected.snapshot))
            std::cerr << "client fixture=" << static_cast<int>(fixture) << " event="
                      << static_cast<int>(event.kind) << " source=" << static_cast<int>(event.source)
                      << " fx=" << actual << " expected=" << expected.fx << '\n';
        assert(actual == expected.fx);
        assert(Snapshot(c) == expected.snapshot);
        if (actual == C::Ignored && event.kind != CE::ResendAt && event.kind != CE::ResendBefore)
            assert(Snapshot(c) == before);
        if (event.kind != CE::ResendAt && event.kind != CE::ResendBefore) {
            if (actual & C::SendRequest) assert(event.kind == CE::HostSame || event.kind == CE::HostOther);
            if (actual & C::InspectLobby) assert(event.kind == CE::LobbySame || event.kind == CE::LobbyOther);
            if (actual & C::JoinLobby) assert(event.kind == CE::OfferHost || event.kind == CE::CheckGood);
        }
        ++cases;
    }
    std::cout << "Client policy cases: " << cases << '\n';
}

struct HostSnapshot {
    HS state;
    Mode mode;
    uint64_t partner, declineTo, declineArg;
    Why declineWhy;
};

HostSnapshot Snapshot(H const& h)
{
    return {h.state(), h.mode(), h.partner(), h.declineTo(), h.declineArg(), h.declineWhy()};
}

bool operator==(HostSnapshot const& a, HostSnapshot const& b)
{
    return a.state == b.state && a.mode == b.mode && a.partner == b.partner &&
           a.declineTo == b.declineTo && a.declineArg == b.declineArg && a.declineWhy == b.declineWhy;
}

struct HostFixture { HS state; Mode mode; bool priorDecline = false; };
H::Join Join(uint64_t from = Host, bool friendOk = true, bool sessionOk = true, unsigned protocol = Protocol)
{
    return {from, friendOk, sessionOk, protocol};
}

H MakeHost(HostFixture fixture)
{
    H h;
    if (fixture.state == HS::Starting) return h;
    assert(h.Ready(fixture.mode, Start) == (fixture.mode == Mode::Auto ? H::CreateLobby : H::None));
    if (fixture.priorDecline)
        assert(h.Request(Join(Other, true, true, 76), Protocol, Start) == H::SendDecline);
    if (fixture.state == HS::Waiting) return h;
    if (fixture.mode == Mode::Invite) assert(h.Request(Join(), Protocol, Start) == H::CreateLobby);
    if (fixture.state == HS::Opening) return h;
    assert(h.LobbyCreated(true, 1, Start) == (fixture.mode == Mode::Auto ? H::None : H::SendOffer));
    if (fixture.state == HS::Offered) return h;
    assert(h.Linked(Host) == H::None);
    return h;
}

enum class HE {
    ReadyInvite, ReadyAuto, NotFriend, WrongSession, WrongProtocol, Partner, OtherFriend, Zero,
    Created, Failed, AdmitPartner, AdmitOther, AdmitZero, LinkPartner, LinkOther, LinkZero,
    LostPartner, LostOther, LostZero, TickBefore, TickAt, TickAfter
};

uint32_t HostDeadline(HostFixture f)
{
    return Start + (f.state == HS::Opening ? CreateWaitMs : OfferWaitMs);
}

unsigned Apply(H& h, HostFixture f, HE event)
{
    switch (event) {
    case HE::ReadyInvite: return h.Ready(Mode::Invite, Start);
    case HE::ReadyAuto: return h.Ready(Mode::Auto, Start);
    case HE::NotFriend: return h.Request(Join(Other, false), Protocol, Start);
    case HE::WrongSession: return h.Request(Join(Other, true, false), Protocol, Start);
    case HE::WrongProtocol: return h.Request(Join(Other, true, true, 76), Protocol, Start);
    case HE::Partner: return h.Request(Join(), Protocol, Start);
    case HE::OtherFriend: return h.Request(Join(Other), Protocol, Start);
    case HE::Zero: return h.Request(Join(0), Protocol, Start);
    case HE::Created: return h.LobbyCreated(true, 1, Start);
    case HE::Failed: return h.LobbyCreated(false, 42, Start);
    case HE::AdmitPartner: return h.Admit(Host) ? 1u : 0u;
    case HE::AdmitOther: return h.Admit(Other) ? 1u : 0u;
    case HE::AdmitZero: return h.Admit(0) ? 1u : 0u;
    case HE::LinkPartner: return h.Linked(Host);
    case HE::LinkOther: return h.Linked(Other);
    case HE::LinkZero: return h.Linked(0);
    case HE::LostPartner: return h.LinkLost(Host);
    case HE::LostOther: return h.LinkLost(Other);
    case HE::LostZero: return h.LinkLost(0);
    case HE::TickBefore: return h.Tick(HostDeadline(f) - 1);
    case HE::TickAt: return h.Tick(HostDeadline(f));
    case HE::TickAfter: return h.Tick(HostDeadline(f) + 1);
    }
    assert(false);
    return 0;
}

struct HostExpected { HostSnapshot snapshot; unsigned fx; };

// Host table: validation precedes the one-partner reservation; auto never negotiates.
HostExpected Expected(HostSnapshot s, HostFixture f, HE e)
{
    unsigned fx = H::Ignored;
    if ((e == HE::ReadyInvite || e == HE::ReadyAuto) && f.state == HS::Starting) {
        s.mode = e == HE::ReadyAuto ? Mode::Auto : Mode::Invite;
        s.state = s.mode == Mode::Auto ? HS::Opening : HS::Waiting;
        fx = s.mode == Mode::Auto ? H::CreateLobby : H::None;
    } else if (e >= HE::NotFriend && e <= HE::Zero && f.state != HS::Starting &&
               f.mode == Mode::Invite && e != HE::Zero) {
        if (e == HE::NotFriend || e == HE::WrongSession || e == HE::WrongProtocol) {
            s.declineTo = Other;
            s.declineWhy = e == HE::NotFriend ? Why::NotFriends :
                           e == HE::WrongSession ? Why::WrongSession : Why::WrongProtocol;
            s.declineArg = e == HE::WrongProtocol ? Protocol : 0;
            fx = H::SendDecline;
        } else if (f.state == HS::Waiting) {
            s.partner = e == HE::Partner ? Host : Other;
            s.state = HS::Opening;
            fx = H::CreateLobby;
        } else if (e == HE::OtherFriend) {
            s.declineTo = Other;
            s.declineWhy = Why::Busy;
            s.declineArg = 0;
            fx = H::SendDecline;
        } else if (f.state == HS::Offered || f.state == HS::Linked) {
            fx = H::SendOffer;
        }
    } else if (e == HE::Created || e == HE::Failed) {
        if (f.state != HS::Opening) {
            fx = e == HE::Created ? H::Teardown : H::Ignored;
        } else if (f.mode == Mode::Auto) {
            if (e == HE::Created) { s.state = HS::Offered; fx = H::None; }
        } else if (e == HE::Created) {
            s.state = HS::Offered;
            fx = H::SendOffer;
        } else {
            s.declineTo = Host;
            s.declineWhy = Why::HostError;
            s.declineArg = 42;
            s.state = HS::Waiting;
            s.partner = 0;
            fx = H::SendDecline;
        }
    } else if (e >= HE::AdmitPartner && e <= HE::AdmitZero) {
        fx = (f.state == HS::Offered || f.state == HS::Linked) &&
             (f.mode == Mode::Auto || e == HE::AdmitPartner) ? 1u : 0u;
    } else if (e >= HE::LinkPartner && e <= HE::LinkZero) {
        const bool admitted = (f.state == HS::Offered || f.state == HS::Linked) &&
                              (f.mode == Mode::Auto || e == HE::LinkPartner);
        if (admitted) {
            if (f.mode == Mode::Invite) s.state = HS::Linked;
            fx = H::None;
        }
    } else if (e == HE::LostPartner && f.mode == Mode::Invite &&
               (f.state == HS::Offered || f.state == HS::Linked)) {
        s.state = HS::Waiting;
        s.partner = 0;
        fx = H::Teardown;
    } else if (e >= HE::TickBefore && e <= HE::TickAfter) {
        fx = H::None;
        if (f.mode == Mode::Invite && e != HE::TickBefore && f.state == HS::Opening) {
            s.declineTo = Host;
            s.declineWhy = Why::HostError;
            s.declineArg = 0;
            s.state = HS::Waiting;
            s.partner = 0;
            fx = H::SendDecline | H::Teardown;
        } else if (f.mode == Mode::Invite && e != HE::TickBefore && f.state == HS::Offered) {
            s.state = HS::Waiting;
            s.partner = 0;
            fx = H::Teardown;
        }
    }
    return {s, fx};
}

void HostTable()
{
    // Auto never reaches Waiting or Linked: it keeps its shared lobby in Offered.
    const HostFixture fixtures[] = {{HS::Starting, Mode::Invite}, {HS::Waiting, Mode::Invite},
        {HS::Opening, Mode::Invite}, {HS::Offered, Mode::Invite}, {HS::Linked, Mode::Invite},
        {HS::Opening, Mode::Auto}, {HS::Offered, Mode::Auto},
        {HS::Waiting, Mode::Invite, true}, {HS::Opening, Mode::Invite, true},
        {HS::Offered, Mode::Invite, true}, {HS::Linked, Mode::Invite, true}};
    unsigned cases = 0;
    for (auto fixture : fixtures) {
        for (int i = static_cast<int>(HE::ReadyInvite); i <= static_cast<int>(HE::TickAfter); ++i) {
            const auto event = static_cast<HE>(i);
            H h = MakeHost(fixture);
            const auto before = Snapshot(h);
            const auto expected = Expected(before, fixture, event);
            const unsigned actual = Apply(h, fixture, event);
            if (actual != expected.fx || !(Snapshot(h) == expected.snapshot))
                std::cerr << "host state=" << static_cast<int>(fixture.state) << " mode="
                          << static_cast<int>(fixture.mode) << " event=" << i
                          << " fx=" << actual << " expected=" << expected.fx << '\n';
            assert(actual == expected.fx);
            assert(Snapshot(h) == expected.snapshot);
            if (actual == H::Ignored && !(event >= HE::AdmitPartner && event <= HE::AdmitZero))
                assert(Snapshot(h) == before);
            if (fixture.mode == Mode::Auto &&
                ((event >= HE::LinkPartner && event <= HE::LinkZero) ||
                 (event >= HE::LostPartner && event <= HE::LostZero)))
                assert(!(actual & H::Teardown));
            ++cases;
        }
    }
    std::cout << "Host policy cases: " << cases << '\n';
}

void ConnectStrings()
{
    constexpr uint64_t id = 76561198068674551ULL;
    for (char const* text : {
             "\"C:\\Program Files\\Gothic 3\\Gothic3.exe\" +g3t_connect 76561198068674551 default",
             "+g3t_connect 76561198068674551 default",
             "\"+g3t_connect 76561198068674551 default\"",
             "-g3t_connect 76561198068674551 default",
             "+G3T_Connect 76561198068674551 default"}) {
        const auto t = ParseConnect(text);
        assert(t.kind == Target::Kind::Host && t.id == id && !t.malformed);
        assert(std::strcmp(t.session, "default") == 0);
    }
    const auto lobby = ParseConnect("+connect_lobby 109775243837622925");
    assert(lobby.kind == Target::Kind::Lobby && lobby.id == 109775243837622925ULL && !lobby.malformed);
    for (char const* text : {"", "unrelated text", "+connect 1.2.3.4:27015"}) {
        const auto t = ParseConnect(text);
        assert(t.kind == Target::Kind::None && !t.malformed);
    }
    assert(!ParseConnect(nullptr).malformed);
    const std::string tooLong(64, 'x');
    const std::string garbage(10000, 'X');
    const std::vector<std::string> malformed = {
        "+g3t_connect 7656", "+g3t_connect 0 default", "+g3t_connect 18446744073709551616 default",
        "+g3t_connect 123 bad!session", "+g3t_connect 123 " + tooLong,
        "+g3t_connect " + garbage + " default", "+g3t_connect 123 " + garbage,
        "+connect_lobby 0", "+connect_lobby 18446744073709551616",
        "+connect_lobby " + garbage
    };
    for (auto const& text : malformed) {
        const auto t = ParseConnect(text.c_str());
        assert(t.kind == Target::Kind::None && t.malformed);
    }
    assert(ParseConnect(garbage.c_str()).kind == Target::Kind::None);
    assert(!ParseConnect(garbage.c_str()).malformed);
    char buffer[256] = {};
    for (auto const& session : {std::string("default"), std::string(63, 's')}) {
        const auto n = FormatConnect(buffer, sizeof(buffer), id, session.c_str());
        assert(n && n == std::strlen(buffer) && n < 256);
        const auto t = ParseConnect(buffer);
        assert(t.kind == Target::Kind::Host && t.id == id && session == t.session && !t.malformed);
        char exact[256] = {};
        assert(FormatConnect(exact, n + 1, id, session.c_str()) == n);
        assert(FormatConnect(exact, n, id, session.c_str()) == 0);
    }
    assert(FormatConnect(buffer, sizeof(buffer), UINT64_MAX, std::string(63, 'a').c_str()) < 256);
    assert(!FormatConnect(buffer, sizeof(buffer), 0, "default"));
    assert(!FormatConnect(buffer, sizeof(buffer), id, tooLong.c_str()));
    assert(!FormatConnect(buffer, sizeof(buffer), id, "bad!session"));
    Mode mode = Mode::Auto;
    assert(ParseMode("InViTe", &mode) && mode == Mode::Invite);
    assert(ParseMode("AUTO", &mode) && mode == Mode::Auto);
    assert(ParseMode("invite", nullptr));
    assert(!ParseMode("other", &mode) && mode == Mode::Auto);
    assert(!ParseMode(nullptr, &mode) && mode == Mode::Auto);
}

Frame Decode(std::string const& text)
{
    Frame frame;
    assert(DecodeFrame(text.data(), text.size(), &frame));
    return frame;
}

void Frames()
{
    char buffer[FrameMax + 1] = {};
    auto n = EncodeJoin(buffer, sizeof(buffer), "default", Protocol);
    assert(n && IsFrame(buffer, n));
    auto frame = Decode(std::string(buffer, n));
    assert(frame.type == 'J' && frame.protocol == Protocol && std::strcmp(frame.session, "default") == 0);
    const std::string maxSession(63, 's');
    n = EncodeJoin(buffer, sizeof(buffer), maxSession.c_str(), 65535);
    frame = Decode(std::string(buffer, n));
    assert(frame.protocol == 65535 && frame.session == maxSession);
    n = EncodeOffer(buffer, sizeof(buffer), 109775243837622925ULL, "default");
    frame = Decode(std::string(buffer, n));
    assert(frame.type == 'L' && frame.lobby == 109775243837622925ULL && std::strcmp(frame.session, "default") == 0);
    for (auto why : {Why::WrongSession, Why::WrongProtocol, Why::Busy, Why::NotFriends, Why::HostError}) {
        n = EncodeDecline(buffer, sizeof(buffer), why, "other", Protocol);
        assert(n);
        frame = Decode(std::string(buffer, n));
        assert(frame.type == 'N' && DeclineWhy(frame.code) == why && DeclineCode(why) == frame.code);
        if (why == Why::WrongSession) assert(std::strcmp(frame.session, "other") == 0);
        if (why == Why::WrongProtocol || why == Why::HostError) assert(frame.number == Protocol);
    }
    n = EncodeDecline(buffer, sizeof(buffer), Why::HostError, nullptr, 0);
    assert(Decode(std::string(buffer, n)).number == 0);
    assert(!EncodeDecline(buffer, sizeof(buffer), Why::NoAnswer, nullptr, 0));
    assert(!EncodeJoin(buffer, 3, "default", Protocol));
    assert(!EncodeOffer(buffer, sizeof(buffer), 0, "default"));
    assert(!EncodeJoin(buffer, sizeof(buffer), "bad!", Protocol));
    const std::vector<std::string> bad = {
        "XXT1J default 78", "G3T1X default 78", "G3T1J default 78 extra",
        "G3T1J default 78 ", "G3T1J  default 78", "G3T1J default  78",
        std::string("G3T1J default ") + char(1), std::string(129, 'x').replace(0, 5, "G3T1J"),
        "G3T1J default 0", "G3T1J default -1", "G3T1J default 65536",
        "G3T1J default 18446744073709551616", "G3T1L 0 default",
        "G3T1L 18446744073709551616 default", "G3T1N X -",
        "G3T1N B 1", "G3T1N F 0", "G3T1N P abc", "G3T1N E -1",
        "G3T1N S bad!", "G3T1J default 78\n"
    };
    for (auto const& text : bad) assert(!DecodeFrame(text.data(), text.size(), &frame));
    assert(!IsFrame(nullptr, 0));
    unsigned char packet[g3t::steam::RelayMaxEnvelopeSize] = {};
    std::size_t packetSize = 0;
    const char payload[] = "G3T1J default 78";
    constexpr uint64_t lobby = 109775243837622925ULL;
    assert(g3t::steam::RelayEncode(lobby, "default", payload, sizeof(payload) - 1,
                                   packet, sizeof(packet), &packetSize));
    void const* decoded = nullptr;
    std::size_t decodedSize = 0;
    assert(g3t::steam::RelayDecode(packet, packetSize, lobby, "default", &decoded, &decodedSize));
    assert(decodedSize == sizeof(payload) - 1 && std::memcmp(decoded, payload, decodedSize) == 0);
    assert(!IsFrame(packet, packetSize));
    assert(!DecodeFrame(packet, packetSize, &frame));
}

// The decode check runs normally; define the macro for the failing IsFrame regression.
void RelayPrefixCollision()
{
    unsigned char packet[256] = {};
    std::size_t size = 0;
    // Little-endian lobby bytes begin with G3T1; this is a valid relay envelope.
    constexpr uint64_t lobby = 0x0186000031543347ULL;
    assert(g3t::steam::RelayEncode(lobby, "default", nullptr, 0, packet, sizeof(packet), &size));
    assert(g3t::steam::RelayDecode(packet, size, lobby, "default", nullptr, nullptr));
    Frame frame;
    assert(!DecodeFrame(packet, size, &frame));
#ifdef TEST_KNOWN_RELAY_PREFIX_COLLISION
    assert(!IsFrame(packet, size));
#endif
}

// Opt-in failing regression; the normal suite remains runnable against the current header.
#ifdef TEST_KNOWN_TRUNCATED_TOKEN
void TruncatedToken()
{
    // The complete lobby ID is 12, but a truncated token must never be accepted as ID 1.
    const std::string text = "+connect_lobby " + std::string(94, '0') + "12";
    const auto target = ParseConnect(text.c_str());
    assert((target.kind == Target::Kind::None && target.malformed) ||
           (target.kind == Target::Kind::Lobby && target.id == 12 && !target.malformed));
}
#endif

unsigned SendJoin(C const& client, H& host, uint64_t clientId, bool friendOk,
                  char const* hostSession, unsigned clientProtocol, unsigned hostProtocol, uint32_t now)
{
    assert(client.state() == CS::Requesting);
    char wire[FrameMax + 1] = {};
    const auto n = EncodeJoin(wire, sizeof(wire), "default", clientProtocol);
    const auto frame = Decode(std::string(wire, n));
    assert(frame.type == 'J');
    return host.Request(Join(clientId, friendOk, std::strcmp(frame.session, hostSession) == 0,
                             frame.protocol), hostProtocol, now);
}

void DeliverOffer(H const& host, C& client, uint32_t now)
{
    assert(host.state() == HS::Offered || host.state() == HS::Linked);
    char wire[FrameMax + 1] = {};
    const auto n = EncodeOffer(wire, sizeof(wire), Lobby, "default");
    const auto frame = Decode(std::string(wire, n));
    assert(frame.type == 'L' && std::strcmp(frame.session, "default") == 0);
    assert(client.Offer(Host, frame.lobby, now) == C::JoinLobby);
}

void DeliverDecline(H const& host, C& client, uint64_t clientId, char const* session)
{
    assert(host.declineTo() == clientId);
    char wire[FrameMax + 1] = {};
    const auto n = EncodeDecline(wire, sizeof(wire), host.declineWhy(), session, host.declineArg());
    const auto frame = Decode(std::string(wire, n));
    assert(frame.type == 'N');
    assert(client.HostDeclined(Host, DeclineWhy(frame.code), frame.number, frame.session) == C::Teardown);
}

C ReadyClient(uint64_t self = Self)
{
    C c;
    c.Configure(self, "default");
    assert(c.Ready() == C::None);
    return c;
}

void Simulation()
{
    H host;
    assert(host.Ready(Mode::Invite, Start) == H::None);
    C client = ReadyClient();
    assert(client.Accept(C::Source::RichJoin, Host, HostTarget(), Start) == C::SendRequest);
    assert(SendJoin(client, host, Self, true, "default", Protocol, Protocol, Start) == H::CreateLobby);
    assert(host.partner() == Self);
    assert(host.LobbyCreated(true, 1, Start) == H::SendOffer);
    DeliverOffer(host, client, Start);
    assert(client.LobbyEntered(Lobby, true, M::Match, Host, Start) == C::HelloReady);
    assert(host.Admit(Self) && !host.Admit(Other));
    assert(host.Linked(Self) == H::None);
    assert(client.Welcomed(Host) == C::None);
    assert(host.state() == HS::Linked && client.state() == CS::Linked);

    // A linked host refuses a second friend without opening another lobby.
    C second = ReadyClient(Other);
    assert(second.Accept(C::Source::CommandLine, Host, HostTarget(), Start) == C::SendRequest);
    assert(SendJoin(second, host, Other, true, "default", Protocol, Protocol, Start) == H::SendDecline);
    assert(host.declineWhy() == Why::Busy && host.partner() == Self && host.state() == HS::Linked);
    DeliverDecline(host, second, Other, "default");
    assert(second.state() == CS::Declined && second.why() == Why::Busy);

    // Host quits: the client is inert until a fresh accept, and the host can reserve again.
    assert(client.LinkLost(Host) == C::Teardown && client.state() == CS::Lost);
    assert(host.LinkLost(Self) == H::Teardown && host.state() == HS::Waiting);
    assert(client.Tick(Start + RequestWaitMs * 4) == C::None);
    assert(client.Accept(C::Source::RichJoin, Host, HostTarget(), Start) == C::SendRequest);
    assert(SendJoin(client, host, Self, true, "default", Protocol, Protocol, Start) == H::CreateLobby);

    C localMismatch = ReadyClient();
    assert(localMismatch.Accept(C::Source::RichJoin, Host, HostTarget(Host, "other"), Start) == C::None);
    assert(localMismatch.state() == CS::Declined && localMismatch.why() == Why::WrongSession);
    assert(std::strcmp(localMismatch.hostSession(), "other") == 0);

    // Independently start host/client pairs for each validation denial.
    for (auto why : {Why::WrongSession, Why::WrongProtocol, Why::NotFriends}) {
        H h;
        assert(h.Ready(Mode::Invite, Start) == H::None);
        C c = ReadyClient();
        assert(c.Accept(C::Source::Manual, Host, HostTarget(), Start) == C::SendRequest);
        const char* hostSession = why == Why::WrongSession ? "other" : "default";
        const unsigned clientProtocol = why == Why::WrongProtocol ? 76 : Protocol;
        const bool friendOk = why != Why::NotFriends;
        assert(SendJoin(c, h, Self, friendOk, hostSession, clientProtocol, Protocol, Start) == H::SendDecline);
        assert(h.state() == HS::Waiting && h.partner() == 0 && h.declineWhy() == why);
        DeliverDecline(h, c, Self, hostSession);
        assert(c.state() == CS::Declined && c.why() == why);
        if (why == Why::WrongSession) assert(std::strcmp(c.hostSession(), "other") == 0);
        if (why == Why::WrongProtocol) assert(c.arg() == Protocol);
    }

    // An unanswered hello times out separately on each side.
    H noHello;
    assert(noHello.Ready(Mode::Invite, Start) == H::None);
    C waitingHello = ReadyClient();
    assert(waitingHello.Accept(C::Source::RichJoin, Host, HostTarget(), Start) == C::SendRequest);
    assert(SendJoin(waitingHello, noHello, Self, true, "default", Protocol, Protocol, Start) == H::CreateLobby);
    assert(noHello.LobbyCreated(true, 1, Start) == H::SendOffer);
    DeliverOffer(noHello, waitingHello, Start);
    assert(waitingHello.LobbyEntered(Lobby, true, M::Match, Host, Start) == C::HelloReady);
    assert(waitingHello.Tick(Start + HelloWaitMs - 1) == C::None);
    assert(waitingHello.Tick(Start + HelloWaitMs) == C::Teardown && waitingHello.why() == Why::NoAnswer);
    assert(noHello.Tick(Start + OfferWaitMs - 1) == H::None);
    assert(noHello.Tick(Start + OfferWaitMs) == H::Teardown && noHello.state() == HS::Waiting);

    for (bool check : {false, true}) {
        C stale = ReadyClient();
        assert(stale.Accept(C::Source::LobbyJoin, Host, LobbyTarget(), Start) == C::InspectLobby);
        if (check) {
            assert(stale.LobbyChecked(Lobby, M::Match, Start) == C::JoinLobby);
            assert(stale.LobbyEntered(Lobby, false, M::Match, Host, Start) == C::Teardown);
            assert(stale.why() == Why::LobbyGone);
        } else {
            assert(stale.LobbyChecked(Lobby, M::Different, Start) == C::Teardown);
            assert(stale.why() == Why::NotG3T);
        }
        assert(stale.state() == CS::Declined);
    }

    C unanswered = ReadyClient();
    assert(unanswered.Accept(C::Source::RichJoin, Host, HostTarget(), Start) == C::SendRequest);
    for (unsigned i = 1; i < RequestWaitMs / RequestResendMs; ++i) {
        assert(!unanswered.ResendDue(Start + i * RequestResendMs - 1));
        assert(unanswered.ResendDue(Start + i * RequestResendMs));
        assert(!unanswered.ResendDue(Start + i * RequestResendMs));
    }
    assert(unanswered.Tick(Start + RequestWaitMs - 1) == C::None);
    assert(unanswered.Tick(Start + RequestWaitMs) == C::Teardown);
    assert(unanswered.state() == CS::Declined && unanswered.why() == Why::NoAnswer);
    assert(!unanswered.ResendDue(Start + RequestWaitMs + RequestResendMs));
}

void MetadataReplication()
{
    assert(CheckLobbyMetadata("", "3", "", "default") == M::Pending);
    assert(CheckLobbyMetadata("3", "3", "", "default") == M::Pending);
    assert(CheckLobbyMetadata("", "3", "default", "default") == M::Pending);
    assert(CheckLobbyMetadata("4", "3", "", "default") == M::Pending);
    assert(CheckLobbyMetadata(nullptr, "3", "default", "default") == M::Pending);
    assert(CheckLobbyMetadata("3", "3", "default", "default") == M::Match);
    assert(CheckLobbyMetadata("4", "3", "default", "default") == M::Different);
    assert(CheckLobbyMetadata("3", "3", "Default", "default") == M::Different);

    // The host's offer arrives before its SetLobbyData has replicated to this client.
    C offered = ReadyClient();
    assert(offered.Accept(C::Source::RichJoin, Host, HostTarget(), Start) == C::SendRequest);
    assert(offered.Offer(Host, Lobby, Start + 1) == C::JoinLobby);
    assert(offered.LobbyEntered(Lobby, true, M::Pending, Host, Start + 2) == C::AwaitMetadata);
    assert(offered.state() == CS::Joining && offered.host() == Host);
    assert(offered.Welcomed(Host) == C::Ignored);
    assert(offered.LobbyEntered(Lobby, true, M::Match, Host, Start + 3) == C::Ignored);
    assert(offered.LobbyChecked(OtherLobby, M::Different, Start + 3) == C::Ignored);
    assert(offered.LobbyChecked(Lobby, M::Pending, Start + 4) == C::None);
    assert(offered.LobbyChecked(Lobby, M::Match, Start + 1000) == C::HelloReady);
    assert(offered.LobbyChecked(Lobby, M::Different, Start + 1001) == C::Ignored);
    assert(offered.Welcomed(Host) == C::None && offered.state() == CS::Linked);

    C mismatch = ReadyClient();
    assert(mismatch.Accept(C::Source::RichJoin, Host, HostTarget(), Start) == C::SendRequest);
    assert(mismatch.Offer(Host, Lobby, Start) == C::JoinLobby);
    assert(mismatch.LobbyEntered(Lobby, true, M::Pending, Host, Start) == C::AwaitMetadata);
    assert(mismatch.LobbyChecked(Lobby, M::Different, Start + 1) == C::Teardown);
    assert(mismatch.state() == CS::Declined && mismatch.why() == Why::NotG3T);

    // Pre-join lobby invites, command lines and manual joins use the same pending policy.
    for (auto source : {C::Source::LobbyJoin, C::Source::CommandLine, C::Source::Manual}) {
        C direct = ReadyClient();
        assert(direct.Accept(source, Host, LobbyTarget(), Start) == C::InspectLobby);
        assert(direct.LobbyChecked(Lobby, M::Pending, Start) == C::None);
        assert(direct.LobbyChecked(OtherLobby, M::Match, Start + 1) == C::Ignored);
        assert(direct.Tick(Start + LobbyMetadataWaitMs - 2) == C::None);
        assert(direct.LobbyChecked(Lobby, M::Match, Start + LobbyMetadataWaitMs - 1) == C::JoinLobby);
        assert(direct.LobbyEntered(Lobby, true, M::Pending, Host, Start + LobbyMetadataWaitMs) == C::AwaitMetadata);
        assert(direct.LobbyChecked(Lobby, M::Match, Start + LobbyMetadataWaitMs + 1) == C::HelloReady);
        assert(direct.Welcomed(Host) == C::None);
    }

    C expired = ReadyClient();
    assert(expired.Accept(C::Source::RichJoin, Host, HostTarget(), Start) == C::SendRequest);
    assert(expired.Offer(Host, Lobby, Start) == C::JoinLobby);
    assert(expired.LobbyEntered(Lobby, true, M::Pending, Host, Start) == C::AwaitMetadata);
    assert(expired.LobbyChecked(Lobby, M::Pending, Start + LobbyMetadataWaitMs - 1) == C::None);
    assert(expired.Tick(Start + LobbyMetadataWaitMs - 1) == C::None);
    assert(expired.Tick(Start + LobbyMetadataWaitMs) == C::Teardown);
    assert(expired.state() == CS::Declined && expired.why() == Why::MetadataTimeout);
    assert(!expired.host() && !expired.lobby());
    assert(expired.LobbyChecked(Lobby, M::Match, Start + LobbyMetadataWaitMs + 1) == C::Ignored);
    assert(expired.Accept(C::Source::RichJoin, Host, HostTarget(), Start + LobbyMetadataWaitMs + 2) == C::SendRequest);
    assert(expired.Offer(Host, OtherLobby, Start + LobbyMetadataWaitMs + 3) == C::JoinLobby);
    assert(expired.LobbyChecked(Lobby, M::Match, Start + LobbyMetadataWaitMs + 4) == C::Ignored);
    assert(expired.LobbyEntered(Lobby, true, M::Match, Host, Start + LobbyMetadataWaitMs + 4) == C::Ignored);
    assert(expired.LobbyEntered(OtherLobby, true, M::Match, Host, Start + LobbyMetadataWaitMs + 4) == C::HelloReady);

    C late = ReadyClient();
    assert(late.Accept(C::Source::LobbyJoin, Host, LobbyTarget(), Start) == C::InspectLobby);
    assert(late.LobbyChecked(Lobby, M::Pending, Start) == C::None);
    assert(late.LobbyChecked(Lobby, M::Match, Start + LobbyMetadataWaitMs) == C::Teardown);
    assert(late.why() == Why::MetadataTimeout);

    constexpr uint32_t wrapStart = UINT32_MAX - 1000;
    C wrap = ReadyClient();
    assert(wrap.Accept(C::Source::LobbyJoin, Host, LobbyTarget(), wrapStart) == C::InspectLobby);
    assert(wrap.LobbyChecked(Lobby, M::Pending, wrapStart) == C::None);
    assert(wrap.Tick(wrapStart + LobbyMetadataWaitMs - 1) == C::None);
    assert(wrap.Tick(wrapStart + LobbyMetadataWaitMs) == C::Teardown);
    assert(wrap.why() == Why::MetadataTimeout);
}

void SupersessionAndWrap()
{
    C commandLine = ReadyClient();
    assert(commandLine.Accept(C::Source::CommandLine, 0, HostTarget(), Start) == C::SendRequest);
    assert(commandLine.host() == Host && commandLine.inviter() == 0);
    C anonymousLobby = ReadyClient();
    assert(anonymousLobby.Accept(C::Source::CommandLine, 0, LobbyTarget(), Start) == C::InspectLobby);
    assert(anonymousLobby.lobby() == Lobby && anonymousLobby.inviter() == 0);

    C client = ReadyClient();
    assert(client.LobbyInvite(Host, Lobby) == C::None);
    assert(client.lobbyInvitedBy() == Host && client.invitedLobby() == Lobby);
    assert(client.Accept(C::Source::LobbyJoin, Host, LobbyTarget(), Start) == C::InspectLobby);
    assert(!client.lobbyInvitedBy() && !client.invitedLobby());
    assert(client.Accept(C::Source::LobbyJoin, Host, LobbyTarget(), Start) == C::Ignored);
    assert(client.Accept(C::Source::RichJoin, Other, HostTarget(Other), Start) ==
           (C::Teardown | C::SendRequest));
    assert(client.host() == Other && client.inviter() == Other && !client.lobby());
    assert(client.Accept(C::Source::RichJoin, Other, HostTarget(Other), Start) == C::Ignored);
    assert(client.Accept(C::Source::LobbyJoin, Host, LobbyTarget(), Start) ==
           (C::Teardown | C::InspectLobby));
    assert(client.lobby() == Lobby && client.inviter() == Host && !client.host());

    constexpr uint32_t wrapStart = UINT32_MAX - 1000;
    C wrap = ReadyClient();
    assert(wrap.Accept(C::Source::Manual, Host, HostTarget(), wrapStart) == C::SendRequest);
    assert(!wrap.ResendDue(wrapStart + RequestResendMs - 1));
    assert(wrap.ResendDue(wrapStart + RequestResendMs));
    assert(wrap.Tick(wrapStart + RequestWaitMs - 1) == C::None);
    assert(wrap.Tick(wrapStart + RequestWaitMs) == C::Teardown && wrap.why() == Why::NoAnswer);
    H host;
    assert(host.Ready(Mode::Invite, wrapStart) == H::None);
    assert(host.Request(Join(), Protocol, wrapStart) == H::CreateLobby);
    assert(host.Tick(wrapStart + CreateWaitMs - 1) == H::None);
    assert(host.Tick(wrapStart + CreateWaitMs) == (H::SendDecline | H::Teardown));
    assert(host.declineWhy() == Why::HostError && host.declineArg() == 0);
    assert(host.Request(Join(), Protocol, wrapStart) == H::CreateLobby);
    assert(host.LobbyCreated(false, -1, wrapStart) == H::SendDecline);
    assert(host.declineWhy() == Why::HostError && host.declineArg() == 0 && host.partner() == 0);
}
}

int main()
{
    assert(!LinkEndedImmediately(false, 0));
    assert(!LinkEndedImmediately(false, ImmediateLinkEndMs - 1));
    assert(LinkEndedImmediately(true, 0));
    assert(LinkEndedImmediately(true, ImmediateLinkEndMs - 1));
    assert(!LinkEndedImmediately(true, ImmediateLinkEndMs));
    assert(!LinkEndedImmediately(true, ImmediateLinkEndMs + 1));
    assert(ImmediateLinkEndMs == 5000);
    ClientTable();
    HostTable();
    ConnectStrings();
    Frames();
    RelayPrefixCollision();
    Simulation();
    MetadataReplication();
    SupersessionAndWrap();
#ifdef TEST_KNOWN_TRUNCATED_TOKEN
    TruncatedToken();
#endif
    std::cout << "Parser, wire and end-to-end scenarios passed\n";
    return 0;
}
