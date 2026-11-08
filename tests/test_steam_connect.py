import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class SteamConnectTests(unittest.TestCase):
    def test_production_policy(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        executable = build / f"steam-connect-test-{os.getpid()}.exe"
        try:
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       str(ROOT / "tests" / "steam_connect_test.cpp"), "-o", str(executable)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        finally:
            executable.unlink(missing_ok=True)

    def test_wire_and_header_dependencies(self):
        self.assertIn("Protocol = 77", (ROOT / "src" / "saving_policy.h").read_text(encoding="utf-8"))
        header = (ROOT / "src" / "steam_connect.h").read_text(encoding="utf-8")
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
        return (ROOT / name).read_text(encoding="utf-8").replace("\r\n", "\n")

    @staticmethod
    def between(text, start, end):
        return text.split(start, 1)[1].split(end, 1)[0]

    def test_source_contracts(self):
        steam = self.source("src/steam.cpp")
        net = self.source("src/net.cpp")
        config_h = self.source("src/config.h")
        config_cpp = self.source("src/config.cpp")
        # Nothing connects by itself: the legacy discovery exists only behind connection = auto.
        self.assertIn("bool connectionAuto = false;", config_h)
        self.assertIn('"connection"', config_cpp)
        start = self.between(steam, "bool Start(bool host, unsigned short port)", "unsigned short RelayPort()")
        self.assertNotIn("CreateLobby", start)  # the host opens no lobby at startup (only hostFlow.Ready may ask for it in auto mode)
        self.assertIn("hostFlow.Ready(connectionMode", start)
        self.assertIn("StartupAccept();", start)
        pump = self.between(steam, "void PumpOnce()\n{\n    if (!ready) return;", "void Pump() {")
        self.assertIn("if (connectionMode == connect::Mode::Auto) Search();", pump)
        self.assertIn("else ClientTick();", pump)
        self.assertIn("connectionMode == connect::Mode::Auto && Now() - lastCreate > 15000", pump)
        search = self.between(steam, "void Search()\n{", "bool Fail(char const* text)")
        self.assertEqual(steam.count("api.FriendGamePlayed("), 1)  # the friends-list scan lives in Search only
        self.assertIn("api.FriendGamePlayed(", search)
        self.assertEqual(steam.count("Search();"), 1)
        # A join request leaves the client only from an accepted invite (flow effect) or the bounded resend of that request.
        self.assertEqual(steam.count("SendJoinRequest(false)"), 1)
        self.assertEqual(steam.count("SendJoinRequest(true)"), 1)
        after_client = self.between(steam, "void AfterClient(", "void SendOffer(")
        self.assertIn("if (fx & F::SendRequest) SendJoinRequest(false);", after_client)
        # Callbacks: 337 Join Game and 333 lobby invites are the accepts, 503 is only a note, the command line is read once at startup.
        for line in ("CbLobbyJoinRequested = 333", "CbRichJoinRequested = 337", "CbLobbyInvite = 503"):
            self.assertIn(line, steam)
        self.assertIn("Source::RichJoin", steam)
        self.assertIn("Source::LobbyJoin", steam)
        invite = self.between(steam, "void OnLobbyInvite(", "void OnJoinRequest(")
        self.assertNotIn("Accept(", invite)
        self.assertNotIn("Join(", invite)
        self.assertEqual(steam.count("GetCommandLineA()"), 1)
        # The host stays joinable: rich presence is published once (connect) and never cleared by closing a lobby.
        leave = self.between(steam, "void LeaveLobby()\n{", "// ---- invite-driven connection")
        self.assertNotIn("SetPresence", leave)
        self.assertEqual(leave.count("api.SetRichPresence"), 2)  # only the legacy auto-mode lobby string is cleared with its lobby
        self.assertIn("if (isHost && connectionMode == connect::Mode::Auto) {", leave)
        self.assertLess(leave.index("connectionMode == connect::Mode::Auto"), leave.index("api.SetRichPresence"))
        self.assertIn('SetPresence("connect", presenceConnect)', steam)
        self.assertIn("connect::FormatConnect(presenceConnect", steam)
        # Friends only, one partner at a time, and the lobby is created on demand for a valid join request.
        self.assertIn("api.CreateLobby(matchmaking, LobbyFriendsOnly, members)", steam)
        self.assertIn("static_cast<int>(connect::MaxPartners) + 1", steam)
        join = self.between(steam, "void OnJoinRequest(", "void OnNegotiation(")
        self.assertIn("IsFriend(from)", join)
        self.assertIn("hostFlow.Request(join, saving::Protocol", join)
        self.assertIn("hostFlow.Admit(r.id)", steam)
        self.assertIn('accepted = "Steam friend"', steam)
        self.assertEqual(steam.count("IsFriend(a)"), 1)
        # Every step is logged.
        for text in ("connect: command line:", "connect: rich presence", "connect: %s: friend=%llu", "connect: join request from",
                     "connect: lobby create result", "connect: LobbyEnter", "connect: connected: welcomed", "connect: connected: partner",
                     "connect: sent %s to", "connect: waiting for an invite", "connect: lobby offer from", "connect: declining",
                     "connect: session with %llu"):
            self.assertIn(text, steam)
        # ENet: connects only while the Steam link is up, never from Start.
        net_start = self.between(net, "bool Start(bool asHost)", "void Service(")
        self.assertNotIn("enet_host_connect", net_start)
        service = self.between(net, "void Service(", "void Flush()")
        self.assertIn("steamLinked", service)
        self.assertIn("!isHost && !server && !refused && steamLinked", service)
        self.assertIn("steam::HostRelayGeneration()", service)
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


if __name__ == "__main__":
    unittest.main()
