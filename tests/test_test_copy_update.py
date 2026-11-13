from __future__ import annotations

import hashlib
import importlib.util
import os
import struct
import tempfile
import unittest
import json
import subprocess
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


verifier = load_tool("verify_test_copies")
updater = load_tool("update_test_copies")


def synthetic_pe(path: Path, dll_name: str = "g3qa32.dll") -> None:
    data = bytearray(0x1200)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, 0x14C)
    struct.pack_into("<H", data, 0x86, 1)
    struct.pack_into("<H", data, 0x94, 224)

    optional = 0x98
    struct.pack_into("<H", data, optional, 0x10B)
    struct.pack_into("<H", data, optional + 20, 224)
    struct.pack_into("<I", data, optional + 92, 2)
    struct.pack_into("<I", data, optional + 104, 0x1000)
    struct.pack_into("<I", data, optional + 108, 20)

    section = 0x178
    data[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<I", data, section + 8, 0x1000)
    struct.pack_into("<I", data, section + 12, 0x1000)
    struct.pack_into("<I", data, section + 16, 0x1000)
    struct.pack_into("<I", data, section + 20, 0x200)

    descriptor = 0x200
    struct.pack_into("<IIIII", data, descriptor, 0x1030, 0, 0, 0x1040, 0x1030)
    struct.pack_into("<I", data, 0x230, 0x1080)
    struct.pack_into("<I", data, 0x234, 0)

    dll = dll_name.encode("ascii") + b"\0"
    data[0x240:0x240 + len(dll)] = dll
    name = b"SHGetFolderPathA\0"
    data[0x280:0x282] = b"\0\0"
    data[0x282:0x282 + len(name)] = name
    path.write_bytes(data)


class TestCopyUpdateTests(unittest.TestCase):
    def staged_package(self) -> Path:
        package_text = os.environ.get("G3_TEST_UPDATE_PACKAGE")
        if not package_text:
            self.skipTest(
                "optional staged launcher fixtures require G3_TEST_UPDATE_PACKAGE")
        package = Path(package_text)
        self.assertTrue(package.is_dir(), "configured staged package is missing")
        return package

    def test_unconfigured_staged_package_skips_explicitly(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            with self.assertRaises(unittest.SkipTest):
                self.staged_package()

    def make_roots(self, base: Path) -> dict[str, Path]:
        host = base / "host"
        client = base / "client"
        steam = base / "steam"
        host.mkdir()
        client.mkdir()
        steam.mkdir()
        return {"host": host, "client": client, "steam": steam}

    def test_synthetic_pe_import_parser_and_malformed_rva_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "module.dll"
            synthetic_pe(path)
            imports, metadata = verifier.parse_pe(path)
            self.assertIn("SHGetFolderPathA", imports["g3qa32.dll"])
            self.assertFalse(metadata["delay_import"])

            malformed = bytearray(path.read_bytes())
            struct.pack_into("<I", malformed, 0x98 + 104, 0x7FFFFFF0)
            path.write_bytes(malformed)
            with self.assertRaises(ValueError):
                verifier.parse_pe(path)

    def test_shipped_ini_constants_can_be_read_from_ast(self):
        host, client = updater.extract_package_ini()
        self.assertIn("role = host", host)
        self.assertIn("role = client", client)
        self.assertIn("session_id = default", host)

    def test_render_ini_is_explicit_and_preserves_unknown_key(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            existing = base / "g3together.ini"
            existing.write_text(
                "role = client\ntransport = steam\npuppet_template = PC_Hero\npersonal_option = keep\n",
                encoding="utf-8",
            )
            _host_template, client_template = updater.extract_package_ini()
            rendered = updater.render_ini(
                client_template,
                existing,
                "client",
                "pair-red",
            ).decode("utf-8")
            self.assertIn("role = client", rendered)
            self.assertIn("transport = steam", rendered)
            self.assertIn("connection = invite", rendered)
            self.assertIn("session_id = pair-red", rendered)
            self.assertIn("language = auto", rendered)
            self.assertIn("personal_option = keep", rendered)
            self.assertNotIn("role = host", rendered)

    def test_manifest_traversal_is_rejected(self):
        for relative in (
            "../g3together.ini",
            r"..\g3together.ini",
            "/absolute/g3together.ini",
            "Ini/ge3.ini",
        ):
            with self.assertRaises(updater.UpdateError):
                updater.validate_relative(relative)

    def test_hard_linked_config_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = self.make_roots(base)
            target = roots["host"] / "g3together.ini"
            target.write_text("role = host\n", encoding="utf-8")
            other = base / "other.ini"
            os.link(target, other)
            with self.assertRaisesRegex(updater.UpdateError, "hard-linked"):
                updater.safe_target(roots["host"], "g3together.ini")

    def test_dry_run_and_apply_with_rollback(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = self.make_roots(base)
            fake = base / "fake_steam_api.dll"
            fake.write_bytes(b"synthetic fake steam api")
            approved = hashlib.sha256(fake.read_bytes()).hexdigest()
            genuine_bytes = b"verified genuine steam api"
            for role in ("host", "client"):
                genuine_target = (
                    roots[role] / "G3Together" / "steam_api.dll"
                )
                genuine_target.parent.mkdir(parents=True, exist_ok=True)
                genuine_target.write_bytes(genuine_bytes)

            transaction_id, entries = updater.build_plan(
                roots,
                fake,
                "pair-red",
                approved,
                ("host", "client"),
            )
            manifest = updater.prepare_manifest(transaction_id, roots, entries)
            self.assertEqual(manifest["default_launcher"], "launch-copies-invite.ps1")
            self.assertEqual(manifest["legacy_launcher"], "launch-copies-realism.ps1")
            self.assertEqual(manifest["launcher_profile"], "ideal")
            self.assertEqual(manifest["connection"], "invite")
            self.assertEqual(manifest["session_id"], "pair-red")
            manifest_path = base / "transaction.json"
            updater.write_manifest(manifest_path, manifest)
            original_config = roots["host"] / "g3together.ini"
            self.assertFalse(original_config.exists())

            updater.check_running_copies(
                roots,
                ("host", "client"),
                process_reader=lambda: [],
            )
            updater.verify_plan_drift(entries)
            updater.apply_entries(manifest_path, manifest, entries)

            for role in ("host", "client"):
                target = roots[role] / "g3together.ini"
                self.assertTrue(target.is_file())
                text = target.read_text(encoding="utf-8")
                self.assertIn("connection = invite", text)
                self.assertIn("session_id = pair-red", text)
                fake_target = (
                    roots[role] / "G3Together" / "test-realism" / "fake_steam_api.dll"
                )
                self.assertEqual(fake_target.read_bytes(), fake.read_bytes())
                genuine_target = roots[role] / "G3Together" / "steam_api.dll"
                self.assertEqual(genuine_target.read_bytes(), genuine_bytes)

            manifest = updater.load_manifest(manifest_path)
            updater.rollback_manifest(
                manifest_path,
                manifest,
                roots,
                True,
                process_reader=lambda: [],
            )
            manifest = updater.load_manifest(manifest_path)
            self.assertEqual(manifest["state"], "rolled-back")
            self.assertFalse((roots["host"] / "g3together.ini").exists())
            self.assertFalse(
                (
                    roots["host"] / "G3Together" / "test-realism" / "fake_steam_api.dll"
                ).exists()
            )

    def test_external_edit_rejects_rollback(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = self.make_roots(base)
            fake = base / "fake_steam_api.dll"
            fake.write_bytes(b"synthetic fake")
            approved = hashlib.sha256(fake.read_bytes()).hexdigest()
            transaction_id, entries = updater.build_plan(
                roots,
                fake,
                "default",
                approved,
                ("host", "client"),
            )
            manifest = updater.prepare_manifest(transaction_id, roots, entries)
            manifest_path = base / "transaction.json"
            updater.write_manifest(manifest_path, manifest)
            updater.apply_entries(manifest_path, manifest, entries)

            config = roots["host"] / "g3together.ini"
            config.write_text("role = host\npersonal_edit = true\n", encoding="utf-8")
            loaded = updater.load_manifest(manifest_path)
            with self.assertRaisesRegex(updater.UpdateError, "externally modified"):
                updater.rollback_manifest(
                    manifest_path,
                    loaded,
                    roots,
                    True,
                    process_reader=lambda: [],
                )

    def test_unknown_fake_hash_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = self.make_roots(base)
            fake = base / "fake.dll"
            fake.write_bytes(b"unknown synthetic fake")
            with self.assertRaisesRegex(updater.UpdateError, "Unknown staged fake DLL"):
                updater.build_plan(
                    roots,
                    fake,
                    "default",
                    None,
                    ("host", "client"),
                )

    def test_running_exact_destination_refuses_apply(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = self.make_roots(base)
            executable = roots["host"] / "Gothic3.exe"
            executable.write_bytes(b"synthetic")
            with self.assertRaisesRegex(updater.UpdateError, "exact target executable"):
                updater.check_running_copies(
                    roots,
                    ("host",),
                    process_reader=lambda: [
                        {
                            "Name": "Gothic3.exe",
                            "ExecutablePath": str(executable),
                        }
                    ],
                )

    def test_running_destination_with_unknown_path_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = self.make_roots(base)
            with self.assertRaisesRegex(updater.UpdateError, "cannot determine"):
                updater.check_running_copies(
                    roots,
                    ("host",),
                    process_reader=lambda: [
                        {"Name": "Gothic3.exe", "ExecutablePath": None}
                    ],
                )

    def test_native_slot_proof_accepts_only_exact_qa_slot(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = self.make_roots(base)
            genuine = bytearray(0x1000000)
            genuine[0:16] = b"synthetic-native"
            for role in ("host", "client", "steam"):
                for relative, offset in verifier.NATIVE_SLOTS.items():
                    path = roots[role] / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    candidate = bytearray(genuine)
                    if role != "steam":
                        candidate[offset:offset + 12] = verifier.SLOT_QA
                    else:
                        candidate[offset:offset + 12] = verifier.SLOT_GENUINE
                    path.write_bytes(candidate)
            rows, failures = verifier.verify_native_slots(roots, {
                "hash_inventory": {"rows": []},
            })
            self.assertFalse(failures)
            self.assertEqual(rows[0][1], "PASS")

            raw_genuine = roots["host"] / "Game.dll"
            raw_candidate = bytearray(genuine)
            raw_candidate[
                verifier.NATIVE_SLOTS["Game.dll"]:
                verifier.NATIVE_SLOTS["Game.dll"] + 12
            ] = verifier.SLOT_GENUINE
            raw_genuine.write_bytes(raw_candidate)
            _rows, failures = verifier.verify_native_slots(roots, {
                "hash_inventory": {"rows": []},
            })
            self.assertTrue(any("Unexpected isolation slot" in item for item in failures))

    def test_partial_apply_failure_rolls_back_applied_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = self.make_roots(base)
            fake = base / "fake.dll"
            fake.write_bytes(b"synthetic fake")
            approved = hashlib.sha256(fake.read_bytes()).hexdigest()
            transaction_id, entries = updater.build_plan(
                roots,
                fake,
                "default",
                approved,
                ("host", "client"),
            )
            manifest = updater.prepare_manifest(transaction_id, roots, entries)
            manifest_path = base / "transaction.json"
            updater.write_manifest(manifest_path, manifest)

            real_replace = os.replace

            def fail_fake_replacement(source, destination):
                if Path(destination).name == "fake_steam_api.dll":
                    raise OSError("synthetic fake-DLL replacement failure")
                return real_replace(source, destination)

            with mock.patch.object(
                updater.os,
                "replace",
                side_effect=fail_fake_replacement,
            ):
                with self.assertRaisesRegex(updater.UpdateError, "replacement failure"):
                    updater.apply_entries(manifest_path, manifest, entries)

            loaded = updater.load_manifest(manifest_path)
            self.assertIn("failed", loaded["state"])
            self.assertFalse((roots["host"] / "g3together.ini").exists())
            self.assertFalse(
                (
                    roots["host"] / "G3Together" / "test-realism" / "fake_steam_api.dll"
                ).exists()
            )

    def test_destination_drift_before_apply_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = self.make_roots(base)
            existing = roots["host"] / "g3together.ini"
            existing.write_text("role = host\n", encoding="utf-8")
            transaction_id, entries = updater.build_plan(
                roots,
                None,
                "default",
                None,
                ("host",),
            )
            existing.write_text("role = host\nexternal = changed\n", encoding="utf-8")
            with self.assertRaisesRegex(updater.UpdateError, "drifted"):
                updater.verify_plan_drift(entries)

    def test_mod_disk_receipt_is_unknown_until_approved(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = self.make_roots(base)
            mod = roots["host"] / "scripts" / "Script_G3Together.dll"
            mod.parent.mkdir(parents=True)
            mod.write_bytes(b"synthetic unknown mod")
            client_mod = roots["client"] / "scripts" / "Script_G3Together.dll"
            client_mod.parent.mkdir(parents=True)
            client_mod.write_bytes(mod.read_bytes())
            steam_mod = roots["steam"] / "scripts" / "Script_G3Together.dll"
            steam_mod.parent.mkdir(parents=True)
            steam_mod.write_bytes(mod.read_bytes())

            rows, failures, warnings = verifier.verify_mod_receipt(
                roots,
                None,
                None,
                None,
                None,
            )
            self.assertEqual(rows[0][1], "FAIL")
            self.assertTrue(failures)
            self.assertTrue(any("unapproved/unknown" in item for item in failures))

    def test_staged_invite_launcher_printonly_profiles_and_alias(self):
        package = self.staged_package()
        alias = package / "launch-copies-invite.ps1"
        for profile in ("ideal", "steam-typical", "steam-bad"):
            result = subprocess.run(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(alias),
                    "-Profile",
                    profile,
                    "-AcceptVia",
                    "Callback",
                    "-Session",
                    "pair-red",
                    "-RunDirectory",
                    r"C:\approved-run",
                    "-PrintOnly",
                ],
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("SYNTHETIC FAKE STEAM ONLY", result.stdout)
            self.assertIn("PROFILE: " + profile, result.stdout)
            self.assertIn("CALLBACK_337", result.stdout)

    def test_launcher_scopes_host_receipt_and_fresh_namespace(self):
        package = self.staged_package()
        launcher = (package / "launch-copies-realism.ps1").read_text(
            encoding="utf-8"
        )
        alias = (package / "launch-copies-invite.ps1").read_text(
            encoding="utf-8"
        )
        self.assertIn("[Nullable[double]]$OneWayMs", launcher)
        self.assertIn("[Nullable[double]]$OneWayMs", alias)
        self.assertIn("legacy-OneWayMs-override", launcher)
        self.assertIn("G3T_FAKESTEAM_LATENCY_MS = [string]$effectiveLatencyMs", launcher)
        self.assertIn("host_creation_time differs from actual PID start time", launcher)
        self.assertIn("Join-Path $hostRootFull \"Gothic3.exe\"", launcher)
        self.assertIn("new empty RunDirectory namespace", launcher)
        self.assertIn("[Double]::IsNaN", launcher)
        self.assertIn("Refusing to overwrite an existing run manifest", launcher)
        self.assertNotIn("Remove-Item -Force", launcher)
        profiles = json.loads((package / "profiles.json").read_text(encoding="utf-8"))
        for profile in profiles["profiles"].values():
            self.assertIn("api_send_buffer_bytes", profile)
            self.assertIn("api_send_rate_min_bps", profile)
            self.assertIn("api_send_rate_max_bps", profile)
            self.assertIn("api_send_rate_max_bps", profile)
            self.assertIn("api_nagle_us", profile)
            self.assertIn("physical_rate_bps", profile)
            self.assertIn("basic-430-contract-validated", profile["contract_status"])
            self.assertIn("product-matrix-recorded", profile["contract_status"])
            self.assertIn("queue_delay_ms", profile)
            self.assertIn("queue_delay_status", profile)
            self.assertIn("queue_delay_period_ms", profile)
            self.assertIn("queue_delay_window_ms", profile)
            self.assertIn("queue_delay_phase_ms", profile)
            for legacy in (
                "throughput_kib_s",
                "send_buffer_kib",
                "queue_capacity_packets",
            ):
                self.assertNotIn(legacy, profile)
        bad = profiles["profiles"]["steam-bad"]
        self.assertEqual(bad["queue_delay_ms"], 2000)
        self.assertEqual(bad["queue_delay_period_ms"], 7000)
        self.assertEqual(bad["queue_delay_window_ms"], 100)
        self.assertEqual(bad["queue_delay_phase_ms"], 6000)
        receipt = json.loads(
            (package / "G3Together" / "test-realism" / "receipt.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(
            receipt["fake_dll_sha256"],
            updater.KNOWN_FAKE_SHA,
        )
        self.assertEqual(receipt["basic_contract"]["assertions"], 430)
        self.assertEqual(receipt["basic_contract"]["status"], "PASS")
        self.assertEqual(receipt["two_process"]["status"], "PASS")

    def test_launcher_clears_all_fake_env_and_reads_log_offset(self):
        package = self.staged_package()
        launcher = (package / "launch-copies-realism.ps1").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            'StartsWith("G3T_FAKESTEAM_", [StringComparison]::OrdinalIgnoreCase)',
            launcher,
        )
        self.assertIn("Read-Shared $LogPath $Offset", launcher)
        self.assertIn("Lobby readiness read transiently unavailable", launcher)
        self.assertIn("UnauthorizedAccessException", launcher)
        for relative in (
            "G3Together/test-realism/fake_steam_api.dll",
            "G3Together/test-realism/fake_steam_diag.h",
            "G3Together/test-realism/fake_steam_abi.h",
            "G3Together/test-realism/profiles/ideal.ini",
            "G3Together/test-realism/profiles/steam-typical.ini",
            "G3Together/test-realism/profiles/steam-bad.ini",
            "G3Together/test-realism/test-summary.txt",
        ):
            self.assertTrue((package / relative).is_file(), relative)

    def test_updater_includes_complete_fake_bundle_sidecars(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = self.make_roots(base)
            bundle = base / "bundle"
            (bundle / "source").mkdir(parents=True)
            (bundle / "profiles").mkdir()
            fake = bundle / "fake_steam_api.dll"
            fake.write_bytes(b"synthetic complete fake")
            for relative in (
                "fake_steam_diag.h",
                "fake_steam_abi.h",
                "fake_steam_api.def",
                "build.ps1",
                "receipt.json",
                "run_tests.ps1",
                "README.md",
                "test-summary.txt",
                "source/fake_steam_api.c",
                "source/fake_steam_test.c",
                "fake_steam_process_test.c",
                "fake_steam_contract_test.c",
                "fake_steam_test.exe",
                "fake_steam_process_test.exe",
                "fake_steam_contract_test.exe",
                "profiles/ideal.ini",
                "profiles/steam-typical.ini",
                "profiles/steam-bad.ini",
            ):
                path = bundle / relative
                path.write_text("synthetic " + relative, encoding="utf-8")

            approved = hashlib.sha256(fake.read_bytes()).hexdigest()
            _transaction_id, entries = updater.build_plan(
                roots,
                fake,
                "pair-red",
                approved,
                ("host", "client"),
            )
            relative_paths = {entry["relative_path"] for entry in entries}
            expected = {
                "G3Together/test-realism/fake_steam_api.dll",
                "G3Together/test-realism/fake_steam_diag.h",
                "G3Together/test-realism/fake_steam_abi.h",
                "G3Together/test-realism/fake_steam_api.def",
                "G3Together/test-realism/build.ps1",
                "G3Together/test-realism/receipt.json",
                "G3Together/test-realism/run_tests.ps1",
                "G3Together/test-realism/README.md",
                "G3Together/test-realism/test-summary.txt",
                "G3Together/test-realism/source/fake_steam_api.c",
                "G3Together/test-realism/source/fake_steam_test.c",
                "G3Together/test-realism/fake_steam_process_test.c",
                "G3Together/test-realism/fake_steam_contract_test.c",
                "G3Together/test-realism/fake_steam_test.exe",
                "G3Together/test-realism/fake_steam_process_test.exe",
                "G3Together/test-realism/fake_steam_contract_test.exe",
                "G3Together/test-realism/profiles/ideal.ini",
                "G3Together/test-realism/profiles/steam-typical.ini",
                "G3Together/test-realism/profiles/steam-bad.ini",
            }
            for relative in expected:
                self.assertIn(relative, relative_paths)
            fake_entries = [
                entry for entry in entries
                if entry["relative_path"].startswith("G3Together/test-realism/")
            ]
            self.assertTrue(all(entry["source_sha256"] for entry in fake_entries))

    def test_updater_rejects_incomplete_fake_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = self.make_roots(base)
            bundle = base / "bundle"
            bundle.mkdir()
            fake = bundle / "fake_steam_api.dll"
            fake.write_bytes(b"synthetic incomplete fake")
            (bundle / "fake_steam_diag.h").write_text("header", encoding="utf-8")
            approved = hashlib.sha256(fake.read_bytes()).hexdigest()
            with self.assertRaisesRegex(updater.UpdateError, "Incomplete staged fake-transport bundle"):
                updater.build_plan(
                    roots,
                    fake,
                    "pair-red",
                    approved,
                    ("host", "client"),
                )

    def test_synthetic_client_only_receipt_is_checked_in_printonly(self):
        package = self.staged_package()
        alias = package / "launch-copies-invite.ps1"
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            host = base / "host"
            client = base / "client"
            host.mkdir()
            client.mkdir()
            fake = package / "G3Together" / "test-realism" / "fake_steam_api.dll"
            fake_sha = hashlib.sha256(fake.read_bytes()).hexdigest()
            receipt = base / "host-receipt.json"
            receipt.write_text(
                json.dumps(
                    {
                        "role": "host",
                        "session": "pair-red",
                        "profile": "steam-bad",
                        "host_pid": 1234,
                        "host_exe": str(host / "Gothic3.exe"),
                        "host_creation_time": "2026-11-07T23:00:00Z",
                        "ports": {"host": 34567, "client": 34568},
                        "fake_sha256": fake_sha,
                        "lobby_root": str(base / "fakesteam"),
                        "run_directory": str(base),
                    }
                ),
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(alias),
                    "-Profile",
                    "steam-bad",
                    "-ClientOnly",
                    "-AcceptVia",
                    "Callback",
                    "-Session",
                    "pair-red",
                    "-RunDirectory",
                    str(base),
                    "-HostReceipt",
                    str(receipt),
                    "-HostRoot",
                    str(host),
                    "-ClientRoot",
                    str(client),
                    "-PrintOnly",
                ],
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("ROLE: client-only", result.stdout)


if __name__ == "__main__":
    unittest.main()
