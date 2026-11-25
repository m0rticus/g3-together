# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

SECTOR_SYNC_TARGET = _testbuild.register_target(
    "sector-sync-policy",
    [_testbuild.unit(
        "tests/sector_sync_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)

TOUCHED_SECTOR_FILES = (
    "sector_sync_policy.h",
    "world_sectors.h",
    "world_sectors.cpp",
    "infocmd.cpp",
    "infocmd_internal.h",
    "infocmd_progression.cpp",
    "infocmd_qa.cpp",
    "persist.cpp",
    "persist_hooks.cpp",
    "persist_storage.cpp",
    "net.cpp",
    "net_internal.h",
    "net_session.cpp",
    "Script_G3Together.cpp",
)


class SectorSyncTests(unittest.TestCase):
    def test_engine_free_sector_sync_policy(self):
        with tempfile.TemporaryDirectory() as folder:
            result = SECTOR_SYNC_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_host_authoritative_sector_hooks_persistence_and_admission_wiring(self):
        world_sectors = _testbuild.src_text("world_sectors.cpp")
        persist = _testbuild.src_text("persist.cpp")
        persist_hooks = _testbuild.src_text("persist_hooks.cpp")
        persist_storage = _testbuild.src_text("persist_storage.cpp")
        net_cpp = _testbuild.src_text("net.cpp")
        net_session = _testbuild.src_text("net_session.cpp")
        script = _testbuild.src_text("Script_G3Together.cpp")
        infocmd = _testbuild.src_text("infocmd.cpp")

        # Game.dll hooks capture gCSector::Enable (dialogs, scripts, EnableSectorFolder) and console OnChangeSectorStatus,
        # and update gCSectorPersistence_PS::CollectSectors() plus RequestProcessingUpdate().
        self.assertIn("?Enable@gCSector@@QAE_N_N@Z", world_sectors)
        self.assertIn("?OnChangeSectorStatus@gCGameApp@@QAGXPAVbCObjectRefBase@@PAVeCConsoleEventArg@@@Z", world_sectors)
        self.assertIn("eEPropertySetType_SectorPersistence", world_sectors)
        self.assertIn("ps->CollectSectors()", world_sectors)
        self.assertIn("world::RequestProcessingUpdate()", world_sectors)
        self.assertIn('persist::SaveExtensions::Register("world_sectors"', world_sectors)
        self.assertIn("Reconcile(", world_sectors)

        # Client admission waits on sectors::AdmissionReady() and reconciles at FinishAdmission().
        self.assertIn("sectors::PrepareAdmission()", persist)
        self.assertIn("sectors::AdmissionReady()", persist)
        self.assertIn("sectors::FinishAdmission(from)", persist)
        self.assertIn("sectors::AdmissionReady()", persist_hooks)
        self.assertIn("sectors::PrepareAdmission()", persist_storage)
        self.assertIn("sectors::OnWorldChange()", persist_storage)
        self.assertIn("sectors::Start()", script)
        self.assertIn("&g3t::sectors::Tick", script)
        self.assertIn("&g3t::sectors::OnWorldChange", script)

        # Pre-admission ENet gate permits reliable SectorHandshake (kind == 2 baseline/request/catchup).
        self.assertIn("SectorHandshake(data, size)", net_cpp)
        self.assertIn("SectorHandshake(packet->data, packet->dataLength)", net_cpp)
        self.assertIn("sector_sync::IsHandshakeWire(", net_session)
        self.assertIn("sector_sync::ValidSendWorldStateTo(", net_session)

        # infocmd routes kind == 2 handshake/live messages through sectors::HandleWire and sectors::ApplySector.
        self.assertIn("sectors::HandleWire(from, s)", infocmd)
        self.assertIn("sectors::ApplySector(s.routine, s.enclave != 0", infocmd)

    def test_touched_files_stay_under_line_limits(self):
        for name in TOUCHED_SECTOR_FILES:
            lines = len(_testbuild.src_text(name).splitlines())
            self.assertLess(lines, 1000, f"src/{name} has {lines} lines (must stay strictly under 1,000 lines)")


if __name__ == "__main__":
    unittest.main()
