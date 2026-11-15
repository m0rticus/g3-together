"""Shared, read-only copy inspection and configuration helpers."""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import stat
import struct
import subprocess
from pathlib import Path
from typing import Iterable

DEFAULT_HOST = Path(r"C:\Repositories\g3-together\workspace\test-install")
DEFAULT_CLIENT = Path(r"C:\Repositories\g3-together\workspace\test-client")
DEFAULT_STEAM = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Gothic 3")
FOLDER_MODULES = ("Game.dll", "Engine.dll", "SharedBase.dll", "Dialogs.dll",
                  "paru_s2.dll", r"scripts\Script_paru.dll")
NATIVE_SLOTS = dict(zip(FOLDER_MODULES, (0x32F2E6, 0x4C0706, 0x9796C, 0x34874, 0x90DCC, 0x6E8F6)))
SLOT_QA = b"g3qa32.dll\x00\x00"
SLOT_GENUINE = b"SHELL32.dll\x00"
STEAM_DLL_SHA256 = "fdafb7c65ec7d4182b42bf92cc8671c68e4dc547cc6e613899d2618a47eb65da"
KNOWN_FAKE_SHA = "a3c85cda223e4bef581440f2f50da419e6d6c1218ee78cc2ebdbc88a998e13f3"
TEST_REALISM_BUNDLE_FILES = tuple("G3Together/test-realism/" + name for name in (
    "fake_steam_api.dll", "fake_steam_diag.h", "fake_steam_abi.h",
    "fake_steam_api.def", "build.ps1", "receipt.json", "run_tests.ps1",
    "README.md", "source/fake_steam_api.c", "source/fake_steam_test.c",
    "fake_steam_process_test.c", "fake_steam_contract_test.c",
    "fake_steam_test.exe", "fake_steam_process_test.exe",
    "fake_steam_contract_test.exe", "test-summary.txt", "profiles/ideal.ini",
    "profiles/steam-typical.ini", "profiles/steam-bad.ini",
))
PRODUCTION_INI_KEYS = frozenset((
    "role", "transport", "session_id", "connection", "steam_lobby", "steam_host",
    "legacy_companion_owner", "language", "puppet_template", "look", "partner_key",
    "trace", "reputation", "host_roi", "ardea_remove_npcs", "npc_authority_fallback",
    "host_sim", "puppet_lead", "motion_seek", "movement_smoothing", "send_rate",
    "network_budget_kib", "listener_textbox", "friendly_fire", "npc_damage",
    "npc_hp", "client_autoload",
))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def reject_links(path: Path, error_cls: type[Exception] = RuntimeError) -> Path:
    path = Path(os.path.abspath(path))
    for part in (path, *path.parents):
        if part.exists() or part.is_symlink():
            info = part.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400):
                raise error_cls(f"Link or reparse point is not allowed: {part}")
    return path


def canonical(path: Path, error_cls: type[Exception] = RuntimeError) -> Path:
    return reject_links(path, error_cls).resolve(strict=False)


def is_within(path: Path, root: Path) -> bool:
    path_text = os.path.normcase(os.path.abspath(path))
    root_text = os.path.normcase(os.path.abspath(root))
    try:
        return os.path.commonpath((path_text, root_text)) == root_text
    except ValueError:
        return False


def overlaps(left: Path, right: Path) -> bool:
    return is_within(left, right) or is_within(right, left)


def validate_copy_roots(
    host: Path,
    client: Path,
    steam: Path,
    fixture_roots: Iterable[Path] = (),
    error_cls: type[Exception] = RuntimeError,
) -> dict[str, Path]:
    allowed_fixtures = [canonical(path, error_cls) for path in fixture_roots]
    defaults = {"host": DEFAULT_HOST, "client": DEFAULT_CLIENT, "steam": DEFAULT_STEAM}
    roots: dict[str, Path] = {}
    for role, path in (("host", host), ("client", client), ("steam", steam)):
        root = canonical(path, error_cls)
        if allowed_fixtures:
            if not any(is_within(root, allowed) for allowed in allowed_fixtures):
                raise error_cls(f"{role} root is outside the explicitly allowed fixture roots: {root}")
        elif canonical(defaults[role], error_cls) != root:
            raise error_cls(f"{role} root must be explicitly authorized or supplied with --allow-fixture-root")
        if not root.is_dir():
            raise error_cls(f"Missing {role} root: {root}")
        roots[role] = root
    for role, root in roots.items():
        for other_role, other in roots.items():
            if role != other_role and overlaps(root, other):
                raise error_cls(f"Roots overlap: {role}={root}, {other_role}={other}")
    return roots


