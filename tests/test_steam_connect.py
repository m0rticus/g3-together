import re
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

STEAM_CONNECT_TARGET = _testbuild.register_target(
    "steam-connect",
    [_testbuild.unit(
        "tests/steam_connect_test.cpp",
        std="c++17",
        warnings="strict",
        pedantic=True,
    )],
)


class SteamConnectTests(unittest.TestCase):
    def test_production_policy(self):
        result = STEAM_CONNECT_TARGET.run(cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_wire_and_header_dependencies(self):
        self.assertIn("Protocol = 81", _testbuild.src_text("saving_policy.h"))
        header = _testbuild.src_text("steam_connect.h")
        includes = re.findall(r'^\s*#\s*include\s*([<"][^>"]+[>"])', header, re.MULTILINE)
        self.assertEqual([name for name in includes if name.startswith('"')], ['"relaywire.h"'])
        standard = set("""algorithm any array atomic bitset cassert cctype cerrno cfenv cfloat
            charconv chrono cinttypes ciso646 climits clocale cmath codecvt complex condition_variable
            csetjmp csignal cstdarg cstddef cstdint cstdio cstdlib cstring ctgmath ctime cuchar cwchar
            cwctype deque exception execution filesystem forward_list fstream functional future
            initializer_list iomanip ios iosfwd iostream istream iterator limits list locale map memory
            memory_resource mutex new numeric optional ostream queue random ratio regex scoped_allocator
            set shared_mutex sstream stack stdexcept streambuf string string_view strstream system_error
            thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray
            variant vector""".split())
        for name in includes:
            if name.startswith("<"):
                self.assertIn(name[1:-1], standard, name)

    @staticmethod
    def source(name):
        if name.startswith("src/"):
            text = _testbuild.src_text(name[len("src/"):])
        else:
            text = _testbuild.cached_text(ROOT / name)
        return text.replace("\r\n", "\n")

    @classmethod
    def steam_source(cls):
        parts = (
            "src/steam_abi.h",
            "src/steam_runtime.h",
            "src/steam.cpp",
            "src/steam_published.cpp",
            "src/steam_p2p.cpp",
            "src/steam_flow.cpp",
            "src/steam_callbacks.cpp",
            "src/steam_legacy_auto.cpp",
            "src/steam_diagnostics.cpp",
        )
        return "\n".join(cls.source(part) for part in parts)

    @classmethod
    def steam_cpp_source(cls):
        parts = (
            "src/steam.cpp",
            "src/steam_published.cpp",
            "src/steam_p2p.cpp",
            "src/steam_flow.cpp",
            "src/steam_callbacks.cpp",
            "src/steam_legacy_auto.cpp",
            "src/steam_diagnostics.cpp",
        )
        return "\n".join(cls.source(part) for part in parts)
    @staticmethod
    def between(text, start, end):
        return text.split(start, 1)[1].split(end, 1)[0]

    @staticmethod
    def function_body(text, signature):
        idx = text.index(signature)
        brace = text.index("{", idx)
        depth = 0
        for pos in range(brace, len(text)):
            if text[pos] == "{":
                depth += 1
            elif text[pos] == "}":
                depth -= 1
                if depth == 0:
                    return text[idx : pos + 1]
        raise AssertionError(f"unclosed function body for {signature}")

    def test_source_contracts(self):
        steam = self.steam_source()
        net = self.source("src/net.cpp")
        config_h = self.source("src/config.h")
        config_cpp = self.source("src/config.cpp")
        for part in (
            "src/steam_abi.h",
            "src/steam_runtime.h",
            "src/steam.cpp",
            "src/steam_published.cpp",
            "src/steam_p2p.cpp",
            "src/steam_flow.cpp",
            "src/steam_callbacks.cpp",
            "src/steam_legacy_auto.cpp",
            "src/steam_diagnostics.cpp",
        ):
            line_count = len(self.source(part).splitlines())
            self.assertLess(line_count, 1000, f"{part} exceeds 1000 lines ({line_count})")
        # Worker thread never calls non-synchronized GetConfig(); uses thread_local GetLocalConfig().
        self.assertEqual(steam.count("GetConfig()"), 0)
        self.assertGreater(steam.count("GetLocalConfig()"), 0)
        # Nothing connects by itself: the legacy discovery exists only behind connection = auto.
        self.assertIn("bool connectionAuto = false;", config_h)
        self.assertIn('"connection"', config_cpp)
        start = self.function_body(steam, "bool Start(bool host, unsigned short port)")
        self.assertNotIn("CreateLobby", start)  # the host opens no lobby at startup (only hostFlow.Ready may ask for it in auto mode)
        self.assertIn("hostFlow.Ready(connectionMode", start)
        self.assertIn("StartupAccept();", start)
        pump = self.function_body(steam, "void WorkerRuntime::PumpOnce()")
        self.assertIn("if (connectionMode == connect::Mode::Auto) Search();", pump)
        self.assertIn("else ClientTick();", pump)
        self.assertIn("connectionMode == connect::Mode::Auto && Now() - lastCreate > 15000", pump)
        search = self.function_body(steam, "void WorkerRuntime::Search()")
        self.assertEqual(steam.count("api.FriendGamePlayed("), 1)  # the friends-list scan lives in Search only
        self.assertIn("api.FriendGamePlayed(", search)
        self.assertEqual(self.steam_cpp_source().count("Search();"), 1)
        # A join request leaves the client only from an accepted invite (flow effect) or the bounded resend of that request.
        self.assertEqual(steam.count("SendJoinRequest(false)"), 1)
        self.assertEqual(steam.count("SendJoinRequest(true)"), 1)
        after_client = self.function_body(steam, "void WorkerRuntime::AfterClient(")
        self.assertIn("if (fx & F::SendRequest) SendJoinRequest(false);", after_client)
        # Callbacks: 337 Join Game and 333 lobby invites are the accepts, 503 is only a note, the command line is read once at startup.
        for line in ("CbLobbyJoinRequested = 333", "CbRichJoinRequested = 337", "CbLobbyInvite = 503"):
            self.assertIn(line, steam)
        self.assertIn("Source::RichJoin", steam)
        self.assertIn("Source::LobbyJoin", steam)
        invite = self.function_body(steam, "void WorkerRuntime::OnLobbyInvite(")
        self.assertNotIn("Accept(", invite)
        self.assertNotIn("Join(", invite)
        self.assertEqual(steam.count("GetCommandLineA()"), 1)
        # The host stays joinable: rich presence is published once (connect) and never cleared by closing a lobby.
        leave = self.function_body(steam, "void WorkerRuntime::LeaveLobby()")
        self.assertNotIn("SetPresence", leave)
        self.assertEqual(leave.count("api.SetRichPresence"), 2)  # only the legacy auto-mode lobby string is cleared with its lobby
        self.assertIn("if (isHost && connectionMode == connect::Mode::Auto) {", leave)
        self.assertLess(leave.index("connectionMode == connect::Mode::Auto"), leave.index("api.SetRichPresence"))
        self.assertIn('SetPresence("connect", presenceConnect)', steam)
        self.assertIn("connect::FormatConnect(presenceConnect", steam)
        # Friends only, one partner at a time, and the lobby is created on demand for a valid join request;
        # same-partner rejoin rotates relay generation.
        self.assertIn("api.CreateLobby(matchmaking, LobbyFriendsOnly, members)", steam)
        self.assertIn("static_cast<int>(connect::MaxPartners) + 1", steam)
        join = self.function_body(steam, "void WorkerRuntime::OnJoinRequest(")
        self.assertIn("IsFriend(from)", join)
        self.assertIn("hostFlow.Request(join, saving::Protocol", join)
        self.assertIn("RotatePartnerForRejoin(", self.function_body(steam, "void WorkerRuntime::AfterHost("))
        self.assertIn("hostFlow.Admit(r.id)", steam)
        self.assertIn('accepted = "Steam friend"', steam)
        # Every step is logged.
        for text in ("connect: command line:", "connect: rich presence", "connect: %s: friend=%llu", "connect: join request from",
                     "connect: lobby create result", "connect: LobbyEnter", "connect: connected: welcomed", "connect: connected: partner",
                     "connect: sent %s to", "connect: waiting for an invite", "connect: lobby offer from", "connect: declining",
                     "connect: session with %llu"):
            self.assertIn(text, steam)
        # ENet: connects only while the Steam link is up, never from Start.
        net_start = self.between(net, "bool Start(bool asHost)", "void Service(")
        self.assertNotIn("enet_host_connect", net_start)
        net_direct = self.source("src/net_direct.cpp")
        service = self.between(net, "void Service(", "void Flush()")
        self.assertIn("SyncDirectSessions(", service)
        self.assertIn("SyncClientSteamLink(", service)
        self.assertIn("!isHost && !server && !refused && steamLinked", service)
        sync_link = self.function_body(net_direct, "void SyncClientSteamLink(")
        self.assertIn("steamLinked", sync_link)
        self.assertIn("steam::HostRelayGeneration()", sync_link)
        # Nothing reconnects by itself: once ENet was up, losing it ends the Steam link (the player accepts again).
        self.assertIn("steam::RequestClientDisconnect()", service)
        self.assertIn("everConnected && steam::RequestClientDisconnect()", service)
        self.assertIn("disconnectRequested.exchange(false)", pump)
        self.assertIn("OnClientLinkLost(clientFlow.host()", pump)
        self.assertIn("bool RequestClientDisconnect()", steam)
        self.assertIn("left the lobby before saying hello", steam)
        # A stale refusal does not outlive the next attempt.
        persist = self.source("src/persist.cpp")
        self.assertIn("a new invite is a new attempt", persist)
        # The menu says what is going on.
        i18n = self.source("src/i18n.cpp")
        self.assertIn("Invite your partner from the Steam friends list (right-click -> Invite to Game / Join Game)", i18n)
        playing = self.source("docs/PLAYING.md")
        self.assertIn("connection", playing)
        self.assertIn("+g3t_connect", playing)

    def test_steam_config_scopes_and_early_crashlog_install(self):
        steam_cfg = self.source("src/steam_config.h")
        self.assertIn("GlobalScope = 1", steam_cfg)
        self.assertIn("SocketsInterfaceScope = 2", steam_cfg)
        self.assertIn("ListenSocketScope = 3", steam_cfg)
        self.assertIn("ConnectionScope = 4", steam_cfg)
        script_init = self.function_body(self.source("src/Script_G3Together.cpp"), "ScriptInit()")
        self.assertLess(script_init.index("g3t::crashlog::Install();"), script_init.index("g3t::npcplayback::Start("))
        self.assertLess(script_init.index("g3t::crashlog::Install();"), script_init.index("g3t::remote::Start();"))
        steam_start = self.function_body(self.source("src/steam.cpp"), "bool Start(bool host, unsigned short port)")
        self.assertGreaterEqual(steam_start.count("crashlog::RefreshModules();"), 2)
        crashlog = self.source("src/crashlog.cpp")
        for mod in ('"Gothic3Final.exe"', '"steam_api.dll"', '"steamclient.dll"'):
            self.assertIn(mod, crashlog)
        tick = self.function_body(crashlog, "void Tick()")
        self.assertIn("if (!tickModulesRefreshed)", tick)
        self.assertIn("FindModules();", tick)
        take_scripts = self.function_body(crashlog, "void TakeScripts()")
        self.assertIn("FindModules();", take_scripts)

    def test_metadata_replication_paths(self):
        steam = self.steam_source()
        entry = self.function_body(steam, "void WorkerRuntime::OnLobbyEnter(")
        self.assertIn("ReadLobbyMetadata(id)", entry)
        self.assertIn("clientFlow.LobbyEntered(id, joined, metadata", entry)
        self.assertIn('InspectLobby(id, "joined lobby", true)', entry)
        self.assertIn("connect::LobbyMetadata::Pending", entry)
        poll = self.function_body(steam, "void WorkerRuntime::PollLobbyMetadata()")
        self.assertIn("ReadLobbyMetadata(id)", poll)
        self.assertIn("clientFlow.LobbyChecked(id, metadata, now)", poll)
        self.assertIn("LobbyMetadataWaitMs", poll)
        self.assertIn("RequestLobbyData(matchmaking, id)", poll)
        self.assertIn("metadata pending", poll)
        self.assertIn("metadata arrived after", poll)
        callback = self.between(steam, "case CbLobbyDataUpdate:", "case CbLobbyChatUpdate:")
        self.assertIn("a != candidateLobby || a != b", callback)
        self.assertIn("PollLobbyMetadata();", callback)
        pump = self.function_body(steam, "void WorkerRuntime::PumpOnce()")
        self.assertIn("PollLobbyMetadata();", pump)
        after = self.function_body(steam, "void WorkerRuntime::AfterClient(")
        self.assertIn("F::AwaitMetadata", after)
        self.assertIn("F::HelloReady", after)
        self.assertIn("FinishLobbyEntry", after)
        self.assertIn("DeclineMetadataTimeout", self.source("src/i18n.h"))
        self.assertIn("Steam did not deliver the lobby's data in time", self.source("src/i18n.cpp"))
        self.assertIn("Why::MetadataTimeout", self.source("src/lobby.cpp"))

    def test_transport_factory_and_link_loss_diagnostics(self):
        net = self.source("src/net.cpp")
        net_direct = self.source("src/net_direct.cpp")
        steam = self.steam_source()
        start = self.between(net, "bool Start(bool asHost)", "void Service(")
        service = self.between(net, "void Service(", "void Flush()")
        reconnect = self.between(net, "void Reconnect(void (*log)(char const*))", "\n}")
        connect_helper = self.between(net_direct, "void ConnectClientWithDirectFallback(", "void FinishClientConnectAttempt(")
        self.assertIn("CreateTransportHost(&bindAddress, isHost ? MaxPlayers - 1 : 1, createError)", start)
        self.assertIn("FinishClientConnectAttempt(", service)
        self.assertIn("ConnectClientWithDirectFallback(", reconnect)
        self.assertIn("ConnectTransportHost(host, &target)", connect_helper)
        self.assertIn("ConnectTransportHost(host, &direct->verifiedAddress, DirectCookieTag(direct->cookie))", connect_helper)
        self.assertNotIn("enet_host_create(", net + net_direct)
        self.assertNotIn("enet_host_connect(", net + net_direct)
        policy = self.source("src/enet_policy.h")
        for required in ("ChannelDefinitions = 7", "ChannelCount = 8", "Mtu = 1200",
                         "enet_host_create(address, peers, ChannelCount, 0, 0)",
                         "enet_host_compress_with_range_coder(host)", "ENET_SOCKOPT_RCVBUF",
                         "ENET_SOCKOPT_SNDBUF", "SIO_UDP_CONNRESET",
                         "enet_host_connect(host, address, ChannelCount, data)"):
            self.assertIn(required, policy)

        queued = self.function_body(steam, "bool WorkerRuntime::QueueDatagram(")
        classify = queued.split("catch (std::bad_alloc", 1)[0]
        self.assertIn("r.pending.Push(*sendCodec, data, size, Now(), &parsed)", classify)
        self.assertNotIn("Drop(", classify)
        self.assertIn("if (!parsed.valid)", classify)
        self.assertIn("return true;", classify)
        self.assertIn("(compressed ? 16u : 0u) | (parsed.command & 15u)", classify)
        self.assertIn("if (!r.unclassifiedLogged[kind])", classify)
        self.assertIn("r.unclassifiedLogged[kind] = true", classify)
        for required in ("unclassified_enet_bytes=%u", "first_bytes=%s", "action=sent-as-bulk",
                         "first ENet datagram role=%s", "parsed.firstCommand",
                         "EnetCommandHeaderSize(parsed.firstCommand)"):
            self.assertIn(required, classify)
        self.assertIn("unclassifiedLogged[32] = {}", steam)
        self.assertIn("r = Remote{};", self.function_body(steam, "Remote* WorkerRuntime::Get("))

        for required in ("type=connect(%u) data=%u state=%s(%u) channels=%zu elapsed_ms=%u",
                         "type=disconnect(%u) data=%u state=%s(%u) channels=%zu elapsed_ms=%u",
                         "steam::NoteClientEnetState(event.peer->state, event.type, event.data",
                         "steam::RequestHostRelayClose(account, relayGeneration, reason)"):
            self.assertIn(required, net)
        for required in ("connect: client link ended: reason=%s steam_state=%s enet_state=%s(%u)",
                         "elapsed_ms=%u enet_elapsed_ms=%u immediate=%d",
                         "connect: host link ended: peer=%llu reason=%s steam_state=%s elapsed_ms=%u",
                         "stage=%s lobby_callback=%u lobby_flags=0x%x",
                         "r->lobbyChange = state", "CbLobbyChatUpdate = 506",
                         "Drop(r, close.reason)", "connect::LinkEndedImmediately(clientHadLink, elapsed)"):
            self.assertIn(required, steam)

        reset = self.function_body(steam, "void WorkerRuntime::ResetClientEnd()")
        self.assertIn("clientHadLink = clientEndRecorded = clientEndedImmediately = false", reset)
        self.assertIn("clientEndReason[0] = '\\0'", reset)
        after = self.function_body(steam, "void WorkerRuntime::AfterClient(")
        self.assertIn('if (!std::strcmp(event, "accept") && (fx & (F::SendRequest | F::InspectLobby))) ResetClientEnd();', after)
        self.assertIn("clientLinkedAt = Now()", steam)
        lobby = self.source("src/lobby.cpp")
        self.assertIn("state.endedImmediately ? i18n::LobbyLostImmediate : i18n::LobbyLost", lobby)
        self.assertIn("i18n::LobbyLost), name, reason", lobby)
        i18n = self.source("src/i18n.cpp")
        for prefix in ("the connection to %s ended (%s).", "the connection to %s ended immediately after forming (%s)."):
            row = re.search(r'\{L"G3 Together: ' + re.escape(prefix) + r'.*?\},', i18n, re.DOTALL)
            self.assertIsNotNone(row, prefix)
            languages = re.findall(r'L"([^"]*)"', row.group())
            self.assertEqual(len(languages), 3)
            for text in languages:
                self.assertEqual(text.count("%s"), 2, text)


if __name__ == "__main__":
    unittest.main()
