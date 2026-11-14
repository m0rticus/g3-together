import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))
from _g3copy import win32_tool_environment


def compile_cpp17(sources, output, folder, includes=()):
    compiler = shutil.which("clang++") or shutil.which("g++")
    if compiler:
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   *(f"-I{inc}" for inc in includes), *map(str, sources), "-o", str(output)]
        subprocess.run(command, check=True, cwd=folder, capture_output=True, text=True)
        return os.environ.copy()
    env = win32_tool_environment(folder)
    cl = shutil.which("cl", path=env.get("PATH"))
    if not cl:
        raise unittest.SkipTest("C++17 compiler required")
    command = [cl, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
               *(f"/I{inc}" for inc in includes), *map(str, sources),
               f"/Fe:{output}", f"/Fo:{folder}\\"]
    subprocess.run(command, check=True, cwd=folder, env=env, capture_output=True, text=True)
    return env


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[brace:end]


class ShutdownPolicyTests(unittest.TestCase):
    def test_production_wait_classification_and_exit_thresholds_without_engine(self):
        with tempfile.TemporaryDirectory() as folder:
            folder_path = Path(folder)
            executable = folder_path / "shutdown_policy.exe"
            env = compile_cpp17([ROOT / "tests" / "shutdown_policy_test.cpp"], executable, folder_path)
            subprocess.run([str(executable)], check=True, cwd=folder_path, env=env, capture_output=True, text=True)

    def test_config_thread_local_snapshot_and_long_line_parsing_without_engine(self):
        harness = r"""
#include "config.h"
#include "options.h"
#include "symbols.h"
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

static std::string g_gameDir;
static std::atomic<bool> g_hostOverride{false};

namespace g3t {
void Log(char const*) {}
void Logf(char const*, ...) {}
namespace symbols {
bool GameDir(char (&out)[260]) {
    if (g_gameDir.empty() || g_gameDir.size() >= sizeof(out)) return false;
    strcpy_s(out, g_gameDir.c_str());
    return true;
}
}
namespace options {
void ApplyMod(Config& cfg) {
    if (g_hostOverride.load(std::memory_order_acquire)) {
        cfg.friendlyFire = 75;
        cfg.npcDamage = 2.5f;
        cfg.npcHp = 3.0f;
    }
}
}
}

int main(int argc, char** argv) {
    assert(argc == 2);
    g_gameDir = argv[1];
    const std::string iniPath = g_gameDir + "\\g3together.ini";
    {
        std::ofstream out(iniPath, std::ios::binary);
        out << "role = client\n"
            << "session_id = test-room\n"
            << "friendly_fire = 15\n"
            << "npc_damage = 1.25\n"
            << "npc_hp = 1.5\n"
            << "legacy_companion_owner = 76561198011111111 Diego#1\n"
            << "legacy_companion_owner = 76561198022222222 Milten#2\n"
            << "# " << std::string(300, 'x') << "\nrole = host\n";
        // Notice the 300-char comment line ends with '\n' followed by a real 'role = host' line,
        // while an overlong setting line without newline before 255 chars must be ignored completely:
        out << "look = " << std::string(280, 'A') << "role = host\n";
    }
    const auto& loaded = g3t::LoadConfig();
    assert(loaded.host == true);
    assert(loaded.friendlyFire == 15);
    assert(std::fabs(loaded.npcDamage - 1.25f) < 1e-5f);
    assert(std::fabs(loaded.npcHp - 1.5f) < 1e-5f);
    assert(loaded.look[0] == '\0');
    assert(loaded.legacyCompanionOwners.size() == 2);
    assert(loaded.legacyCompanionOwners.at("Diego#1") == 76561198011111111ULL);
    assert(loaded.legacyCompanionOwners.at("Milten#2") == 76561198022222222ULL);

    // Verify local balance values are restored when host override turns off, and concurrent threads do not race.
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto& cfg = g3t::GetConfig();
                assert(cfg.friendlyFire == 15 || cfg.friendlyFire == 75);
                assert(cfg.legacyCompanionOwners.at("Diego#1") == 76561198011111111ULL);
            }
        });
    }
    for (int i = 0; i < 200; ++i) {
        g_hostOverride.store((i % 2) == 0, std::memory_order_release);
        const auto& cfg = g3t::GetConfig();
        if (i % 2 == 0) {
            assert(cfg.friendlyFire == 75);
            assert(std::fabs(cfg.npcDamage - 2.5f) < 1e-5f);
            assert(std::fabs(cfg.npcHp - 3.0f) < 1e-5f);
        } else {
            assert(cfg.friendlyFire == 15);
            assert(std::fabs(cfg.npcDamage - 1.25f) < 1e-5f);
            assert(std::fabs(cfg.npcHp - 1.5f) < 1e-5f);
        }
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& w : workers) w.join();
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as folder:
            folder_path = Path(folder)
            driver = folder_path / "config_driver.cpp"
            driver.write_text(harness, encoding="utf-8")
            executable = folder_path / "config_driver.exe"
            env = compile_cpp17(
                [driver, ROOT / "src" / "config.cpp"],
                executable,
                folder_path,
                includes=[ROOT / "src"],
            )
            subprocess.run([str(executable), str(folder_path)], check=True, cwd=folder_path,
                           env=env, capture_output=True, text=True)

    def test_cleanup_precedes_exit_eligibility_without_changing_menu_leave(self):
        source = (ROOT / "src" / "Script_G3Together.cpp").read_text(encoding="utf-8")
        pre = function(source, "bEResult PreShutdown() override")
        ordered = ["ai::BeginShutdown()", "crashlog::StartWatchdog()", "persist::PrepareShutdown()",
                   "ForgetWorld()", "net::Stop()", "ai::RestorePatches()", "crashlog::Shutdown()",
                   'Stage("pre-shutdown done")', "CompletePreShutdown("]
        positions = [pre.index(item) for item in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("savesQuiescent && threadsJoined && !patches.failed", pre)
        for callback in ("bEResult WorldDeactivate() override", "bEResult WorldActivate() override"):
            body = function(source, callback)
            self.assertNotIn("StartWatchdog", body)
            self.assertNotIn("CompletePreShutdown", body)
            self.assertNotIn("PrepareShutdown", body)

    def test_native_writers_are_not_interrupted_and_staging_uses_existing_failure_path(self):
        source = (ROOT / "src" / "persist.cpp").read_text(encoding="utf-8")
        body = function(source, "bool PrepareShutdown()")
        self.assertLess(body.index("nativeLoadDepth || nativeSaveDepth"), body.index('CancelSave("shutdown")'))
        self.assertIn("return false;", body)
        self.assertIn("StopSaveTimer()", body)
        self.assertIn("CleanupStaging()", body)
        self.assertIn("RetryCleanup(", body)
        self.assertIn("requestPending = false", body)
        self.assertNotIn("Commit", body)
        self.assertNotIn("SaveDetour", body)

    def test_monitor_survives_our_shutdown_and_unknown_hangs_keep_diagnostics(self):
        crashlog = (ROOT / "src" / "crashlog.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "shutdown_watchdog.cpp").read_text(encoding="utf-8")
        self.assertNotIn("CancelWatchdog", function(crashlog, "bool Shutdown()"))
        monitor = function(source, "DWORD WINAPI WatchdogThread(LPVOID)")
        self.assertIn("finished && fastExitReady.load() && SampleGraphicsWait", monitor)
        self.assertLess(monitor.index("exit.Observe("), monitor.index("TerminateProcess("))
        self.assertIn("TerminateProcess(GetCurrentProcess(), 0)", monitor)
        self.assertNotIn("ExitProcess(", monitor)
        self.assertIn("shutdown::DiagnosticMs", monitor)
        self.assertIn("WriteWatchdogDump(dmp)", monitor)
        self.assertIn("WriteWatchdogText(txt)", monitor)
        self.assertIn("fastExitReady.store(false)", monitor)
        sample = function(source, "bool SampleGraphicsWait(")
        self.assertLess(sample.index("ResumeThread(thread)"), sample.index("Logf("))
        self.assertNotIn("TerminateProcess", sample)
        self.assertIn("ReadProcessMemory", sample)
        self.assertIn("context.Eip == graphicsSites.nativeWait + 0xc", sample)
        self.assertIn("shutdown::WaitingObject(", sample)
        self.assertIn("objectWords[0x48 / 4]", sample)
        self.assertIn("main thread resume failed", sample)
        self.assertIn("!suspended && captured", sample)
        for error in ("OpenThread failed", "SuspendThread failed", "GetThreadContext failed", "ResumeThread failed"):
            self.assertIn(error, sample)
        self.assertIn("candidate_word", source)
        self.assertIn("GRAPHICS_WAIT matched=", source)

    def test_native_device_release_signature_matches_operator_supplied_cp_image(self):
        # Inspect bytes only. No engine, Steam, log polling or process execution.
        path = os.environ.get("G3T_GAME_DLL")
        if not path:
            self.skipTest("G3T_GAME_DLL required for CP image byte fixture")
        image = Path(path).with_name("Engine.dll").read_bytes()
        nt = struct.unpack_from("<I", image, 0x3C)[0]
        count = struct.unpack_from("<H", image, nt + 6)[0]
        optional_size = struct.unpack_from("<H", image, nt + 20)[0]
        sections = nt + 24 + optional_size
        rva = 0x9B127
        for index in range(count):
            at = sections + index * 40
            size, address, raw_size, raw = struct.unpack_from("<IIII", image, at + 8)
            if address <= rva < address + max(size, raw_size):
                offset = raw + rva - address
                self.assertEqual(image[offset:offset + 15], bytes.fromhex("8b46088b088b510850ffd285c077f1"))
                break
        else:
            self.fail("CP device Release site not in an image section")


if __name__ == "__main__":
    unittest.main()