_WIN32_ENV_MEM_CACHE: dict[str, dict[str, str]] = {}


def win32_tool_environment(build: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["TEMP"] = env["TMP"] = str(build)
    if env.get("VSCMD_ARG_TGT_ARCH") == "x86" and shutil.which("cl", path=env.get("PATH")):
        return env
    vswhere = (Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
               / "Microsoft Visual Studio" / "Installer" / "vswhere.exe")
    if not vswhere.is_file():
        raise RuntimeError("Visual Studio vswhere is required to locate the Win32 toolchain")
    vs_stat = vswhere.stat()
    cache_key = hashlib.sha256(
        f"{vswhere}:{vs_stat.st_mtime_ns}:{vs_stat.st_size}:{env.get('PATH', '')}".encode("utf-8", errors="replace")
    ).hexdigest()[:20]
    cached_base = _WIN32_ENV_MEM_CACHE.get(cache_key)
    cache_dir = Path(__file__).resolve().parents[1] / "build" / "test-cache" / "env"
    cache_file = cache_dir / f"vcvars32-{cache_key}.json"
    if cached_base is None and cache_file.is_file():
        try:
            loaded = json.loads(cache_file.read_text(encoding="utf-8"))
            if isinstance(loaded, dict) and shutil.which("cl", path=loaded.get("PATH") or loaded.get("Path")):
                cached_base = {str(k): str(v) for k, v in loaded.items()}
                _WIN32_ENV_MEM_CACHE[cache_key] = cached_base
        except OSError:
            cached_base = None
    if cached_base is not None:
        out = cached_base.copy()
        out["TEMP"] = out["TMP"] = str(build)
        return out

    result = subprocess.run(
        [str(vswhere), "-latest", "-products", "*",
         "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"],
        capture_output=True, text=True, timeout=30, env=env)
    if result.returncode or not result.stdout.strip():
        raise RuntimeError("Visual Studio Win32 C++ tools not found: " + result.stderr)
    vcvars = Path(result.stdout.strip()) / "VC" / "Auxiliary" / "Build" / "vcvars32.bat"
    if not vcvars.is_file():
        raise RuntimeError("Win32 toolchain setup not found: " + str(vcvars))
    command = f'"{os.environ.get("COMSPEC", "cmd.exe")}" /d /s /c "call "{vcvars}" >nul && set"'
    result = subprocess.run(command, cwd=build, env=env, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError("Win32 toolchain setup failed:\n" + result.stdout + result.stderr)
    for line in result.stdout.splitlines():
        if "=" in line and not line.startswith("="):
            name, value = line.split("=", 1)
            for existing in list(env):
                if existing.lower() == name.lower() and existing != name:
                    del env[existing]
            env[name] = value
    _WIN32_ENV_MEM_CACHE[cache_key] = env.copy()
    try:
        cache_dir.mkdir(parents=True, exist_ok=True)
        tmp_file = cache_dir / f"vcvars32-{cache_key}.tmp.{os.getpid()}.json"
        tmp_file.write_text(json.dumps(env), encoding="utf-8")
        os.replace(tmp_file, cache_file)
    except OSError:
        pass
    env["TEMP"] = env["TMP"] = str(build)
    return env


def is_hard_linked(path: Path) -> bool:
    return path.stat(follow_symlinks=False).st_nlink != 1


def pe_rva_to_offset(data: bytes, rva: int, sections: list[tuple[int, int, int, int]], pe_offset: int = 0) -> int:
    if rva < 0:
        raise ValueError("Negative PE RVA")
    for virtual_address, virtual_size, raw_address, raw_size in sections:
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            delta = rva - virtual_address
            if delta >= raw_size or raw_address + delta >= len(data):
                raise ValueError("PE RVA maps outside the file")
            return raw_address + delta
    raise ValueError(f"Unmapped PE RVA 0x{rva:x}")


def parse_pe(path: Path) -> tuple[dict[str, list[str]], dict[str, object]]:
    data = path.read_bytes()

    def unpack(fmt: str, offset: int):
        try:
            return struct.unpack_from(fmt, data, offset)
        except struct.error as exc:
            raise ValueError(f"Truncated PE structure: {path}") from exc

    def ascii_at(offset: int) -> str:
        end = data.find(b"\0", offset)
        if offset < 0 or offset >= len(data) or end < 0:
            raise ValueError("Invalid PE string")
        try:
            return data[offset:end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise ValueError("Non-ASCII PE string") from exc

    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError(f"Not a PE file: {path}")
    pe = unpack("<I", 0x3c)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"Missing PE header: {path}")
    machine, count = unpack("<HH", pe + 4)
    size = unpack("<H", pe + 20)[0]
    opt = pe + 24
    if machine != 0x14c or size < 96 or opt + size > len(data) or unpack("<H", opt)[0] != 0x10b:
        raise ValueError(f"Invalid PE32 optional header: {path}")
    count_dirs = unpack("<I", opt + 92)[0]
    if count_dirs > (size - 96) // 8:
        raise ValueError(f"Truncated PE data directories: {path}")
    section_start = opt + size
    if section_start + count * 40 > len(data):
        raise ValueError(f"Truncated PE section table: {path}")
    sections = []
    for i in range(count):
        virtual_size, virtual_address, raw_size, raw_address = unpack("<IIII", section_start + i * 40 + 8)
        if raw_address + raw_size > len(data):
            raise ValueError(f"Section maps outside PE file: {path}")
        sections.append((virtual_address, virtual_size, raw_address, raw_size))

    def offset(rva: int) -> int:
        return pe_rva_to_offset(data, rva, sections, pe)

    def directory(index: int) -> tuple[int, int]:
        return unpack("<II", opt + 96 + index * 8) if count_dirs > index else (0, 0)

    imports: dict[str, list[str]] = {}
    import_rva, import_size = directory(1)
    if import_rva:
        if not import_size:
            raise ValueError("PE import table has RVA but zero size")
        descriptor = offset(import_rva)
        for _ in range(4096):
            original, timestamp, forwarder, name, thunks = unpack("<IIIII", descriptor)
            if not (original or timestamp or forwarder or name or thunks):
                break
            if not name or not (original or thunks):
                raise ValueError("Malformed PE import descriptor")
            dll = ascii_at(offset(name)).lower()
            thunk = offset(original or thunks)
            for _ in range(65536):
                value = unpack("<I", thunk)[0]
                if not value:
                    break
                symbol = f"<ordinal:{value & 0xffff}>" if value & 0x80000000 else ascii_at(offset(value) + 2)
                imports.setdefault(dll, []).append(symbol)
                thunk += 4
            else:
                raise ValueError("PE import thunk loop overflow")
            descriptor += 20
        else:
            raise ValueError("PE import descriptor loop overflow")
    export_rva, export_size = directory(0)
    exports: dict[str, object] = {}
    if export_rva:
        if not export_size:
            raise ValueError("PE export table has RVA but zero size")
        values = unpack("<IIHHIIIIIII", offset(export_rva))
        _, _, _, _, _, base, address_count, name_count, addresses, names, ordinals = values
        exports = {"ordinal_base": base, "address_count": address_count, "names": [], "named_count": name_count}
        for i in range(name_count):
            if not names or not ordinals or not addresses:
                raise ValueError("Malformed PE export name tables")
            name_rva = unpack("<I", offset(names + i * 4))[0]
            ordinal = unpack("<H", offset(ordinals + i * 2))[0]
            if ordinal >= address_count:
                raise ValueError("PE export ordinal outside address table")
            address = unpack("<I", offset(addresses + ordinal * 4))[0]
            exports["names"].append((ascii_at(offset(name_rva)), address))
    delay_rva, delay_size = directory(13)
    return imports, {"imports": imports, "exports": exports, "delay_import": bool(delay_rva or delay_size)}


def parse_g3t_ini(source: str | Path) -> tuple[dict[str, str], list[str]]:
    text = (
        source.read_text(encoding="utf-8-sig", errors="replace")
        if isinstance(source, Path)
        else source
    )
    scalars: dict[str, str] = {}
    owners: list[str] = []
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip().lower()
        if not key or key.startswith(("#", ";")):
            continue
        if key != "legacy_companion_owner":
            value = value.split("#", 1)[0]
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] == '"':
            value = value[1:-1]
        if key == "legacy_companion_owner":
            owners.append(value)
        else:
            scalars[key] = value
    return scalars, owners


def package_ini_templates() -> tuple[str, str]:
    from package import HOST_INI, CLIENT_INI
    return HOST_INI, CLIENT_INI
