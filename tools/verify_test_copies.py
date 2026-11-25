#!/usr/bin/env python3
"""Read-only verification for the isolated Gothic 3 test copies.

This tool never launches a game or writes to a target.  It can optionally write
a JSON report to an explicitly supplied path.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Iterable


WORKTREE = Path(__file__).resolve().parents[1]
PACKAGE_AUDIT = WORKTREE / "expected-installation.json"

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))
from _g3copy import (
    DEFAULT_CLIENT, DEFAULT_HOST, DEFAULT_STEAM, FOLDER_MODULES, KNOWN_FAKE_SHA,
    NATIVE_SLOTS, SLOT_GENUINE, SLOT_QA, TEST_REALISM_BUNDLE_FILES,
    canonical as _canonical, is_hard_linked, is_within, overlaps,
    package_ini_templates, parse_g3t_ini, parse_pe, reject_links as _reject_links,
    sha256, validate_copy_roots,
)


class VerificationError(RuntimeError):
    pass


def reject_links(path: Path) -> Path:
    return _reject_links(path, VerificationError)


def canonical(path: Path) -> Path:
    return _canonical(path, VerificationError)


def validate_roots(
    host: Path, client: Path, steam: Path, fixture_roots: Iterable[Path] = ()
) -> dict[str, Path]:
    return validate_copy_roots(host, client, steam, fixture_roots, VerificationError)


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
    del audit
    rows: list[list[str]] = []
    failures: list[str] = []

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
    allowed = {"script_g3together.dll", "script_game.dll", "script_paru.dll"}
    if sum(name.lower() == "script_g3together.dll" for name in matches) != 1:
        failures.append(
            f"Expected exactly one Script_G3Together.dll in {role}; found {matches}"
        )
    extras = [name for name in matches if name.lower() not in allowed]
    if extras:
        failures.append(
            f"Unrecognized Script*.dll entries in {role}: {extras}"
        )
    return failures


def current_protocol() -> int:
    try:
        text = (WORKTREE / "src" / "saving_policy.h").read_text(encoding="utf-8")
        match = re.search(r"\bProtocol\s*=\s*(\d+)", text)
        return int(match.group(1)) if match else 81
    except OSError:
        return 81


def verify_profile_and_template(
    roots: dict[str, Path], expected_protocol: int | None = None,
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
    pairs = [
        (reject_links(template_dir / ("G3T_client_template." + ext)),
         reject_links(template_dir / ("G3T_client_template." + ext + "dat")))
        for ext in ("g3savcp", "g3savcpx")
    ]
    pair = next(((header, world) for header, world in pairs if header.is_file() and world.is_file()), None)
    if not marker.is_file() or pair is None:
        failures.append("Client template is missing; warm rejoin is not verified")
    else:
        text = marker.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        fields = dict(line.split("=", 1) for line in lines[1:] if "=" in line)
        try:
            header_size = int(fields["header"])
            world_size = int(fields["world"])
            protocol = int(fields["protocol"])
            expected = current_protocol() if expected_protocol is None else expected_protocol
            valid = (lines[0].strip().lower() == "g3t client template 2"
                     and protocol == expected
                     and fields.get("origin") in ("new-game", "existing-world")
                     and 1024 <= header_size <= 16_777_216
                     and 4096 <= world_size <= 268_435_456
                     and pair[0].stat().st_size == header_size
                     and pair[1].stat().st_size == world_size)
        except (ValueError, KeyError, IndexError):
            valid = False
        if not valid:
            failures.append("Client template marker, protocol, or recorded sizes are invalid")

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
    for role in ("host", "client"):
        path = reject_links(roots[role] / "g3together.ini")
        if not path.is_file():
            failures.append(f"Missing g3together.ini {role}:{path}")
            continue
        try:
            ini, _ = parse_g3t_ini(path.read_text(encoding="utf-8-sig", errors="replace"))
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
    parser.add_argument("--protocol", type=int, default=current_protocol())
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)

    try:
        if args.expected == PACKAGE_AUDIT and not args.expected.exists():
            raise VerificationError("Pass --expected <expected-installation.json> explicitly")
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

        profile_rows, failures, warnings = verify_profile_and_template(roots, args.protocol)
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
