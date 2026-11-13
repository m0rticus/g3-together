#!/usr/bin/env python3
"""Read-only verification for the isolated Gothic 3 test copies.

This tool never launches a game or writes to a target.  It can optionally write
a JSON report to an explicitly supplied path.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import stat
import struct
import sys
from pathlib import Path
from typing import Iterable


DEFAULT_HOST = Path(r"C:\Repositories\g3-together\workspace\test-install")
DEFAULT_CLIENT = Path(r"C:\Repositories\g3-together\workspace\test-client")
DEFAULT_STEAM = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Gothic 3")
WORKTREE = Path(__file__).resolve().parents[1]
PACKAGE_AUDIT = Path(__file__).resolve().parents[1] / "expected-installation.json"

FOLDER_MODULES = (
    "Game.dll",
    "Engine.dll",
    "SharedBase.dll",
    "Dialogs.dll",
    "paru_s2.dll",
    r"scripts\Script_paru.dll",
)

NATIVE_SLOTS = {
    "Game.dll": 0x32F2E6,
    "Engine.dll": 0x4C0706,
    "SharedBase.dll": 0x9796C,
    "Dialogs.dll": 0x34874,
    "paru_s2.dll": 0x90DCC,
    r"scripts\Script_paru.dll": 0x6E8F6,
}
SLOT_QA = b"g3qa32.dll\x00\x00"
SLOT_GENUINE = b"SHELL32.dll\x00"

CORE_SHA = {
    "scripts\\Script_G3Together.dll": "0fba732727c0f0467d8e2f6d7722760c4078f3a613c49d7e346f9fc5096eecb9",
    "Script_Game.dll": "cdd9c46120abac49ebb6d53e5bed79512a528e5940238d6d88d150fe9aef3cdc",
}


class VerificationError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def reject_links(path: Path) -> Path:
    path = Path(os.path.abspath(str(path)))
    current = path
    while True:
        if current.exists() or current.is_symlink():
            info = current.lstat()
            reparse = getattr(info, "st_file_attributes", 0) & getattr(
                stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400
            )
            if stat.S_ISLNK(info.st_mode) or reparse:
                raise VerificationError(
                    f"Link or reparse point is not allowed: {current}"
                )
        if current.parent == current:
            break
        current = current.parent
    return path


def canonical(path: Path) -> Path:
    return reject_links(path).resolve(strict=False)


def overlaps(left: Path, right: Path) -> bool:
    left_text = os.path.normcase(str(left))
    right_text = os.path.normcase(str(right))
    try:
        return left == right or left.is_relative_to(right) or right.is_relative_to(left)
    except AttributeError:
        return (
            left_text == right_text
            or left_text.startswith(right_text + os.sep)
            or right_text.startswith(left_text + os.sep)
        )


def validate_roots(
    host: Path,
    client: Path,
    steam: Path,
    fixture_roots: Iterable[Path] = (),
) -> dict[str, Path]:
    fixture_roots = [canonical(path) for path in fixture_roots]
    roots: dict[str, Path] = {}
    for role, path in (("host", host), ("client", client), ("steam", steam)):
        root = canonical(path)
        if fixture_roots:
            if not any(root == allowed or overlaps(root, allowed) for allowed in fixture_roots):
                raise VerificationError(
                    f"{role} root is outside the explicitly allowed fixture roots: {root}"
                )
        else:
            allowed = {
                "host": DEFAULT_HOST,
                "client": DEFAULT_CLIENT,
                "steam": DEFAULT_STEAM,
            }[role]
            if canonical(allowed) != root:
                raise VerificationError(
                    f"{role} root must be explicitly authorized or supplied with --allow-fixture-root"
                )
        if not root.is_dir():
            raise VerificationError(f"Missing {role} root: {root}")
        roots[role] = root

    for role, root in roots.items():
        for other_role, other in roots.items():
            if role != other_role and overlaps(root, other):
                raise VerificationError(
                    f"Roots overlap: {role}={root}, {other_role}={other}"
                )
    return roots


def pe_rva_to_offset(
    data: bytes,
    rva: int,
    sections: list[tuple[int, int, int, int]],
) -> int:
    if rva < 0:
        raise ValueError("Negative PE RVA")
    for virtual_address, virtual_size, raw_address, raw_size in sections:
        mapped_size = max(virtual_size, raw_size)
        if virtual_address <= rva < virtual_address + mapped_size:
            offset = raw_address + rva - virtual_address
            if offset < 0 or offset >= len(data):
                raise ValueError("PE RVA maps outside the file")
            return offset
    raise ValueError(f"Unmapped PE RVA 0x{rva:x}")


def read_ascii(data: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(data):
        raise ValueError("PE string offset outside file")
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("Unterminated PE import name")
    try:
        return data[offset:end].decode("ascii")
    except UnicodeDecodeError as exc:
        raise ValueError("Non-ASCII PE import name") from exc


def parse_pe(path: Path) -> tuple[dict[str, list[str]], dict[str, object]]:
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError(f"Not a PE file: {path}")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset > len(data) - 24 or data[pe_offset:pe_offset + 4] != b"PE\0\0":
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
    if section_offset + section_count * 40 > len(data):
        raise ValueError(f"Truncated PE section table: {path}")
    sections: list[tuple[int, int, int, int]] = []
    for index in range(section_count):
        header = section_offset + 40 * index
        virtual_size = struct.unpack_from("<I", data, header + 8)[0]
        virtual_address = struct.unpack_from("<I", data, header + 12)[0]
        raw_size = struct.unpack_from("<I", data, header + 16)[0]
        raw_address = struct.unpack_from("<I", data, header + 20)[0]
        if raw_address + raw_size > len(data):
            raise ValueError(f"Section maps outside PE file: {path}")
        sections.append((virtual_address, virtual_size, raw_address, raw_size))

    number_of_rvas = struct.unpack_from("<I", data, optional_offset + 92)[0]
    imports: dict[str, list[str]] = {}
    data_directory_rva = optional_offset + 96
    if number_of_rvas < 2:
        data_directory_rva = optional_offset + 96

    import_rva = struct.unpack_from("<I", data, optional_offset + 104)[0]
    import_size = struct.unpack_from("<I", data, optional_offset + 108)[0]
    if import_rva:
        if import_size == 0:
            raise ValueError("PE import table has RVA but zero size")
        descriptor_offset = pe_rva_to_offset(data, import_rva, sections)
        for _ in range(4096):
            if descriptor_offset + 20 > len(data):
                raise ValueError(f"Truncated PE import table: {path}")
            original_thunks, _timestamp, _forwarder, name_rva, thunks = (
                struct.unpack_from("<IIIII", data, descriptor_offset)
            )
            if (
                original_thunks == 0
                and name_rva == 0
                and thunks == 0
            ):
                break
            if name_rva == 0:
                raise ValueError(f"Missing PE import DLL name: {path}")
            name_offset = pe_rva_to_offset(data, name_rva, sections)
            dll_name = read_ascii(data, name_offset).lower()
            thunk_rva = original_thunks or thunks
            if thunk_rva == 0:
                raise ValueError(f"Missing PE import thunk table: {dll_name}")
            thunk_offset = pe_rva_to_offset(data, thunk_rva, sections)
            for _ in range(65536):
                if thunk_offset + 4 > len(data):
                    raise ValueError(f"Truncated PE import thunk table: {path}")
                value = struct.unpack_from("<I", data, thunk_offset)[0]
                if value == 0:
                    break
                if value & 0x80000000:
                    ordinal = value & 0xFFFF
                    imports.setdefault(dll_name, []).append(f"<ordinal:{ordinal}>")
                else:
                    hint_name_offset = pe_rva_to_offset(data, value, sections)
                    if hint_name_offset + 2 > len(data):
                        raise ValueError(f"Truncated PE import name: {path}")
                    import_name = read_ascii(data, hint_name_offset + 2)
                    imports.setdefault(dll_name, []).append(import_name)
                thunk_offset += 4
            descriptor_offset += 20
        else:
            raise ValueError(f"PE import table descriptor loop overflow: {path}")

    delay_rva = 0
    delay_size = 0
    if number_of_rvas > 13:
        delay_rva = struct.unpack_from("<I", data, optional_offset + 208)[0]
        delay_size = struct.unpack_from("<I", data, optional_offset + 212)[0]

    exports: dict[str, object] = {}
    export_rva = struct.unpack_from("<I", data, optional_offset + 96)[0]
    export_size = struct.unpack_from("<I", data, optional_offset + 100)[0]
    if export_rva:
        if export_size == 0:
            raise ValueError("PE export table has RVA but zero size")
        export_offset = pe_rva_to_offset(data, export_rva, sections)
        if export_offset + 40 > len(data):
            raise ValueError(f"Truncated PE export table: {path}")
        (
            _flags,
            _timestamp,
            _major,
            _minor,
            name_rva,
            ordinal_base,
            address_count,
            name_count,
            address_rva,
            names_rva,
            ordinals_rva,
        ) = struct.unpack_from("<IIHHIIIIIII", data, export_offset)
        exports["ordinal_base"] = ordinal_base
        exports["address_count"] = address_count
        exports["names"] = []
        for index in range(name_count):
            if names_rva == 0 or ordinals_rva == 0 or address_rva == 0:
                raise ValueError("Malformed PE export name tables")
            name_entry_offset = pe_rva_to_offset(data, names_rva + 4 * index, sections)
            name_entry_rva = struct.unpack_from("<I", data, name_entry_offset)[0]
            name_offset = pe_rva_to_offset(data, name_entry_rva, sections)
            export_name = read_ascii(data, name_offset)
            ordinal_offset = pe_rva_to_offset(data, ordinals_rva + 2 * index, sections)
            ordinal = struct.unpack_from("<H", data, ordinal_offset)[0]
            if ordinal >= address_count:
                raise ValueError("PE export ordinal outside address table")
            address_offset = pe_rva_to_offset(
                data, address_rva + 4 * ordinal, sections
            )
            export_address = struct.unpack_from("<I", data, address_offset)[0]
            exports["names"].append((export_name, export_address))
        exports["named_count"] = name_count

    metadata = {
        "imports": imports,
        "exports": exports,
        "delay_import": bool(delay_rva or delay_size),
    }
    return imports, metadata


def parse_ini(path: Path) -> dict[str, str]:
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = raw.decode("cp1252")
    result: dict[str, str] = {}
    section = ""
    for line_number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", ";")):
            continue
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1].strip().lower()
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip().lower()
        if not key:
            raise VerificationError(f"Empty INI key in {path}:{line_number}")
        if section:
            key = f"{section}.{key}"
        if key in result:
            raise VerificationError(
                f"Duplicate active INI key {key!r} in {path}:{line_number}"
            )
        result[key] = value.strip().strip('"')
    return result


def extract_package_ini() -> tuple[str, str]:
    package_path = WORKTREE / "tools" / "package.py"
    tree = ast.parse(package_path.read_text(encoding="utf-8"), filename=str(package_path))
    constants: dict[str, str] = {}
    for node in tree.body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id in ("HOST_INI", "CLIENT_INI"):
                    if isinstance(node.value, ast.Constant) and isinstance(
                        node.value.value, str
                    ):
                        constants[target.id] = node.value.value
    if set(constants) != {"HOST_INI", "CLIENT_INI"}:
        raise VerificationError("Could not safely read HOST_INI/CLIENT_INI from tools/package.py")
    return constants["HOST_INI"], constants["CLIENT_INI"]


def load_audit(path: Path) -> dict:
    reject_links(path)
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise VerificationError(f"Invalid expected installation JSON: {path}") from exc


def is_deep_path(relative: str) -> bool:
    normalized = relative.lower().replace("/", "\\")
    return normalized.startswith("data\\") or normalized.startswith("data/")


def inventory_rows(
    audit: dict,
    role: str,
    deep: bool,
) -> list[tuple[str, str, int]]:
    result = []
    inventory = audit["hash_inventory"]
    for relative, roles, size, expected_hash in inventory["rows"]:
        role_list = [item.strip() for item in roles.split(",")]
        if role not in role_list:
            continue
        if not deep and is_deep_path(relative):
            continue
        result.append((relative, expected_hash, size))
    return result


def known_missing(audit: dict, role: str, relative: str) -> bool:
    normalized_relative = relative.lower().replace("/", "\\")
    for item in audit["hash_inventory"].get("known_missing", []):
        left, right = item.split("\\", 1) if "\\" in item else ("", item)
        if role in [part.strip() for part in left.split(",")]:
            if right.lower().replace("/", "\\") == normalized_relative:
                return True
            if right.endswith("*"):
                prefix = right[:-1].lower().replace("/", "\\")
                if normalized_relative.startswith(prefix):
                    return True
    return False


def add_row(
    rows: list[list[str]],
    check: str,
    status: str,
    host: str,
    client: str,
    detail: str,
) -> None:
    rows.append([check, status, host, client, detail])


def verify_inventory(
    audit: dict,
    roots: dict[str, Path],
    deep: bool,
    approved_mod_sha: str | None = None,
    approved_mod_size: int | None = None,
) -> tuple[list[list[str]], list[str]]:
    rows: list[list[str]] = []
    failures: list[str] = []
    for role, root in roots.items():
        for relative, expected_hash, expected_size in inventory_rows(audit, role, deep):
            if (
                relative.lower().replace("/", "\\")
                == r"scripts\Script_G3Together.dll".lower()
                and approved_mod_sha
            ):
                expected_hash = approved_mod_sha
                if approved_mod_size is not None:
                    expected_size = approved_mod_size
            path = reject_links(root / relative)
            if not path.is_file():
                if role != "steam" and known_missing(audit, role, relative):
                    continue
                failures.append(f"Missing expected file {role}:{relative}")
                continue
            actual_size = path.stat().st_size
            actual_hash = sha256(path)
            if actual_size != expected_size:
                failures.append(
                    f"Size mismatch {role}:{relative}: {actual_size} != {expected_size}"
                )
            if actual_hash != expected_hash:
                failures.append(
                    f"SHA mismatch {role}:{relative}: {actual_hash} != {expected_hash}"
                )

    add_row(
        rows,
        "Full audit inventory",
        "PASS" if not failures else "FAIL",
        "checked" if not failures else "mismatch",
        "checked" if not failures else "mismatch",
        f"{'--deep' if deep else 'core'}; 26 Data archives/9 videos included only with --deep",
    )
    return rows, failures


def verify_native_slots(
    roots: dict[str, Path],
    audit: dict,
) -> tuple[list[list[str]], list[str]]:
    rows: list[list[str]] = []
    failures: list[str] = []
    inventory = {row[0]: row for row in audit["hash_inventory"]["rows"]}

    for relative, offset in NATIVE_SLOTS.items():
        for role in ("host", "client"):
            root = roots[role]
            qa_path = reject_links(root / relative)
            genuine_path = reject_links(roots["steam"] / relative)
            if not qa_path.is_file():
                failures.append(f"Missing native module {role}:{relative}")
                continue
            if not genuine_path.is_file():
                failures.append(f"Missing genuine reference steam:{relative}")
                continue
            qa = qa_path.read_bytes()
            genuine = genuine_path.read_bytes()
            if len(qa) != len(genuine):
                failures.append(
                    f"Native size mismatch {role}:{relative}: QA={len(qa)}, genuine={len(genuine)}"
                )
                continue
            reconstructed = qa[:offset] + SLOT_GENUINE + qa[offset + 12:]
            if reconstructed != genuine:
                mismatches = sum(a != b for a, b in zip(qa, genuine))
                failures.append(
                    f"Native isolation proof failed {role}:{relative}: "
                    f"restoring 12-byte slot does not equal genuine bytes "
                    f"(direct differing bytes={mismatches})"
                )
                continue
            if qa[offset:offset + 12] != SLOT_QA:
                failures.append(
                    f"Unexpected isolation slot {role}:{relative}: "
                    f"0x{qa[offset:offset+12].hex()}"
                )

    add_row(
        rows,
        "Native 12-byte isolation slots",
        "FAIL" if failures else "PASS",
        "6/6" if not failures else "failed",
        "6/6" if not failures else "failed",
        "exact SLOT_QA to SLOT_GENUINE proof; no blind normalized replacement",
    )
    return rows, failures


def verify_shim_and_imports(
    roots: dict[str, Path],
) -> tuple[list[list[str]], list[str]]:
    rows: list[list[str]] = []
    failures: list[str] = []
    for role in ("host", "client"):
        root = roots[role]
        shim = reject_links(root / "g3qa32.dll")
        if not shim.is_file():
            failures.append(f"Missing shim {role}:g3qa32.dll")
            continue
        try:
            _imports, metadata = parse_pe(shim)
            exports = metadata["exports"]
            names = exports.get("names", [])
            if names != [("SHGetFolderPathA", 0x1000)]:
                failures.append(
                    f"Unexpected shim export shape {role}: "
                    f"{names!r}; expected [('SHGetFolderPathA', 0x1000)] "
                    "(export RVA; 0x10001000 is PE preferred VA)"
                )
        except (OSError, ValueError) as exc:
            failures.append(f"Invalid shim PE {role}:g3qa32.dll: {exc}")
            continue

        for relative in FOLDER_MODULES:
            path = reject_links(root / relative)
            if not path.is_file():
                failures.append(f"Missing isolated module {role}:{relative}")
                continue
            try:
                imports, metadata = parse_pe(path)
            except (OSError, ValueError) as exc:
                failures.append(f"Invalid PE {role}:{relative}: {exc}")
                continue
            if metadata["delay_import"]:
                failures.append(
                    f"Delay imports present but unreviewed {role}:{relative}"
                )
            if "SHGetFolderPathA" not in imports.get("g3qa32.dll", []):
                failures.append(
                    f"Unisolated folder import {role}:{relative}: "
                    "must import SHGetFolderPathA from g3qa32.dll"
                )
            for dll_name, functions in imports.items():
                if dll_name == "g3qa32.dll":
                    continue
                if any(
                    name.startswith(
                        ("SHGetFolderPath", "SHGetKnownFolderPath")
                    )
                    for name in functions
                ):
                    failures.append(
                        f"Native folder import remains {role}:{relative}: "
                        f"{dll_name}"
                    )

    add_row(
        rows,
        "Direct folder imports / shim",
        "FAIL" if failures else "PASS",
        "6/6" if not failures else "failed",
        "6/6" if not failures else "failed",
        "static PE evidence; dynamic folder APIs remain UNPROVEN",
    )
    return rows, failures


def verify_scripts(root: Path, role: str) -> list[str]:
    failures: list[str] = []
    scripts = reject_links(root / "scripts")
    if not scripts.is_dir():
        return [f"Missing scripts directory {role}:{scripts}"]
    matches = [
        path.name
        for path in scripts.iterdir()
        if path.is_file() and path.name.lower().startswith("script")
        and path.suffix.lower() == ".dll"
    ]
    allowed = {"Script_G3Together.dll", "Script_Game.dll", "Script_paru.dll"}
    if sum(name.lower() == "script_g3together.dll" for name in matches) != 1:
        failures.append(
            f"Expected exactly one Script_G3Together.dll in {role}; found {matches}"
        )
    extras = [name for name in matches if name not in allowed]
    if extras:
        failures.append(
            f"Unrecognized Script*.dll entries in {role}: {extras}"
        )
    return failures


def verify_profile_and_template(
    roots: dict[str, Path],
) -> tuple[list[list[str]], list[str], list[str]]:
    rows: list[list[str]] = []
    failures: list[str] = []
    warnings: list[str] = []
    for role in ("host", "client"):
        root = roots[role]
        profile = reject_links(root / r"isolated-profile\Documents\gothic3")
        if not profile.is_dir():
            failures.append(f"Missing isolated profile directory {role}:{profile}")
            continue
        for candidate in profile.iterdir():
            if candidate.is_file():
                reject_links(candidate)

    host_profile = reject_links(
        roots["host"] / r"isolated-profile\Documents\gothic3\UserOptions.ini"
    )
    client_profile = reject_links(
        roots["client"] / r"isolated-profile\Documents\gothic3\UserOptions.ini"
    )
    difficulty: dict[str, str] = {}
    for role, path in (("host", host_profile), ("client", client_profile)):
        if not path.is_file():
            failures.append(f"Missing isolated UserOptions.ini {role}:{path}")
            continue
        ini = parse_ini(path)
        key = "options.difficulty.difficulty"
        difficulty[role] = ini.get(key, "")
    if difficulty.get("host") != "Medium":
        failures.append(
            f"Host isolated difficulty mismatch: {difficulty.get('host')!r}"
        )
    if difficulty.get("client") != "Easy":
        failures.append(
            f"Client isolated difficulty mismatch: {difficulty.get('client')!r}"
        )
    if difficulty:
        warnings.append(
            f"Isolated host/client difficulty: {difficulty.get('host')}/{difficulty.get('client')}"
        )

    template_dir = reject_links(
        roots["client"] / r"isolated-profile\Documents\gothic3"
    )
    marker = reject_links(template_dir / "G3T_client_template.g3t")
    template_header = reject_links(template_dir / "G3T_client_template.g3savcp")
    template_world = reject_links(template_dir / "G3T_client_template.g3savcpdat")
    if not all(path.is_file() for path in (marker, template_header, template_world)):
        failures.append("Client template is missing; warm rejoin is not verified")
    else:
        if marker.stat().st_size != 116:
            failures.append("Client template marker size mismatch")
        if template_header.stat().st_size != 65708:
            failures.append("Client template header size mismatch")
        if template_world.stat().st_size != 1958922:
            failures.append("Client template world size mismatch")
        text = marker.read_text(encoding="utf-8", errors="replace").lower()
        if not all(
            token in text
            for token in (
                "g3t client template 2",
                "protocol=77",
                "origin=new-game",
            )
        ):
            failures.append(
                f"Client template marker lacks v2/protocol77/new-game: {text[:200]!r}"
            )

    add_row(
        rows,
        "Isolated profile/template",
        "FAIL" if failures else "PASS",
        "Medium" if difficulty.get("host") else "unknown",
        "Easy" if difficulty.get("client") else "unknown",
        "preserved; no profile/save writes",
    )
    return rows, failures, warnings


def verify_ini(
    roots: dict[str, Path],
    strict: bool,
) -> tuple[list[list[str]], list[str], list[str]]:
    rows: list[list[str]] = []
    failures: list[str] = []
    warnings: list[str] = []
    try:
        host_template, client_template = extract_package_ini()
        host_template_ini = parse_ini_text(host_template)
        client_template_ini = parse_ini_text(client_template)
    except (OSError, SyntaxError, VerificationError) as exc:
        failures.append(f"Could not read shipped INI constants safely: {exc}")
        host_template_ini = {}
        client_template_ini = {}

    for role in ("host", "client"):
        path = reject_links(roots[role] / "g3together.ini")
        if not path.is_file():
            failures.append(f"Missing g3together.ini {role}:{path}")
            continue
        try:
            ini = parse_ini(path)
        except VerificationError as exc:
            failures.append(str(exc))
            continue
        if ini.get("role") != role:
            failures.append(
                f"Wrong role in {role} g3together.ini: {ini.get('role')!r}"
            )
        if ini.get("transport") != "steam":
            failures.append(
                f"Wrong transport in {role} g3together.ini: "
                f"{ini.get('transport')!r}"
            )
        missing = [
            key
            for key in ("connection", "session_id", "language")
            if key not in ini
        ]
        if strict and missing:
            failures.append(
                f"Strict shipped INI missing explicit keys {role}: {missing}"
            )
        elif missing:
            warnings.append(
                f"{role} g3together.ini relies on defaults for: {missing}"
            )
        if "session_id" in ini and ini["session_id"] != "default":
            warnings.append(
                f"{role} g3together.ini session_id={ini['session_id']}"
            )

    ge3_host = reject_links(roots["host"] / r"Ini\ge3.ini")
    ge3_client = reject_links(roots["client"] / r"Ini\ge3.ini")
    ge3_steam = reject_links(roots["steam"] / r"Ini\ge3.ini")
    if ge3_host.is_file() and ge3_client.is_file() and ge3_steam.is_file():
        try:
            steam = parse_ini(ge3_steam)
            host = parse_ini(ge3_host)
            client = parse_ini(ge3_client)
            for key, steam_value in (
                ("window.fullscreen", steam.get("window.fullscreen")),
                ("window.capturecursor", steam.get("window.capturecursor")),
                ("game.testmode", steam.get("game.testmode")),
            ):
                qa_values = {host.get(key), client.get(key)}
                if qa_values != {steam_value}:
                    warnings.append(
                        f"Intentional ge3.ini difference {key}: "
                        f"Steam={steam_value}, QA={sorted(v for v in qa_values if v)}"
                    )
        except VerificationError as exc:
            failures.append(str(exc))
    else:
        failures.append("Missing ge3.ini required for Steam/QA comparison")

    add_row(
        rows,
        "INI role/transport/explicitness",
        "FAIL" if failures else ("PASS WITH WARN" if warnings else "PASS"),
        "host",
        "client",
        "strict" if strict else "default policy; intentional differences WARN",
    )
    return rows, failures, warnings


def parse_ini_text(text: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", ";")):
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip().lower()
        if key in result:
            raise VerificationError(f"Duplicate key in package INI: {key}")
        result[key] = value.strip()
    return result


def verify_hardlinks(
    roots: dict[str, Path],
    audit: dict,
    deep: bool,
) -> tuple[list[list[str]], list[str], list[str]]:
    rows: list[list[str]] = []
    failures: list[str] = []
    warnings: list[str] = []
    count = 0
    for relative, roles, size, _hash in audit["hash_inventory"]["rows"]:
        normalized = relative.lower().replace("/", "\\")
        if not (normalized.startswith("data\\") or normalized.lower().endswith(".bik")):
            continue
        for role in ("host", "client", "steam"):
            if role not in [item.strip() for item in roles.split(",")]:
                continue
            path = reject_links(roots[role] / relative)
            if not path.is_file():
                continue
            count += 1
            try:
                link_count = path.stat(follow_symlinks=False).st_nlink
            except TypeError:
                link_count = path.lstat().st_nlink
            if role in ("host", "client") and link_count != 2:
                if deep:
                    warnings.append(
                        f"Expected observed nlink=2 for immutable {role}:{relative}; got {link_count}"
                    )
    warnings.append(
        f"Immutable hardlink evidence inspected for {count} active paths; never edit in place"
    )
    add_row(
        rows,
        "Data/video immutable hardlinks",
        "PASS WITH NOTE" if warnings and not failures else "PASS",
        f"{count} paths",
        f"{count} paths",
        "nlink=2 is evidence, not a write authorization",
    )
    return rows, failures, warnings


def verify_mod_receipt(
    roots: dict[str, Path],
    approved_mod_sha: str | None,
    approved_mod_size: int | None,
    tested_run_sha: str | None,
    tested_run_time: str | None,
) -> tuple[list[list[str]], list[str], list[str]]:
    actual: dict[str, dict[str, object]] = {}
    failures: list[str] = []
    warnings: list[str] = []
    for role in ("host", "client", "steam"):
        path = reject_links(roots[role] / r"scripts\Script_G3Together.dll")
        if not path.is_file():
            failures.append(f"Missing mod receipt file {role}:scripts\\Script_G3Together.dll")
            continue
        actual[role] = {
            "sha256": sha256(path),
            "size": path.stat().st_size,
        }

    if approved_mod_sha:
        approved_mod_sha = approved_mod_sha.lower()
        mismatches = [
            f"{role} sha={item['sha256']} size={item['size']}"
            for role, item in actual.items()
            if item["sha256"] != approved_mod_sha
        ]
        if approved_mod_size is not None:
            mismatches.extend(
                f"{role} size={item['size']} != approved size={approved_mod_size}"
                for role, item in actual.items()
                if item["size"] != approved_mod_size
            )
        if mismatches:
            failures.append(
                "Approved mod receipt mismatch: " + "; ".join(mismatches)
            )
            status = "FAIL"
            detail = "disk mod differs from separately approved receipt"
        else:
            status = "PASS WITH NOTE"
            detail = "disk mod matches approved receipt"
            if not tested_run_sha or not tested_run_time:
                warnings.append(
                    "Approved disk mod receipt exists; fresh tested-run SHA/time remains NOT TESTED"
                )
                status = "PASS WITH NOTE"
                detail += "; runtime exercised build/time NOT TESTED"
            else:
                warnings.append(
                    f"Tested-run receipt SHA={tested_run_sha} time={tested_run_time}; "
                    f"disk SHA={approved_mod_sha}"
                )
    else:
        current = {
            role: f"{item['sha256']} ({item['size']} bytes)"
            for role, item in actual.items()
        }
        failures.append(
            "Mod disk receipt is unapproved/unknown: " + "; ".join(
                f"{role}={value}" for role, value in current.items()
            )
        )
        status = "FAIL"
        detail = "unknown disk mod; no separately approved build receipt"

    rows = [[
        "Mod disk/tested-run receipt",
        status,
        "approved" if approved_mod_sha else "unknown",
        "runtime not exercised" if not tested_run_sha else "tested receipt supplied",
        detail,
    ]]
    return rows, failures, warnings


def print_table(rows: list[list[str]], report_status: str) -> None:
    print("CHECK | STATUS | HOST | CLIENT | DETAIL")
    for row in rows:
        print(" | ".join(row))
    print(f"OVERALL | {report_status}")


def write_json_report(path: Path, report: dict) -> None:
    reject_links(path.parent)
    path.parent.mkdir(parents=True, exist_ok=True)
    reject_links(path.parent)
    path.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Read-only verifier for isolated Gothic 3 host/client copies."
    )
    parser.add_argument("--host-root", "--host", type=Path, default=DEFAULT_HOST)
    parser.add_argument("--client-root", "--client", type=Path, default=DEFAULT_CLIENT)
    parser.add_argument("--steam-root", "--steam", type=Path, default=DEFAULT_STEAM)
    parser.add_argument(
        "--expected",
        type=Path,
        default=PACKAGE_AUDIT,
        help="expected installation JSON (defaults to package-adjacent expected-installation.json)",
    )
    parser.add_argument("--approved-mod-sha256")
    parser.add_argument("--approved-mod-size", type=int)
    parser.add_argument("--tested-run-sha256")
    parser.add_argument("--tested-run-time")
    parser.add_argument(
        "--allow-fixture-root",
        action="append",
        type=Path,
        default=[],
        help="explicit synthetic fixture root; repeat as needed",
    )
    parser.add_argument("--deep", action="store_true")
    parser.add_argument("--strict-shipped-ini", action="store_true")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)

    try:
        roots = validate_roots(
            args.host_root,
            args.client_root,
            args.steam_root,
            args.allow_fixture_root,
        )
        audit = load_audit(args.expected)

        all_rows: list[list[str]] = []
        all_failures: list[str] = []
        all_warnings: list[str] = []

        inventory_rows_out, failures = verify_inventory(
            audit,
            roots,
            args.deep,
            args.approved_mod_sha256,
            args.approved_mod_size,
        )
        all_rows.extend(inventory_rows_out)
        all_failures.extend(failures)

        mod_rows, failures, warnings = verify_mod_receipt(
            roots,
            args.approved_mod_sha256,
            args.approved_mod_size,
            args.tested_run_sha256,
            args.tested_run_time,
        )
        all_rows.extend(mod_rows)
        all_failures.extend(failures)
        all_warnings.extend(warnings)

        native_rows, failures = verify_native_slots(roots, audit)
        all_rows.extend(native_rows)
        all_failures.extend(failures)

        shim_rows, failures = verify_shim_and_imports(roots)
        all_rows.extend(shim_rows)
        all_failures.extend(failures)

        script_failures: list[str] = []
        for role in ("host", "client"):
            role_failures = verify_scripts(roots[role], role)
            script_failures.extend(role_failures)
            all_failures.extend(role_failures)
        all_rows.append(
            [
                "Script*.dll shadow check",
                "FAIL" if script_failures else "PASS",
                "PASS" if not any("host" in item for item in script_failures) else "FAIL",
                "PASS" if not any("client" in item for item in script_failures) else "FAIL",
                "exactly one Script_G3Together.dll; no extra unrecognized Script*.dll",
            ]
        )

        ini_rows, failures, warnings = verify_ini(
            roots, args.strict_shipped_ini
        )
        all_rows.extend(ini_rows)
        all_failures.extend(failures)
        all_warnings.extend(warnings)

        profile_rows, failures, warnings = verify_profile_and_template(roots)
        all_rows.extend(profile_rows)
        all_failures.extend(failures)
        all_warnings.extend(warnings)

        link_rows, failures, warnings = verify_hardlinks(
            roots, audit, args.deep
        )
        all_rows.extend(link_rows)
        all_failures.extend(failures)
        all_warnings.extend(warnings)

        status = "FAIL" if all_failures else "PASS"
        print_table(all_rows, status)
        if all_warnings:
            print("NOTES")
            for warning in all_warnings:
                print(f"WARN: {warning}")
        if all_failures:
            print("FAILURES")
            for failure in all_failures:
                print(f"FAIL: {failure}")

        report = {
            "status": status,
            "strict_shipped_ini": args.strict_shipped_ini,
            "deep": args.deep,
            "roots": {role: str(path) for role, path in roots.items()},
            "rows": all_rows,
            "warnings": all_warnings,
            "failures": all_failures,
        }
        if args.json:
            write_json_report(args.json, report)
            print(f"JSON_REPORT: {args.json}")
        return 1 if all_failures else 0
    except (OSError, VerificationError, KeyError, json.JSONDecodeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
