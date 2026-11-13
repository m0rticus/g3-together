# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise measured transport acceptance without a game or a log parser."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "realism_runner_assertions", ROOT / "tools" / "run_steam_realism.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class RealismAssertionTests(unittest.TestCase):
    def fixture(self):
        host_metrics = {
            "schema": 1, "role": "host", "seeded_bypass": False,
            "ready": True, "host_link_observed": True, "enet_connected": True,
            "sampled_before_stop": True, "fake_stats_available": True,
            "fake_config_available": True,
            "fake_config": {
                "profile": "steam-typical", "metadata_delay_ms": 300,
                "physical_rate_bps": 262144, "nodelay_policy": 0,
            },
            "fake_stats": {"callback504_count": 1, "result25_count": 1},
        }
        client_metrics = copy.deepcopy(host_metrics)
        client_metrics.update({
            "role": "client", "lobby_to_link_ms": 340,
            "lobby_enter_callback_observed": True,
            "metadata_empty_at_lobby_enter": True,
            "metadata_update_callback_observed": True,
            "enet_connect_count": 2, "steam_link_count": 2,
            "recovered_echo_samples": 5, "rejoin_count": 1,
            "link_lost_count": 1, "session_failed_callbacks": 1,
        })
        echo = {"samples": 20, "p50_ms": 40, "p95_ms": 60, "max_ms": 90}
        merged = {
            "stress_offer_duration_ms": 60000,
            "stress_offer_bps_observed": 400000,
            "channel0_app_offered_bytes": 24000000,
            "product_queue_bytes": 1024, "product_queue_oldest_ms": 10,
            "product_queue_slope_bytes_per_second": 0,
            "rtt_enet_p95_ms": 60,
            "channel1_echo": copy.deepcopy(echo),
            "player_opcode3_echo": copy.deepcopy(echo),
            "exact_reliable_payloads_sent": 20,
            "exact_reliable_payloads_received": 20,
            "exact_reliable_payload_mismatch": 0,
            "steam_plain_tx_bytes": 24000000,
            "steam_wire_tx_bytes": 12000000,
            "compression_ratio_plain_over_wire": 2,
        }
        host_metrics.update(copy.deepcopy(merged))
        client_metrics.update(copy.deepcopy(merged))
        host = {"returncode": 0, "timed_out": False, "metrics": host_metrics}
        client = {"returncode": 0, "timed_out": False, "metrics": client_metrics}
        return host, client, merged

    def evaluate(self, name, fixture):
        return RUNNER.evaluate_case(name, *fixture)

    def test_saturation_uses_measured_bounds_not_an_unconditional_failure(self):
        self.assertEqual(self.evaluate("saturation", self.fixture())[0], "PASS")

    def test_saturation_requires_the_full_actual_load(self):
        fixture = self.fixture()
        fixture[2]["stress_offer_duration_ms"] = 59000
        self.assertEqual(self.evaluate("saturation", fixture)[0], "NOT_REACHED")
        fixture = self.fixture()
        fixture[2]["stress_offer_bps_observed"] = 0
        self.assertEqual(self.evaluate("saturation", fixture)[0], "NOT_REACHED")

    def test_saturation_rejects_measured_queue_growth(self):
        fixture = self.fixture()
        fixture[2]["product_queue_slope_bytes_per_second"] = 100000
        self.assertEqual(self.evaluate("saturation", fixture)[0], "FAIL")

    def test_saturation_rejects_critical_latency(self):
        fixture = self.fixture()
        fixture[2]["player_opcode3_echo"]["p95_ms"] = 2000
        self.assertEqual(self.evaluate("saturation", fixture)[0], "FAIL")

    def test_saturation_requires_actual_result25(self):
        fixture = self.fixture()
        fixture[0]["metrics"]["fake_stats"]["result25_count"] = 0
        fixture[1]["metrics"]["fake_stats"]["result25_count"] = 0
        self.assertEqual(self.evaluate("saturation", fixture)[0], "FAIL")

    def test_steady_load_is_separate_and_does_not_require_overload(self):
        fixture = self.fixture()
        for peer in fixture[:2]:
            peer["metrics"]["app_bps_requested"] = 212659
            peer["metrics"]["fake_stats"]["result25_count"] = 0
        fixture[2].update({
            "stress_offer_bps_observed": 212659,
            "channel0_app_offered_bytes": 212659 * 60,
        })
        self.assertEqual(self.evaluate("steady-load", fixture)[0], "PASS")
        self.assertEqual(self.evaluate("saturation", fixture)[0], "NOT_REACHED")
        fixture[2]["product_queue_slope_bytes_per_second"] = 100000
        self.assertEqual(self.evaluate("steady-load", fixture)[0], "FAIL")

    def test_steady_load_requires_an_explicit_rate(self):
        with self.assertRaisesRegex(ValueError, "explicit offered rate"):
            RUNNER.peer_args("host", "steady-load", 60, 3000)
        args = RUNNER.peer_args("host", "steady-load", 60, 3000, 212659)
        self.assertEqual(args[-2:], ["--app-bps", "212659"])

    def test_metadata_requires_empty_keys_after_actual_entry(self):
        self.assertEqual(self.evaluate("metadata", self.fixture())[0], "PASS")
        fixture = self.fixture()
        fixture[1]["metrics"]["metadata_empty_at_lobby_enter"] = False
        self.assertEqual(self.evaluate("metadata", fixture)[0], "FAIL")

    def test_metadata_cannot_substitute_a_lobby_id_for_a_callback(self):
        fixture = self.fixture()
        fixture[1]["metrics"]["lobby_enter_callback_observed"] = False
        self.assertEqual(self.evaluate("metadata", fixture)[0], "NOT_REACHED")

    def test_rejoin_requires_a_second_connection_and_payload(self):
        self.assertEqual(self.evaluate("rejoin", self.fixture())[0], "PASS")
        fixture = self.fixture()
        fixture[1]["metrics"]["enet_connect_count"] = 1
        self.assertEqual(self.evaluate("rejoin", fixture)[0], "FAIL")
        fixture = self.fixture()
        fixture[1]["metrics"]["recovered_echo_samples"] = 0
        self.assertEqual(self.evaluate("rejoin", fixture)[0], "FAIL")

    def test_bad_requires_observed_loss_failure_and_recovery(self):
        fixture = self.fixture()
        for peer in fixture[:2]:
            peer["metrics"]["fake_config"].update({
                "physical_rate_bps": 102400, "loss_permille": 20,
                "jitter_ms": 150, "queue_delay_ms": 2000,
            })
        self.assertEqual(self.evaluate("bad", fixture)[0], "FAIL")
        fixture[0]["metrics"]["fake_stats"]["dropped_data_packets"] = 1
        fixture[0]["metrics"]["fake_stats"].update({
            "queue_delay_events": 1, "max_queue_delay_ms": 2000,
        })
        self.assertEqual(self.evaluate("bad", fixture)[0], "PASS")
        fixture[1]["metrics"]["session_failed_callbacks"] = 0
        self.assertEqual(self.evaluate("bad", fixture)[0], "FAIL")

    def test_compressible_requires_load_duration_not_just_a_ratio(self):
        self.assertEqual(self.evaluate("compressible", self.fixture())[0], "PASS")
        fixture = self.fixture()
        fixture[2]["stress_offer_duration_ms"] = 100
        self.assertEqual(self.evaluate("compressible", fixture)[0], "FAIL")

    def test_compression_is_byte_weighted_not_the_best_direction(self):
        host, client, _ = self.fixture()
        host["metrics"].update({
            "steam_plain_tx_bytes": 24000000,
            "steam_wire_tx_bytes": 24000000,
            "compression_ratio_plain_over_wire": 1,
        })
        client["metrics"].update({
            "steam_plain_tx_bytes": 2000,
            "steam_wire_tx_bytes": 1000,
            "compression_ratio_plain_over_wire": 2,
        })
        merged = RUNNER.merge_metrics(host, client)
        self.assertEqual(merged["steam_plain_tx_bytes"], 24002000)
        self.assertEqual(merged["steam_wire_tx_bytes"], 24001000)
        self.assertAlmostEqual(
            merged["compression_ratio_plain_over_wire"], 24002000 / 24001000)
        self.assertEqual(
            self.evaluate("compressible", (host, client, merged))[0], "FAIL")

    def test_invalid_host_wire_counter_cannot_pass_compression(self):
        fixture = self.fixture()
        fixture[0]["metrics"]["steam_wire_tx_bytes"] = float("nan")
        self.assertEqual(self.evaluate("compressible", fixture)[0], "FAIL")

    def test_invalid_schema_and_seeded_shortcuts_are_rejected(self):
        fixture = self.fixture()
        fixture[0]["metrics"]["schema"] = 2
        self.assertEqual(self.evaluate("connect", fixture)[0], "FAIL")
        fixture = self.fixture()
        fixture[0]["metrics"]["seeded_bypass"] = True
        self.assertEqual(self.evaluate("connect", fixture)[0], "FAIL")

    def test_absent_retained_queue_metrics_cannot_become_zero_in_a_merge(self):
        fixture = self.fixture()
        del fixture[0]["metrics"]["product_queue_bytes"]
        self.assertEqual(self.evaluate("saturation", fixture)[0], "FAIL")
        fixture = self.fixture()
        fixture[1]["metrics"]["rtt_enet_p95_ms"] = float("nan")
        self.assertEqual(self.evaluate("compressible", fixture)[0], "FAIL")

    def test_compression_cannot_hide_an_unlimited_physical_profile(self):
        fixture = self.fixture()
        fixture[0]["metrics"]["fake_config"]["physical_rate_bps"] = 0
        self.assertEqual(self.evaluate("compressible", fixture)[0], "FAIL")

    def test_nan_latency_cannot_satisfy_a_bound(self):
        fixture = self.fixture()
        fixture[2]["player_opcode3_echo"]["p95_ms"] = float("nan")
        self.assertEqual(self.evaluate("saturation", fixture)[0], "FAIL")

    def test_shutdown_statistics_cannot_substitute_for_live_queue_metrics(self):
        fixture = self.fixture()
        fixture[0]["metrics"]["sampled_before_stop"] = False
        self.assertEqual(self.evaluate("connect", fixture)[0], "FAIL")

    def test_compressed_classifier_failure_requires_actual_native_output(self):
        fixture = self.fixture()
        fixture[2]["native_connect_header_size"] = 44
        self.assertEqual(self.evaluate("compressed-connect", fixture)[0], "NOT_REACHED")
        fixture[2].update({
            "native_outbound_connect_frames": 1,
            "native_outbound_compressed_connect_frames": 1,
            "native_observed_connect_header": 48,
        })
        self.assertEqual(self.evaluate("compressed-connect", fixture)[0], "FAIL")

    def test_native_connect_without_verify_is_an_observed_failure(self):
        fixture = self.fixture()
        fixture[2].update({
            "native_connect_header_size": 48,
            "native_verify_header_size": 44,
            "native_outbound_connect_frames": 1,
            "native_outbound_compressed_connect_frames": 1,
            "native_observed_connect_header": 48,
        })
        self.assertEqual(self.evaluate("compressed-connect", fixture)[0], "FAIL")

    def test_empty_delivered_metadata_without_recovery_is_a_failure(self):
        fixture = self.fixture()
        fixture[1]["metrics"]["metadata_update_callback_observed"] = False
        self.assertEqual(self.evaluate("metadata", fixture)[0], "FAIL")

    def test_initial_connection_without_recovery_is_a_failure(self):
        fixture = self.fixture()
        fixture[1]["metrics"]["enet_connected"] = False
        self.assertEqual(self.evaluate("rejoin", fixture)[0], "FAIL")
        self.assertEqual(self.evaluate("bad", fixture)[0], "FAIL")

    def test_pair_does_not_restore_inherited_fake_settings_after_cleanup(self):
        captured = []

        class Process:
            returncode = 0

            def __init__(self, command, **kwargs):
                captured.append(kwargs["env"])
                output = Path(command[command.index("--out") + 1])
                output.write_text(json.dumps({"schema": 1}), encoding="utf-8")

            def wait(self, timeout):
                return 0

            def poll(self):
                return 0

        with tempfile.TemporaryDirectory(prefix="g3t-env-unit-") as folder, \
                patch.dict(os.environ, {"G3T_FAKESTEAM_TEST_CLOCK": "1"}), \
                patch.object(RUNNER.subprocess, "Popen", Process):
            root = Path(folder)
            RUNNER.run_pair(root / "peer.exe", {
                "G3T_REALISM_FAKE_DLL": str(root / "fake.dll"),
                "G3T_FAKESTEAM_TEST_CLOCK": "1",
            }, "connect", 3, root, root / "case", 3000)
        self.assertEqual(len(captured), 2)
        for environment in captured:
            self.assertNotIn("G3T_FAKESTEAM_TEST_CLOCK", environment)
            self.assertEqual(environment["G3T_FAKESTEAM_LATENCY_MS"], "20")
        self.assertNotEqual(captured[0]["G3T_FAKESTEAM_PORT"],
                            captured[1]["G3T_FAKESTEAM_PORT"])


if __name__ == "__main__":
    unittest.main()
