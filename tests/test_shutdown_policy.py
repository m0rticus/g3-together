import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


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
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required for engine-free shutdown fixture")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "shutdown_policy.exe"
            subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(ROOT / "tests" / "shutdown_policy_test.cpp"), "-o", str(executable)],
                check=True, capture_output=True, text=True,
            )
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)

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
        source = (ROOT / "src" / "crashlog.cpp").read_text(encoding="utf-8")
        self.assertNotIn("CancelWatchdog", function(source, "bool Shutdown()"))
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
