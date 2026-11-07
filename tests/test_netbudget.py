import os
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]


class NetBudgetTests(unittest.TestCase):
    def compile_and_run(self, build, compiler, source, executable):
        if Path(compiler).stem.lower() == "cl":
            command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/O2",
                       str(source), f"/Fe:{executable}", f"/Fo:{build / 'netbudget.obj'}"]
        else:
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                       str(source), "-o", str(executable)]
        return subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)

    def test_pure_policy_and_actual_enet_fragment_boundaries(self):
        cxx = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(cxx, "C++17 compiler required")

        build = ROOT / "build" / f"netbudget-tests-{os.getpid()}"
        build.mkdir(parents=True, exist_ok=True)
        try:
            policy = build / ("netbudget_test.exe" if os.name == "nt" else "netbudget_test")
            compiled = self.compile_and_run(
                build, cxx, ROOT / "tests" / "netbudget_test.cpp", policy)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(policy)], cwd=build, capture_output=True, text=True, timeout=60)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

            enet = ROOT / "thirdparty" / "enet"
            include = enet / "include"
            enet_test = build / ("netbudget_enet_test.exe" if os.name == "nt" else "netbudget_enet_test")

            c_compiler = os.environ.get("CC")
            if not c_compiler:
                stem = Path(cxx).stem.lower()
                if stem == "cl":
                    c_compiler = cxx
                elif "clang" in stem:
                    c_compiler = shutil.which("clang")
                else:
                    c_compiler = shutil.which("gcc")
            self.assertIsNotNone(c_compiler, "C compiler required for ENet harness")

            c_sources = [enet / "peer.c", enet / "list.c", enet / "packet.c", enet / "callbacks.c"]
            objects = []
            stem_lower = Path(c_compiler).stem.lower()
            if stem_lower == "cl":
                for source in c_sources:
                    obj = build / (source.stem + ".obj")
                    command = [c_compiler, "/nologo", "/TC", "/O2", f"/I{include}",
                               "/c", str(source), f"/Fo:{obj}"]
                    result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    objects.append(obj)
                command = [cxx, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/O2",
                           f"/I{include}", str(ROOT / "tests" / "netbudget_enet_test.cpp"),
                           *(str(obj) for obj in objects),
                           f"/Fe:{enet_test}", "ws2_32.lib", "winmm.lib"]
            else:
                for source in c_sources:
                    obj = build / (source.stem + ".o")
                    command = [c_compiler, "-std=c11", "-O2", "-I", str(include),
                               "-c", str(source), "-o", str(obj)]
                    result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    objects.append(obj)
                command = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                           "-I", str(include), str(ROOT / "tests" / "netbudget_enet_test.cpp"),
                           *(str(obj) for obj in objects), "-o", str(enet_test)]
                if os.name == "nt":
                    command.extend(["-lws2_32", "-lwinmm"])

            compiled = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(enet_test)], cwd=build, capture_output=True, text=True, timeout=60)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
        finally:
            shutil.rmtree(build, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
