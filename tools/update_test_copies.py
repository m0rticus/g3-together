#!/usr/bin/env python3
"""Dry-run and explicit-apply updater for isolated Gothic 3 test copies.

Only g3together.ini and the owned G3Together/test-realism transport bundle are
update candidates. The genuine G3Together/steam_api.dll API is preserved.
Native modules, Data, video, saves and templates are never touched.
"""

from __future__ import annotations

import argparse
import ast
import datetime as dt
import hashlib
import json
import os
import stat
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Iterable


DEFAULT_HOST = Path(r"C:\Repositories\g3-together\workspace\test-install")
DEFAULT_CLIENT = Path(r"C:\Repositories\g3-together\workspace\test-client")
DEFAULT_STEAM = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Gothic 3")
WORKTREE = Path(__file__).resolve().parents[1]
KNOWN_FAKE_SHA = "a3c85cda223e4bef581440f2f50da419e6d6c1218ee78cc2ebdbc88a998e13f3"
ALLOWED_DESTINATIONS = {
    "g3together.ini",
    "G3Together/test-realism/fake_steam_api.dll",
    "G3Together/test-realism/fake_steam_diag.h",
    "G3Together/test-realism/fake_steam_abi.h",
    "G3Together/test-realism/fake_steam_api.def",
    "G3Together/test-realism/build.ps1",
    "G3Together/test-realism/receipt.json",
    "G3Together/test-realism/run_tests.ps1",
    "G3Together/test-realism/README.md",
    "G3Together/test-realism/source/fake_steam_api.c",
    "G3Together/test-realism/source/fake_steam_test.c",
    "G3Together/test-realism/fake_steam_process_test.c",
    "G3Together/test-realism/fake_steam_contract_test.c",
    "G3Together/test-realism/fake_steam_test.exe",
    "G3Together/test-realism/fake_steam_process_test.exe",
    "G3Together/test-realism/fake_steam_contract_test.exe",
    "G3Together/test-realism/test-summary.txt",
    "G3Together/test-realism/profiles/ideal.ini",
    "G3Together/test-realism/profiles/steam-typical.ini",
    "G3Together/test-realism/profiles/steam-bad.ini",
}


class UpdateError(RuntimeError):
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
                raise UpdateError(f"Link or reparse point is not allowed: {current}")
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
                raise UpdateError(
                    f"{role} root is outside the explicitly allowed fixture roots: {root}"
                )
        else:
            allowed = {
                "host": DEFAULT_HOST,
                "client": DEFAULT_CLIENT,
                "steam": DEFAULT_STEAM,
            }[role]
            if canonical(allowed) != root:
                raise UpdateError(
                    f"{role} root must be explicitly authorized or supplied with --allow-fixture-root"
                )
        if not root.is_dir():
            raise UpdateError(f"Missing {role} root: {root}")
        roots[role] = root

    for role, root in roots.items():
        for other_role, other in roots.items():
            if role != other_role and overlaps(root, other):
                raise UpdateError(
                    f"Roots overlap: {role}={root}, {other_role}={other}"
                )
    return roots


def validate_relative(relative: str) -> str:
    if not relative or "\\" in relative or "/" in relative:
        if "\\" in relative:
            normalized = relative.replace("\\", "/")
        else:
            normalized = relative
    else:
        normalized = relative
    parts = Path(normalized).parts
    if normalized.startswith("/") or normalized.startswith("\\"):
        raise UpdateError(f"Absolute manifest destination is forbidden: {relative}")
    if ".." in parts or any(part in ("", ".", "") for part in parts):
        raise UpdateError(f"Traversal-like manifest destination is forbidden: {relative}")
    normalized_key = normalized.replace("\\", "/")
    if normalized_key not in ALLOWED_DESTINATIONS:
        raise UpdateError(f"Manifest destination is not in the allowlist: {relative}")
    return normalized_key


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
        raise UpdateError("Could not safely read HOST_INI/CLIENT_INI from tools/package.py")
    return constants["HOST_INI"], constants["CLIENT_INI"]


