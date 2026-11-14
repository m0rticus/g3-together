# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
class SteamRealismTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.name != "nt":
            raise unittest.SkipTest("real Win32 /Gz peers require Windows")
        fake = os.environ.get("G3T_REALISM_FAKE_DLL", "")
        if not fake:
            raise unittest.SkipTest(
                "missing G3T_REALISM_FAKE_DLL: staged fake Steam DLL is required")
        cls.fake_dll = Path(fake)
        if not cls.fake_dll.is_file():
            raise RuntimeError(
                "G3T_REALISM_FAKE_DLL is configured but does not point to an existing fake DLL")

    def run_cli(self, scenario, seconds=3):
        with tempfile.TemporaryDirectory(prefix="g3t-steam-realism-") as temp:
            output = Path(temp)
            env = os.environ.copy()
            env["G3T_REALISM_FAKE_DLL"] = str(self.fake_dll)
            env["G3T_STEAM_DLL"] = str(self.fake_dll)
            result = subprocess.run([
                sys.executable,
                str(ROOT / "tools" / "run_steam_realism.py"),
                "--fake-dll", str(self.fake_dll),
                "--source-root", str(ROOT),
                "--output-dir", str(output),
                "--scenario", scenario,
                "--seconds", str(seconds),
            ], cwd=ROOT, env=env, capture_output=True,
               text=True, timeout=seconds + 120)
            summary_path = output / "summary.json"
            self.assertTrue(summary_path.is_file(),
                            result.stdout + result.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            return result, summary

    def test_connect_smoke(self):
        result, summary = self.run_cli("connect", 3)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        case = summary["cases"][0]
        self.assertEqual(case["name"], "connect")
        self.assertEqual(case["status"], "PASS", case["reasons"])
        self.assertTrue(case["metrics"].get("host_link_observed") or
                        case["metrics"].get("linked"))
        self.assertTrue(case["metrics"].get("enet_connected"))
        self.assertGreater(
            case["metrics"].get("exact_reliable_payloads_sent", 0), 0)
        self.assertGreater(
            case["metrics"].get("exact_reliable_payloads_received", 0), 0)
        self.assertEqual(
            case["metrics"].get("exact_reliable_payload_mismatch", 0), 0)

    def test_metadata_smoke(self):
        result, summary = self.run_cli("metadata", 3)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        case = summary["cases"][0]
        self.assertEqual(case["status"], "PASS", case["reasons"])
        metrics = case["metrics"]
        self.assertTrue(metrics.get("host_link_observed"))
        self.assertFalse(metrics.get("enet_connected", False))

    def test_compressed_connect_smoke(self):
        result, summary = self.run_cli("compressed-connect", 3)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        case = summary["cases"][0]
        self.assertEqual(case["status"], "PASS", case["reasons"])
        self.assertEqual(
            case["metrics"].get("native_connect_header_size"), 48)
        self.assertEqual(
            case["metrics"].get("native_verify_header_size"), 44)
        self.assertTrue(case["metrics"].get("enet_connected"))
        self.assertGreater(
            case["metrics"].get("native_outbound_frames", 0), 0)
        self.assertGreater(
            case["metrics"].get("native_outbound_connect_frames", 0), 0)
        self.assertGreater(
            case["metrics"].get("native_outbound_verify_frames", 0), 0)

    def test_refusal41_api_and_payload_delivery(self):
        result, summary = self.run_cli("refusal41", 10)
        self.assertIn(result.returncode, (0, 1), result.stdout + result.stderr)
        case = summary["cases"][0]
        metrics = case["metrics"]
        self.assertTrue(metrics["enet_connected"], case["reasons"])
        self.assertGreater(metrics["exact_reliable_payloads_received"], 0)
        self.assertEqual(metrics["exact_reliable_payload_mismatch"], 0)
        for name in ("player_opcode3_echo", "channel1_echo"):
            self.assertGreater(metrics[name]["samples"], 0)
        config = metrics["fake_config"]
        host_config = config.get("host") or {}
        self.assertEqual(host_config.get("nodelay_policy"), 1)
        stats = metrics["fake_stats"]
        host_stats = stats.get("host") or {}
        self.assertGreater(host_stats.get("result41_count", 0), 0)
        if case["status"] != "PASS":
            self.assertEqual(case["status"], "FAIL", case["reasons"])
            self.assertTrue(case["reasons"])
            self.assertTrue(all("latency exceeds" in reason
                                for reason in case["reasons"]), case["reasons"])
            print("SYNTHETIC_REFUSAL41_TIMING: " + "; ".join(case["reasons"]))


if __name__ == "__main__":
    unittest.main()
