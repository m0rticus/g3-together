# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]


def win32_tool_environment(build):
    env = os.environ.copy()
    env["TEMP"] = env["TMP"] = str(build)
    if env.get("VSCMD_ARG_TGT_ARCH") == "x86" and shutil.which("cl", path=env.get("PATH")):
        return env
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        raise RuntimeError("Visual Studio vswhere is required to locate the Win32 toolchain")
    result = subprocess.run(
        [str(vswhere), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        capture_output=True, text=True, timeout=30, env=env)
    if result.returncode or not result.stdout.strip():
        raise RuntimeError("Visual Studio Win32 C++ tools not found: " + result.stderr)
    vcvars = Path(result.stdout.strip()) / "VC" / "Auxiliary" / "Build" / "vcvars32.bat"
    if not vcvars.is_file():
        raise RuntimeError(f"Win32 toolchain setup not found: {vcvars}")
    command = f'"{os.environ.get("COMSPEC", "cmd.exe")}" /d /s /c "call "{vcvars}" >nul && set"'
    result = subprocess.run(command,
                            cwd=build, env=env, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError("Win32 toolchain setup failed:\n" + result.stdout + result.stderr)
    for line in result.stdout.splitlines():
        if "=" in line and not line.startswith("="):
            name, value = line.split("=", 1)
            for existing in list(env):
                if existing.lower() == name.lower() and existing != name:
                    del env[existing]
            env[name] = value
    env["TEMP"] = env["TMP"] = str(build)
    return env


class EnetRelayHandshakeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.name != "nt":
            raise unittest.SkipTest("production /Gz Win32 ENet ABI requires Windows")
        build = ROOT / "build" / f"enet-relay-handshake-{os.getpid()}"
        build.mkdir(parents=True)
        cls.addClassCleanup(shutil.rmtree, build)
        cls.build = build
        cls.env = win32_tool_environment(build)
        cls.executable = build / "test.exe"
        compiler = shutil.which("cl.exe", path=cls.env.get("PATH") or cls.env.get("Path"))
        if not compiler:
            raise RuntimeError("Win32 compiler not found after vcvars32")
        enet = ROOT / "thirdparty" / "enet"
        include = enet / "include"
        sources = sorted(enet.glob("*.c"))
        objects = [build / (source.stem + ".obj") for source in sources]
        commands = [
            [compiler, "/nologo", "/TC", "/c", "/Gz", "/O2", "/W3",
             "/D_WINSOCK_DEPRECATED_NO_WARNINGS", f"/I{include}",
             *(str(source) for source in sources), f"/Fo{build}\\"],
            [compiler, "/nologo", "/std:c++17", "/EHsc", "/Gz", "/O2", "/W4", "/WX",
             f"/I{include}", str(ROOT / "tests" / "enet_relay_handshake_test.cpp"),
             *(str(obj) for obj in objects), f"/Fo{build / 'test.obj'}",
             f"/Fe{cls.executable}", "/link", "ws2_32.lib", "winmm.lib"],
        ]
        warnings = []
        for command in commands:
            result = subprocess.run(command, cwd=build, env=cls.env, capture_output=True,
                                    text=True, timeout=120)
            output = result.stdout + result.stderr
            warnings.extend(line for line in output.splitlines()
                            if re.search(r"\bwarning [A-Z]+\d+:", line, re.IGNORECASE))
            if result.returncode:
                raise RuntimeError(output)
        print(f"Win32 /Gz compiler warnings: {len(warnings)}")
        for warning in warnings:
            print(warning)

    def run_case(self, *args):
        result = subprocess.run([str(self.executable), *args], cwd=self.build, env=self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="")
        return result.stdout

    def test_native_win32_compressed_handshake_through_both_queues(self):
        self.assertIn("native ENet two-relay queued handshake/data/reconnect passed", self.run_case())
        self.assertIn("all 13 native command sizes match", self.run_case("sizes"))
        self.assertIn("direct ENet UDP cookie hole-punch and uninvited packet drop passed", self.run_case("direct"))

    def test_native_win32_impaired_first_handshake_through_unreliable_queues(self):
        self.assertIn("impaired connected=2 exact_reliable_payloads=2",
                      self.run_case("impaired"))


if __name__ == "__main__":
    unittest.main()