def active_ini(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = raw.decode("cp1252")
    values: dict[str, str] = {}
    for line_number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", ";")):
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip().lower()
        if key in values:
            raise UpdateError(
                f"Duplicate active INI key {key!r} in {path}:{line_number}"
            )
        values[key] = value.strip()
    return values


def set_ini_key(lines: list[str], key: str, value: str) -> None:
    lower_key = key.lower()
    found = False
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", ";")):
            continue
        if "=" not in line:
            continue
        existing_key = line.split("=", 1)[0].strip().lower()
        if existing_key == lower_key:
            prefix = line[: line.find("=")] if "=" in line else ""
            indent = prefix[: len(prefix) - len(prefix.lstrip())]
            lines[index] = f"{indent}{key} = {value}"
            found = True
    if not found:
        lines.extend([f"{key} = {value}"])


def render_ini(template: str, existing: Path, role: str, session_id: str) -> bytes:
    if len(session_id) > 63:
        raise UpdateError("session_id must be 1-63 ASCII characters")
    try:
        session_id.encode("ascii")
    except UnicodeEncodeError as exc:
        raise UpdateError("session_id must contain ASCII only") from exc
    if not session_id:
        raise UpdateError("session_id must not be empty")
    if any(
        char not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"
        for char in session_id
    ):
        raise UpdateError(
            "session_id may contain only ASCII letters, digits, underscores and hyphens"
        )

    lines = template.splitlines()
    set_ini_key(lines, "role", role)
    set_ini_key(lines, "transport", "steam")
    set_ini_key(lines, "connection", "invite")
    set_ini_key(lines, "session_id", session_id)
    set_ini_key(lines, "language", "auto")

    if existing.is_file():
        existing_values = active_ini(existing)
        template_values = active_ini(existing.with_name("__package_template_probe.ini")) if False else {}
        base_keys = {
            "role",
            "transport",
            "connection",
            "session_id",
            "language",
        }
        unknown = {
            key: value
            for key, value in existing_values.items()
            if key not in base_keys
            and key not in {"puppet_template", "partner_key", "friendly_fire"}
        }
        if unknown:
            lines.extend(["", "# Preserved unknown personal/configuration keys"])
            for key, value in sorted(unknown.items()):
                set_ini_key(lines, key, value)

    return ("\n".join(lines).rstrip() + "\n").encode("utf-8")


def read_running_processes() -> list[dict]:
    if os.name != "nt":
        raise UpdateError(
            "Fail closed: process metadata requires Windows and PowerShell/CIM"
        )
    script = (
        "$ErrorActionPreference='Stop'; "
        "Get-CimInstance Win32_Process "
        "| Where-Object { $_.Name -ieq 'Gothic3.exe' -or $_.Name -ieq 'Gothic3Final.exe' } "
        "| Select-Object ProcessId,Name,ExecutablePath,CreationDate "
        "| ConvertTo-Json -Compress"
    )
    try:
        result = subprocess.run(
            [
                "powershell.exe",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                script,
            ],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise UpdateError(f"Could not query process metadata: {exc}") from exc
    if result.returncode != 0:
        raise UpdateError(
            f"Process metadata query failed: {result.stderr.strip() or result.stdout.strip()}"
        )
    output = result.stdout.strip()
    if not output:
        return []
    try:
        value = json.loads(output)
    except json.JSONDecodeError as exc:
        raise UpdateError("Process metadata query returned malformed JSON") from exc
    if isinstance(value, dict):
        value = [value]
    if not isinstance(value, list):
        raise UpdateError("Process metadata query returned unexpected JSON")
    return value


def check_running_copies(
    roots: dict[str, Path],
    roles: Iterable[str],
    process_reader=read_running_processes,
) -> None:
    processes = process_reader()
    if not isinstance(processes, list):
        raise UpdateError("Process metadata reader did not return a list")

    exact_destinations: set[Path] = set()
    for role in roles:
        root = roots[role]
        for name in ("Gothic3.exe", "Gothic3Final.exe"):
            exact_destinations.add(canonical(root / name))

    for process in processes:
        if not isinstance(process, dict):
            raise UpdateError("Malformed process metadata entry")
        name = str(process.get("Name") or "").lower()
        if name not in ("gothic3.exe", "gothic3final.exe"):
            continue
        executable = process.get("ExecutablePath")
        if not executable:
            raise UpdateError(
                f"Fail closed: cannot determine executable path for running {name}"
            )
        executable_path = canonical(Path(str(executable)))
        if executable_path in exact_destinations:
            raise UpdateError(
                f"Refusing apply: exact target executable is running: {executable_path}"
            )
    return


def safe_target(root: Path, relative: str) -> Path:
    normalized = validate_relative(relative).replace("/", "\\")
    path = reject_links(root / normalized)
    if path.exists():
        info = path.lstat()
        reparse = getattr(info, "st_file_attributes", 0) & getattr(
            stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400
        )
        if stat.S_ISLNK(info.st_mode) or reparse:
            raise UpdateError(f"Refusing link/reparse target: {path}")
        if path.is_file():
            try:
                link_count = path.stat(follow_symlinks=False).st_nlink
            except TypeError:
                link_count = path.lstat().st_nlink
            if link_count != 1:
                raise UpdateError(
                    f"Refusing hard-linked writable target: {path} nlink={link_count}"
                )
    return path


def staged_fake_support(fake_dll: Path) -> dict[str, Path]:
    bundle = fake_dll.parent
    candidates = {
        "G3Together/test-realism/fake_steam_diag.h":
            bundle / "fake_steam_diag.h",
        "G3Together/test-realism/fake_steam_abi.h":
            bundle / "fake_steam_abi.h",
        "G3Together/test-realism/fake_steam_api.def":
            bundle / "fake_steam_api.def",
        "G3Together/test-realism/build.ps1":
            bundle / "build.ps1",
        "G3Together/test-realism/receipt.json":
            bundle / "receipt.json",
        "G3Together/test-realism/run_tests.ps1":
            bundle / "run_tests.ps1",
        "G3Together/test-realism/README.md":
            bundle / "README.md",
        "G3Together/test-realism/source/fake_steam_api.c":
            bundle / "source" / "fake_steam_api.c",
        "G3Together/test-realism/source/fake_steam_test.c":
            bundle / "source" / "fake_steam_test.c",
        "G3Together/test-realism/fake_steam_process_test.c":
            bundle / "fake_steam_process_test.c",
        "G3Together/test-realism/fake_steam_contract_test.c":
            bundle / "fake_steam_contract_test.c",
        "G3Together/test-realism/fake_steam_test.exe":
            bundle / "fake_steam_test.exe",
        "G3Together/test-realism/fake_steam_process_test.exe":
            bundle / "fake_steam_process_test.exe",
        "G3Together/test-realism/fake_steam_contract_test.exe":
            bundle / "fake_steam_contract_test.exe",
        "G3Together/test-realism/test-summary.txt":
            bundle / "test-summary.txt",
        "G3Together/test-realism/profiles/ideal.ini":
            bundle / "profiles" / "ideal.ini",
        "G3Together/test-realism/profiles/steam-typical.ini":
            bundle / "profiles" / "steam-typical.ini",
        "G3Together/test-realism/profiles/steam-bad.ini":
            bundle / "profiles" / "steam-bad.ini",
    }
    existing_markers = sum(source.is_file() for source in candidates.values())
    if existing_markers == 0:
        return {}
    missing = [
        relative
        for relative, source in candidates.items()
        if not source.is_file()
    ]
    if missing:
        raise UpdateError(
            "Incomplete staged fake-transport bundle; missing: "
            + ", ".join(missing)
        )
    return candidates


def build_plan(
    roots: dict[str, Path],
    fake_dll: Path | None,
    session_id: str,
    approved_fake_sha: str | None,
    roles: Iterable[str],
) -> tuple[str, list[dict]]:
    host_template, client_template = extract_package_ini()
    if fake_dll is None:
        package_fake = (
            Path(__file__).resolve().parents[1]
            / "G3Together"
            / "test-realism"
            / "fake_steam_api.dll"
        )
        if package_fake.is_file():
            fake_dll = package_fake
    if fake_dll is not None:
        fake_dll = canonical(fake_dll)
        if not fake_dll.is_file():
            raise UpdateError(f"Missing staged fake DLL: {fake_dll}")
        fake_hash = sha256(fake_dll)
        approved = approved_fake_sha or KNOWN_FAKE_SHA
        if fake_hash != approved:
            raise UpdateError(
                f"Unknown staged fake DLL SHA-256 {fake_hash}; "
                "pass --approved-dll-sha256 for an explicitly reviewed hash"
            )
    else:
        fake_hash = None

    entries: list[dict] = []
    transaction_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:12]

    for role in roles:
        root = roots[role]
        template = host_template if role == "host" else client_template
        ini_path = safe_target(root, "g3together.ini")
        proposed_ini = render_ini(template, ini_path, role, session_id)
        before_exists = ini_path.is_file()
        before_hash = sha256(ini_path) if before_exists else None
        after_hash = hashlib.sha256(proposed_ini).hexdigest()
        action = "NOOP" if before_exists and before_hash == after_hash else (
            "REPLACE" if before_exists else "ADD"
        )
        entries.append(
            {
                "root_role": role,
                "root": str(root),
                "relative_path": "g3together.ini",
                "kind": "config",
                "action": action,
                "existed_before": before_exists,
                "before_sha256": before_hash,
                "after_sha256": after_hash,
                "source_sha256": None,
                "backup_relative": None,
                "proposed_bytes": proposed_ini,
            }
        )

        if fake_dll is not None:
            fake_path = safe_target(
                root,
                "G3Together/test-realism/fake_steam_api.dll",
            )
            before_exists = fake_path.is_file()
            before_hash = sha256(fake_path) if before_exists else None
            source_bytes = fake_dll.read_bytes()
            after_hash = hashlib.sha256(source_bytes).hexdigest()
            action = "NOOP" if before_exists and before_hash == after_hash else (
                "REPLACE" if before_exists else "ADD"
            )
            entries.append(
                {
                    "root_role": role,
                    "root": str(root),
                    "relative_path": "G3Together/test-realism/fake_steam_api.dll",
                    "kind": "fake-dll",
                    "action": action,
                    "existed_before": before_exists,
                    "before_sha256": before_hash,
                    "after_sha256": after_hash,
                    "source_sha256": fake_hash,
                    "backup_relative": None,
                    "proposed_bytes": source_bytes,
                }
            )
            for support_relative, support_source in staged_fake_support(fake_dll).items():
                support_path = safe_target(root, support_relative)
                support_before_exists = support_path.is_file()
                support_before_hash = (
                    sha256(support_path) if support_before_exists else None
                )
                support_bytes = support_source.read_bytes()
                support_after_hash = hashlib.sha256(support_bytes).hexdigest()
                support_action = (
                    "NOOP"
                    if support_before_exists
                    and support_before_hash == support_after_hash
                    else "REPLACE"
                    if support_before_exists
                    else "ADD"
                )
                entries.append(
                    {
                        "root_role": role,
                        "root": str(root),
                        "relative_path": support_relative,
                        "kind": "fake-support",
                        "action": support_action,
                        "existed_before": support_before_exists,
                        "before_sha256": support_before_hash,
                        "after_sha256": support_after_hash,
                        "source_sha256": sha256(support_source),
                        "backup_relative": None,
                        "proposed_bytes": support_bytes,
                    }
                )
    return transaction_id, entries


def write_manifest(path: Path, manifest: dict) -> None:
    reject_links(path.parent)
    path.parent.mkdir(parents=True, exist_ok=True)
    reject_links(path.parent)
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")


def print_plan(entries: list[dict]) -> None:
    session_id = "default"
    for entry in entries:
        if entry["relative_path"] == "g3together.ini":
            for line in entry["proposed_bytes"].decode("utf-8").splitlines():
                if line.strip().lower().startswith("session_id"):
                    session_id = line.split("=", 1)[1].strip()
                    break
            break
    print(
        "LAUNCHER | launch-copies-invite.ps1"
        " (alias; implementation=launch-copies-realism.ps1)"
        f" | PROFILE=ideal | CONNECTION=invite | SESSION={session_id}"
    )
    print("ROOT_ROLE | RELATIVE_PATH | ACTION | KIND | BEFORE_SHA256 | AFTER_SHA256")
    for entry in entries:
        print(
            " | ".join(
                (
                    entry["root_role"],
                    entry["relative_path"],
                    entry["action"],
                    entry["kind"],
                    entry["before_sha256"] or "-",
                    entry["after_sha256"],
                )
            )
        )
    print(
        "IMMUTABLE | genuine G3Together/steam_api.dll, "
        "Data/video/native/shim/PUP/template/profile | NOOP"
    )


def prepare_manifest(
    transaction_id: str,
    roots: dict[str, Path],
    entries: list[dict],
) -> dict:
    manifest_entries = []
    for index, entry in enumerate(entries):
        if entry["action"] == "NOOP":
            backup_relative = None
        else:
            backup_relative = f".g3t-update-{transaction_id}/backups/{index:04d}.bak"
        manifest_entries.append(
            {
                key: value
                for key, value in entry.items()
                if key != "proposed_bytes"
            }
            | {"backup_relative": backup_relative}
        )
    session_id = "default"
    for entry in entries:
        if entry["relative_path"] != "g3together.ini":
            continue
        for line in entry["proposed_bytes"].decode("utf-8").splitlines():
            if line.strip().lower().startswith("session_id"):
                session_id = line.split("=", 1)[1].strip()
                break
    return {
        "schema": 1,
        "transaction_id": transaction_id,
        "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "state": "planned",
        "roots": {role: str(path) for role, path in roots.items()},
        "default_launcher": "launch-copies-invite.ps1",
        "legacy_launcher": "launch-copies-realism.ps1",
        "launcher_profile": "ideal",
        "connection": "invite",
        "session_id": session_id,
        "files": manifest_entries,
    }


def verify_plan_drift(entries: list[dict]) -> None:
    for entry in entries:
        if entry["action"] == "NOOP":
            continue
        path = safe_target(Path(entry["root"]), entry["relative_path"])
        existed = path.is_file()
        current_hash = sha256(path) if existed else None
        if existed != entry["existed_before"]:
            raise UpdateError(
                f"Destination presence drifted before apply: {path}"
            )
        if current_hash != entry["before_sha256"]:
            raise UpdateError(
                f"Destination SHA-256 drifted before apply: {path}"
            )


def fsync_file(path: Path) -> None:
    with path.open("r+b") as handle:
        os.fsync(handle.fileno())


def apply_entries(
    manifest_path: Path,
    manifest: dict,
    entries: list[dict],
) -> None:
    transaction_id = manifest["transaction_id"]
    applied: list[dict] = []
    try:
        manifest["state"] = "applying"
        write_manifest(manifest_path, manifest)

        for entry in entries:
            if entry["action"] == "NOOP":
                continue
            root = Path(entry["root"])
            target = safe_target(root, entry["relative_path"])
            transaction_dir = root / f".g3t-update-{transaction_id}"
            transaction_dir.mkdir(exist_ok=True)
            reject_links(transaction_dir)

            backup_relative = next(
                item["backup_relative"]
                for item in manifest["files"]
                if item["root_role"] == entry["root_role"]
                and item["relative_path"] == entry["relative_path"]
            )
            backup_path = transaction_dir / "backups" / Path(backup_relative).name
            backup_path.parent.mkdir(parents=True, exist_ok=True)

            if entry["existed_before"]:
                if not target.is_file():
                    raise UpdateError(f"Destination disappeared before backup: {target}")
                backup_bytes = target.read_bytes()
                if hashlib.sha256(backup_bytes).hexdigest() != entry["before_sha256"]:
                    raise UpdateError(f"Destination changed before independent backup: {target}")
                with backup_path.open("xb") as handle:
                    handle.write(backup_bytes)
                    handle.flush()
                    os.fsync(handle.fileno())
                fsync_file(backup_path)
                if sha256(backup_path) != entry["before_sha256"]:
                    raise UpdateError(f"Independent backup verification failed: {backup_path}")
                try:
                    link_count = backup_path.stat(follow_symlinks=False).st_nlink
                except TypeError:
                    link_count = backup_path.lstat().st_nlink
                if link_count != 1:
                    raise UpdateError(f"Backup is unexpectedly hard-linked: {backup_path}")

            staged_path = transaction_dir / ("replacement-" + uuid.uuid4().hex)
            target.parent.mkdir(parents=True, exist_ok=True)
            reject_links(target.parent)
            with staged_path.open("xb") as handle:
                handle.write(entry["proposed_bytes"])
                handle.flush()
                os.fsync(handle.fileno())
            fsync_file(staged_path)
            if sha256(staged_path) != entry["after_sha256"]:
                raise UpdateError(f"Staged replacement verification failed: {staged_path}")

            os.replace(staged_path, target)
            fsync_file(target)
            if sha256(target) != entry["after_sha256"]:
                raise UpdateError(f"Post-replacement verification failed: {target}")
            applied.append(entry)

        manifest["state"] = "committed"
        write_manifest(manifest_path, manifest)
    except (OSError, UpdateError) as exc:
        rollback_errors: list[str] = []
        for entry in reversed(applied):
            try:
                rollback_entry(entry, manifest, transaction_id)
            except (OSError, UpdateError) as rollback_exc:
                rollback_errors.append(str(rollback_exc))
        manifest["state"] = "failed"
        if rollback_errors:
            manifest["rollback_errors"] = rollback_errors
            manifest["state"] = "failed-with-rollback-errors"
        else:
            manifest["rolled_back"] = True
        write_manifest(manifest_path, manifest)
        detail = f"{exc}"
        if rollback_errors:
            detail += "; rollback errors: " + "; ".join(rollback_errors)
        raise UpdateError(detail)


def rollback_entry(entry: dict, manifest: dict, transaction_id: str) -> None:
    root = Path(entry["root"])
    target = safe_target(root, entry["relative_path"])
    transaction_dir = root / f".g3t-update-{transaction_id}"
    current_hash = sha256(target) if target.is_file() else None
    if current_hash != entry["after_sha256"]:
        raise UpdateError(
            f"Refusing rollback of externally modified destination: {target}"
        )
    if entry["existed_before"]:
        manifest_item = next(
            item
            for item in manifest["files"]
            if item["root_role"] == entry["root_role"]
            and item["relative_path"] == entry["relative_path"]
        )
        backup_path = transaction_dir / "backups" / Path(manifest_item["backup_relative"]).name
        if not backup_path.is_file():
            raise UpdateError(f"Missing independent backup: {backup_path}")
        if sha256(backup_path) != entry["before_sha256"]:
            raise UpdateError(f"Backup hash mismatch: {backup_path}")
        restore_path = transaction_dir / ("restore-" + uuid.uuid4().hex)
        with restore_path.open("xb") as handle:
            handle.write(backup_path.read_bytes())
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(restore_path, target)
        if sha256(target) != entry["before_sha256"]:
            raise UpdateError(f"Rollback verification failed: {target}")
    else:
        target.unlink()
        if target.exists():
            raise UpdateError(f"Rollback could not remove tool-created file: {target}")


def load_manifest(path: Path) -> dict:
    reject_links(path)
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise UpdateError(f"Invalid transaction manifest JSON: {path}") from exc
    if manifest.get("schema") != 1:
        raise UpdateError("Unsupported or malformed transaction manifest")
    if not isinstance(manifest.get("files"), list):
        raise UpdateError("Malformed transaction manifest files list")
    return manifest


def validate_manifest_against_roots(
    manifest: dict,
    roots: dict[str, Path],
) -> None:
    manifest_roots = manifest.get("roots")
    if not isinstance(manifest_roots, dict):
        raise UpdateError("Manifest has no exact roots map")
    for role in ("host", "client", "steam"):
        if role in roots:
            manifest_value = manifest_roots.get(role)
            if manifest_value is None:
                raise UpdateError(f"Manifest is missing exact {role} root")
            if canonical(Path(manifest_value)) != roots[role]:
                raise UpdateError(
                    f"Manifest root mismatch for {role}: "
                    f"{manifest_value} != {roots[role]}"
                )

    for item in manifest["files"]:
        if not isinstance(item, dict):
            raise UpdateError("Malformed manifest file entry")
        relative = validate_relative(item.get("relative_path", ""))
        role = item.get("root_role")
        root_value = item.get("root")
        if role not in roots:
            raise UpdateError(f"Manifest uses unknown root role: {role}")
        if canonical(Path(root_value)) != roots[role]:
            raise UpdateError(f"Manifest file root mismatch: {root_value}")
        if item.get("action") not in ("ADD", "REPLACE", "NOOP"):
            raise UpdateError("Manifest contains unknown action")
        if not isinstance(item.get("existed_before"), bool):
            raise UpdateError("Manifest lacked existed-before flag")
        if item.get("before_sha256") is not None:
            if len(str(item["before_sha256"])) != 64:
                raise UpdateError("Malformed before SHA-256")
        if len(str(item.get("after_sha256") or "")) != 64:
            raise UpdateError("Malformed after SHA-256")
        backup_relative = item.get("backup_relative")
        if backup_relative is not None:
            backup_path = Path(backup_relative)
            if backup_path.is_absolute():
                raise UpdateError("Absolute backup path is forbidden")
            if ".." in backup_path.parts:
                raise UpdateError("Traversal in backup path is forbidden")


def rollback_manifest(
    manifest_path: Path,
    manifest: dict,
    roots: dict[str, Path],
    apply: bool,
    process_reader=read_running_processes,
) -> None:
    validate_manifest_against_roots(manifest, roots)
    entries = manifest["files"]
    print("ROLLBACK PLAN")
    print("ROOT_ROLE | RELATIVE_PATH | CURRENT_STATUS")
    for item in entries:
        if item["action"] == "NOOP":
            continue
        target = safe_target(Path(item["root"]), item["relative_path"])
        current = sha256(target) if target.is_file() else None
        if item["existed_before"]:
            status = "restore backup" if current == item["after_sha256"] else "REFUSE external edit"
        else:
            status = "delete tool-created file" if current == item["after_sha256"] else "REFUSE external edit"
        print(f"{item['root_role']} | {item['relative_path']} | {status}")
    if not apply:
        print("DRY-RUN: no rollback changes made")
        return

    touched_roles = sorted(
        {item["root_role"] for item in entries if item["action"] != "NOOP"}
    )
    check_running_copies(roots, touched_roles, process_reader)
    transaction_id = manifest["transaction_id"]
    rollback_errors: list[str] = []
    for item in reversed(entries):
        if item["action"] == "NOOP":
            continue
        try:
            rollback_entry(
                {
                    "root_role": item["root_role"],
                    "root": item["root"],
                    "relative_path": item["relative_path"],
                    "action": item["action"],
                    "existed_before": item["existed_before"],
                    "before_sha256": item["before_sha256"],
                    "after_sha256": item["after_sha256"],
                },
                manifest,
                transaction_id,
            )
        except (OSError, UpdateError) as exc:
            rollback_errors.append(str(exc))
    if rollback_errors:
        manifest["state"] = "rolled-back-with-errors"
        manifest["rollback_errors"] = rollback_errors
    else:
        manifest["state"] = "rolled-back"
    write_manifest(manifest_path, manifest)
    if rollback_errors:
        raise UpdateError("; ".join(rollback_errors))


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Dry-run/default, explicit-apply updater for isolated Gothic 3 copies."
    )
    parser.add_argument("--host-root", "--host", type=Path, default=DEFAULT_HOST)
    parser.add_argument("--client-root", "--client", type=Path, default=DEFAULT_CLIENT)
    parser.add_argument("--steam-root", "--steam", type=Path, default=DEFAULT_STEAM)
    parser.add_argument(
        "--allow-fixture-root",
        action="append",
        type=Path,
        default=[],
    )
    parser.add_argument("--fake-dll", type=Path)
    parser.add_argument("--approved-dll-sha256")
    parser.add_argument("--session-id", default="default")
    parser.add_argument("--manifest-out", type=Path)
    parser.add_argument("--rollback", type=Path)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--client-only", action="store_true")
    args = parser.parse_args(argv)

    try:
        roots = validate_roots(
            args.host_root,
            args.client_root,
            args.steam_root,
            args.allow_fixture_root,
        )

        if args.rollback:
            manifest = load_manifest(args.rollback)
            # A rollback manifest itself selects its exact roots. The CLI roots
            # are still required and must match.
            rollback_manifest(
                args.rollback,
                manifest,
                roots,
                args.apply,
            )
            return 0

        roles = ("client",) if args.client_only else ("host", "client")
        transaction_id, entries = build_plan(
            roots,
            args.fake_dll,
            args.session_id,
            args.approved_dll_sha256,
            roles,
        )
        print_plan(entries)

        if not args.apply:
            print("DRY-RUN: no target files or directories changed")
            if args.manifest_out:
                print(f"Manifest output not written in dry run: {args.manifest_out}")
            return 0

        if not args.manifest_out:
            raise UpdateError("--apply requires --manifest-out")

        # Process metadata is checked immediately before the first mutation.
        check_running_copies(roots, roles)
        verify_plan_drift(entries)

        manifest = prepare_manifest(transaction_id, roots, entries)
        manifest_path = canonical(args.manifest_out)
        write_manifest(manifest_path, manifest)
        apply_entries(manifest_path, manifest, entries)
        print(f"APPLIED | manifest={manifest_path}")
        print("Apply complete; rerun the read-only full verifier.")
        return 0
    except (OSError, UpdateError, KeyError, json.JSONDecodeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
