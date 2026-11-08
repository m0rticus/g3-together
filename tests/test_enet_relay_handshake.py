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
    def test_native_win32_compressed_handshake_through_both_queues(self):
        if os.name != "nt":
            self.skipTest("production /Gz Win32 ENet ABI requires Windows")
        build = ROOT / "build" / f"enet-relay-handshake-{os.getpid()}"
        build.mkdir(parents=True)
        try:
            env = win32_tool_environment(build)
            compiler = shutil.which("cl.exe", path=env.get("PATH") or env.get("Path"))
            self.assertIsNotNone(compiler, "Win32 compiler not found after vcvars32")
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
                 f"/Fe{build / 'test.exe'}", "/link", "ws2_32.lib", "winmm.lib"],
            ]
            warnings = []
            for command in commands:
                result = subprocess.run(command, cwd=build, env=env, capture_output=True,
                                        text=True, timeout=120)
                output = result.stdout + result.stderr
                warnings.extend(line for line in output.splitlines()
                                if re.search(r"\bwarning [A-Z]+\d+:", line, re.IGNORECASE))
                self.assertEqual(result.returncode, 0, output)
            result = subprocess.run([str(build / "test.exe")], cwd=build, env=env,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(f"Win32 /Gz compiler warnings: {len(warnings)}")
            for warning in warnings:
                print(warning)
            print(result.stdout, end="")
            result = subprocess.run([str(build / "test.exe"), "sizes"], cwd=build, env=env,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")
        finally:
            shutil.rmtree(build)


if __name__ == "__main__":
    unittest.main()
