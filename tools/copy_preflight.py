#!/usr/bin/env python3
"""Manually verify separate Gothic 3 copies before a manual co-op session.

This tool only reads files and reports errors. It never launches a game, sends
input, installs mods, creates fixtures, takes snapshots, or starts a session.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import struct
import sys
from pathlib import Path
from typing import Iterable


REPO = Path(__file__).resolve().parents[1]
FOLDER_MODULES = ("Game.dll", "Engine.dll", "SharedBase.dll", "Dialogs.dll", "paru_s2.dll")


def reject_links(path: Path) -> Path:
    """Reject symlinks and Windows reparse points on a path and every parent."""
    path = Path(os.path.abspath(path))
    for part in (path, *path.parents):
        if part.exists() or part.is_symlink():
            info = part.lstat()
            reparse = getattr(info, "st_file_attributes", 0) & getattr(
                stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400
            )
            if stat.S_ISLNK(info.st_mode) or reparse:
                raise RuntimeError(f"Link or reparse point is not allowed: {part}")
    return path


def canonical(path: Path) -> Path:
    return reject_links(path).resolve()


def overlaps(left: Path, right: Path) -> bool:
    try:
        return left == right or left.is_relative_to(right) or right.is_relative_to(left)
    except AttributeError:
        left_text = os.path.normcase(str(left))
        right_text = os.path.normcase(str(right))
        return left_text == right_text or left_text.startswith(right_text + os.sep) or right_text.startswith(left_text + os.sep)


def is_hard_linked(path: Path) -> bool:
    try:
        return path.stat(follow_symlinks=False).st_nlink != 1
    except TypeError:
        return path.lstat().st_nlink != 1


def read_workspace_config(workspace: Path) -> dict:
    project = workspace / "project.json"
    if not project.is_file():
        return {}
    reject_links(project)
    try:
        return json.loads(project.read_text(encoding="utf-8-sig"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Invalid JSON in {project}: {exc}") from exc


def configured_copies(config: dict) -> list[Path]:
    for key in ("copies", "copy_roots", "isolated_copies", "game_copies"):
        value = config.get(key)
        if isinstance(value, dict):
            return [Path(path) for path in value.values() if path]
        if isinstance(value, list):
            return [Path(path) for path in value if path]
    return []


def require_file(root: Path, relative: str) -> Path:
    path = reject_links(root / relative)
    if not path.is_file():
        raise RuntimeError(f"Missing required game/mod file: {path}")
    return path


def require_directory(root: Path, relative: str) -> Path:
    path = reject_links(root / relative)
    if not path.is_dir():
        raise RuntimeError(f"Missing required directory: {path}")
    return path


def pe_rva_to_offset(data: bytes, rva: int, sections: list[tuple[int, int, int, int]], pe_offset: int) -> int:
    for virtual_address, virtual_size, raw_address, raw_size in sections:
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_address + rva - virtual_address
    raise ValueError(f"Unmapped PE RVA 0x{rva:x}")


def read_ascii(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("Unterminated PE import name")
    try:
        return data[offset:end].decode("ascii")
    except UnicodeDecodeError as exc:
        raise ValueError("Non-ASCII PE import name") from exc


def imports(path: Path) -> dict[str, list[str]]:
    """Read PE32 imports without loading executable code."""
    data = Path(path).read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"Not a PE file: {path}")
    if len(data) < 0x40:
        raise ValueError(f"Truncated PE file: {path}")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise ValueError(f"Missing PE header: {path}")

    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    section_count = struct.unpack_from("<H", data, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    optional_offset = pe_offset + 24
    if machine != 0x14C:
        raise ValueError(f"Not a 32-bit PE file: {path}")
    if optional_size < 112 or optional_offset + optional_size > len(data):
        raise ValueError(f"Truncated PE32 optional header: {path}")
    if struct.unpack_from("<H", data, optional_offset)[0] != 0x10B:
        raise ValueError(f"Not PE32: {path}")

    section_offset = optional_offset + optional_size
    sections = []
    for index in range(section_count):
        header = section_offset + 40 * index
        if header + 40 > len(data):
            raise ValueError(f"Truncated PE section table: {path}")
        virtual_size = struct.unpack_from("<I", data, header + 8)[0]
        virtual_address = struct.unpack_from("<I", data, header + 12)[0]
        raw_size = struct.unpack_from("<I", data, header + 16)[0]
        raw_address = struct.unpack_from("<I", data, header + 20)[0]
        sections.append((virtual_address, virtual_size, raw_address, raw_size))

    rva_count = struct.unpack_from("<I", data, optional_offset + 92)[0]
    if rva_count < 2:
        return {}
    import_rva = struct.unpack_from("<I", data, optional_offset + 104)[0]
    if import_rva == 0:
        return {}

    found: dict[str, list[str]] = {}
    descriptor_offset = pe_rva_to_offset(data, import_rva, sections, pe_offset)
    while True:
        if descriptor_offset + 20 > len(data):
            raise ValueError(f"Truncated PE import table: {path}")
        original_thunks, _timestamp, _forwarder, name_rva, thunks = struct.unpack_from(
            "<IIIII", data, descriptor_offset
        )
        if original_thunks == 0 and name_rva == 0 and thunks == 0:
            break
        if name_rva == 0:
            raise ValueError(f"Missing PE import DLL name: {path}")
        name_offset = pe_rva_to_offset(data, name_rva, sections, pe_offset)
        dll_name = read_ascii(data, name_offset).lower()

        thunk_rva = original_thunks or thunks
        if thunk_rva == 0:
            raise ValueError(f"Missing PE import thunk table: {dll_name}")
        thunk_offset = pe_rva_to_offset(data, thunk_rva, sections, pe_offset)
        while thunk_offset + 4 <= len(data):
            value = struct.unpack_from("<I", data, thunk_offset)[0]
            if value == 0:
                break
            if value & 0x80000000:
                thunk_offset += 4
                continue
            hint_name_offset = pe_rva_to_offset(data, value, sections, pe_offset)
            if hint_name_offset + 2 > len(data):
                raise ValueError(f"Truncated PE import name: {path}")
            name_offset = hint_name_offset + 2
            found.setdefault(dll_name, []).append(read_ascii(data, name_offset))
            thunk_offset += 4

        descriptor_offset += 20
    return found


def check_isolation_modules(root: Path) -> None:
    """Require the read-only folder-isolation shim import pattern."""
    require_file(root, "g3qa32.dll")
    for name in FOLDER_MODULES:
        module = require_file(root, name)
        try:
            found = imports(module)
        except (OSError, ValueError) as exc:
            raise RuntimeError(f"Invalid folder-sensitive module {module}: {exc}") from exc
        if "SHGetFolderPathA" not in found.get("g3qa32.dll", ()):
            raise RuntimeError(
                f"Unisolated folder import: {module} must import SHGetFolderPathA from g3qa32.dll"
            )
        for dll_name, functions in found.items():
            if dll_name == "g3qa32.dll":
                continue
            if any(
                function.startswith(("SHGetFolderPath", "SHGetKnownFolderPath"))
                for function in functions
            ):
                raise RuntimeError(
                    f"Native folder import remains in {module}: {dll_name}"
                )


def check_profile(root: Path) -> Path:
    profile_root = require_directory(root, "isolated-profile")
    profile = require_directory(root, "isolated-profile/Documents/gothic3")
    for path in profile_root.rglob("*"):
        reject_links(path)
        if path.is_file():
            if is_hard_linked(path):
                raise RuntimeError(f"Isolated profile file has hard links: {path}")
    return profile


def check_writable_hard_links(root: Path) -> None:
    names = (
        "g3together.ini",
        "g3together.log",
        "g3trace.log",
        "Ini/ge3.ini",
        "isolated-profile/Documents/gothic3/UserOptions.ini",
    )
    for name in names:
        path = reject_links(root / name)
        if path.exists() and path.is_file() and is_hard_linked(path):
            raise RuntimeError(f"Writable file has hard links: {path}")


def check_competing_mods(root: Path) -> None:
    scripts = require_directory(root, "scripts")
    competing = []
    for path in scripts.iterdir():
        if path.is_file() and "coop" in path.name.lower() and path.name.lower() != "script_g3together.dll":
            competing.append(path)
    if competing:
        names = ", ".join(path.name for path in competing)
        raise RuntimeError(f"Another co-op mod is active in {root}: {names}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def resolve_paths(args: argparse.Namespace) -> tuple[Path, list[Path], Path]:
    workspace = canonical(args.workspace) if args.workspace else canonical(
        Path(os.environ["G3_WORKSPACE"]) if os.environ.get("G3_WORKSPACE") else REPO / "workspace"
    )
    config = read_workspace_config(workspace)

    protected_value = args.protected_save_dir
    if not protected_value:
        protected_value = config.get("protected_save_dir")
    if not protected_value:
        raise RuntimeError(
            "Protected save directory is required: use --protected-save-dir or set "
            "protected_save_dir in workspace/project.json."
        )
    protected_candidate = Path(protected_value).expanduser()
    if not protected_candidate.is_absolute():
        raise RuntimeError("protected_save_dir must be an absolute path")
    protected = canonical(protected_candidate)

    copy_values = list(args.copy or ())
    if not copy_values:
        copy_values = [str(path) for path in configured_copies(config)]
    if not copy_values:
        raise RuntimeError(
            "No game copies supplied: use --copy twice for a host/client pair, or "
            "configure copies in workspace/project.json."
        )

    roots = []
    for value in copy_values:
        root = canonical(Path(value).expanduser())
        if root not in roots:
            roots.append(root)
    if len(roots) < 2:
        raise RuntimeError("Two distinct Gothic 3 copy roots are required for host/client preflight")
    return workspace, roots, protected


def preflight(args: argparse.Namespace) -> tuple[Path, list[Path], list[tuple[Path, Path]]]:
    workspace, roots, protected = resolve_paths(args)

    for index, root in enumerate(roots):
        if not root.is_dir():
            raise RuntimeError(f"Missing Gothic 3 copy root: {root}")
        if root.is_relative_to(protected) or protected.is_relative_to(root):
            raise RuntimeError(f"Copy overlaps the protected save directory: {root}")
        for other in roots[:index]:
            if overlaps(root, other):
                raise RuntimeError(f"Copy roots overlap: {root} and {other}")

    profiles: list[tuple[Path, Path]] = []
    hashes = {}
    for root in roots:
        require_file(root, "Gothic3.exe")
        mod = require_file(root, "scripts/Script_G3Together.dll")
        require_file(root, "G3Together/steam_api.dll")
        require_directory(root, "scripts")
        require_directory(root, "Ini")
        require_directory(root, "Data")
        require_file(root, "scripts/Script_paru.dll")
        check_isolation_modules(root)
        check_competing_mods(root)
        profile = check_profile(root)
        if overlaps(profile, protected):
            raise RuntimeError(f"Isolated save/profile overlaps the protected save directory: {profile}")
        for other_profile in (item[1] for item in profiles):
            if overlaps(profile, other_profile):
                raise RuntimeError(f"Isolated profiles overlap: {profile} and {other_profile}")
        check_writable_hard_links(root)
        hashes[root] = sha256(mod)
        profiles.append((root, profile))

    unique_hashes = set(hashes.values())
    if len(unique_hashes) != 1:
        mismatches = ", ".join(f"{root}: {hashes[root]}" for root in roots)
        raise RuntimeError(f"Mismatched G3 Together mod DLL hashes: {mismatches}")

    return workspace, roots, profiles


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Manual-only safety preflight for separate Gothic 3 co-op copies."
    )
    parser.add_argument("--workspace", type=Path, help="workspace containing project.json")
    parser.add_argument(
        "--protected-save-dir",
        type=Path,
        help="absolute protected save directory; defaults to protected_save_dir in project.json",
    )
    parser.add_argument(
        "--copy",
        action="append",
        type=Path,
        help="separate Gothic 3 copy root; repeat once per copy",
    )
    args = parser.parse_args(argv)

    try:
        workspace, roots, profiles = preflight(args)
    except (OSError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print(f"PASS: workspace={workspace}")
    for root, profile in profiles:
        print(f"PASS: copy={root}")
        print(f"PASS: isolated-profile={profile}")
    print("PASS: no copy/save overlap, link/reparse, hard-link, mod-hash, or competing-mod issue")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
