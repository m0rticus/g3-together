# SPDX-License-Identifier: GPL-3.0-or-later
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


NET_SEPARATE_DEFINES = ("G3T_NET_SEPARATE_TUS=1",)
NET_SEPARATE_UNITS = tuple(
    _testbuild.unit(
        source,
        std="c++17",
        warnings="strict",
        defines=NET_SEPARATE_DEFINES,
        includes=(ROOT / "thirdparty" / "enet" / "include", ROOT / "src"),
    )
    for source in (
        "src/net.cpp",
        "src/net_session.cpp",
        "src/net_npc.cpp",
        "src/net_npc_pose.cpp",
        "src/net_messages.cpp",
    )
)
NET_DIRECT_PUNCH_TARGET = _testbuild.register_target(
    "net-direct-punch",
    [
        _testbuild.unit(
            "tests/net_direct_punch_test.cpp",
            std="c++17",
            warnings="strict",
            includes=(ROOT / "thirdparty" / "enet" / "include", ROOT / "src"),
        ),
        *NET_SEPARATE_UNITS,
        *(
            _testbuild.unit(
                "src/npcwire_bones.cpp",
                std="c++17",
                warnings="strict",
                includes=(ROOT / "src",),
            ),
            _testbuild.unit(
                "src/npcwire_control.cpp",
                std="c++17",
                warnings="strict",
                includes=(ROOT / "src",),
            ),
            _testbuild.unit(
                "src/npcwire_adapter.cpp",
                std="c++17",
                warnings="strict",
                includes=(ROOT / "src",),
            ),
        ),
    ],
)


class NetDirectPunchTests(unittest.TestCase):
    def test_direct_punch_security_fallback_and_goods_validation_separate_tus(self):
        result = NET_DIRECT_PUNCH_TARGET.run(timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
