from __future__ import annotations

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


copy_preflight = load_tool("copy_preflight")


def synthetic_pe(path: Path, dll_name: str = "g3qa32.dll", function_name: str = "SHGetFolderPathA") -> None:
    data = bytearray(0x300)
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

    section = 0x178
    data[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<I", data, section + 8, 0x1000)
    struct.pack_into("<I", data, section + 12, 0x1000)
    struct.pack_into("<I", data, section + 16, 0x1000)
    struct.pack_into("<I", data, section + 20, 0x200)

    descriptor = 0x200
    struct.pack_into("<IIIII", data, descriptor, 0x1020, 0, 0, 0x1040, 0x1020)
    struct.pack_into("<I", data, 0x220, 0x1060)
    struct.pack_into("<I", data, 0x224, 0)

    dll = dll_name.encode("ascii") + b"\0"
    data[0x240:0x240 + len(dll)] = dll

    name = function_name.encode("ascii") + b"\0"
    data[0x260:0x260 + len(name)] = name
    struct.pack_into("<H", data, 0x260, 0)
    data[0x262:0x262 + len(name) - 1] = name[:-1]
    data[0x262 + len(name) - 1] = 0
    path.write_bytes(data)


class CopyPreflightTests(unittest.TestCase):
    def test_overlaps_rejects_nested_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child = root / "copy"
            child.mkdir()
            self.assertTrue(copy_preflight.overlaps(root, child))

    def test_isolation_modules_accepts_g3qa32_import(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "g3qa32.dll").write_bytes(b"shim")
            for module_name in copy_preflight.FOLDER_MODULES:
                (root / module_name).write_bytes(b"synthetic module")
            with mock.patch.object(
                copy_preflight,
                "imports",
                return_value={"g3qa32.dll": ["SHGetFolderPathA"]},
            ):
                copy_preflight.check_isolation_modules(root)

    def test_isolation_modules_rejects_native_folder_import(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "g3qa32.dll").write_bytes(b"shim")
            for module_name in copy_preflight.FOLDER_MODULES:
                (root / module_name).write_bytes(b"synthetic module")
            with mock.patch.object(
                copy_preflight,
                "imports",
                return_value={"shell32.dll": ["SHGetFolderPathA"]},
            ):
                with self.assertRaisesRegex(RuntimeError, "Unisolated folder import"):
                    copy_preflight.check_isolation_modules(root)

    def test_synthetic_pe_rva_mapping(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "module.dll"
            synthetic_pe(path)
            data = path.read_bytes()
            self.assertEqual(data[:2], b"MZ")
            self.assertEqual(data[0x80:0x84], b"PE\0\0")
            sections = [(0x1000, 0x1000, 0x200, 0x1000)]
            self.assertEqual(copy_preflight.pe_rva_to_offset(data, 0x1040, sections, 0x80), 0x240)


if __name__ == "__main__":
    unittest.main()
