#!/usr/bin/env python3
"""Opt-in manual toggle for Gothic 3 startup logos and intro video.

This never runs the game. Use --disable or --restore explicitly for one copy.
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))
from _g3copy import is_hard_linked, reject_links


BACKUP_SUFFIX = ".g3t-manual-video-backup"


def check_files(paths: tuple[Path, ...]) -> None:
    for path in paths:
        reject_redirect(path)
        if path.exists():
            if not path.is_file():
                raise RuntimeError(f"Expected a file: {path}")
            if is_hard_linked(path):
                raise RuntimeError(f"Hard-linked file is not allowed (would mutate shared install): {path}")


def atomic_write(path: Path, data: bytes) -> None:
    fd, name = tempfile.mkstemp(prefix=".g3t-logo-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as target:
            target.write(data)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def reject_redirect(path: Path) -> None:
    reject_links(Path(path), RuntimeError)


def disable(root: Path) -> None:
    root = Path(root)
    ini_dir = root / "Ini"
    video_dir = root / "Data" / "Video"
    for directory in (root, ini_dir, root / "Data", video_dir):
        reject_redirect(directory)
        if not directory.is_dir():
            raise RuntimeError(f"Missing required directory: {directory}")

    logos = ini_dir / "logo.ini"
    logos_backup = logos.with_name(logos.name + BACKUP_SUFFIX)
    intro = video_dir / "G3_Intro.bik"
    intro_backup = intro.with_name(intro.name + BACKUP_SUFFIX)
    check_files((logos, logos_backup, intro, intro_backup))

    if not logos.is_file():
        raise RuntimeError(f"Missing startup logo list: {logos}")
    original_logos = logos.read_bytes()
    if original_logos and logos_backup.exists() and logos_backup.read_bytes() != original_logos:
        raise RuntimeError(f"Logo list differs from existing backup: {logos}")
    if intro.exists() and intro_backup.exists():
        raise RuntimeError(f"Both intro and its backup exist: {intro}")

    if original_logos:
        if not logos_backup.exists():
            logos_backup.write_bytes(original_logos)
        atomic_write(logos, b"")
    if intro.exists():
        intro.rename(intro_backup)
    print(f"PASS: startup videos disabled for {root}")
    print(f"PASS: restore with --restore --root \"{root}\"")


def restore(root: Path) -> None:
    root = Path(root)
    ini_dir = root / "Ini"
    video_dir = root / "Data" / "Video"
    logos = ini_dir / "logo.ini"
    logos_backup = logos.with_name(logos.name + BACKUP_SUFFIX)
    intro = video_dir / "G3_Intro.bik"
    intro_backup = intro.with_name(intro.name + BACKUP_SUFFIX)
    for path in (root, ini_dir, video_dir, logos, logos_backup, intro, intro_backup):
        reject_redirect(path)
    check_files((logos, logos_backup, intro, intro_backup))
    if intro_backup.exists() and intro.exists():
        raise RuntimeError(f"Cannot restore over an existing intro: {intro}")

    restored = False
    if logos_backup.is_file():
        atomic_write(logos, logos_backup.read_bytes())
        logos_backup.unlink()
        restored = True
    if intro_backup.is_file():
        intro_backup.rename(intro)
        restored = True
    if not restored:
        raise RuntimeError(f"No startup-video backup found for {root}")
    print(f"PASS: startup videos restored for {root}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Explicitly disable or restore startup videos in one Gothic 3 copy."
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--disable", action="store_true", help="disable startup logos and intro")
    action.add_argument("--restore", action="store_true", help="restore files saved by --disable")
    parser.add_argument("--root", required=True, type=Path, help="Gothic 3 copy root")
    args = parser.parse_args(argv)
    try:
        supplied_root = args.root.expanduser()
        reject_redirect(supplied_root)
        root = supplied_root.resolve()
        if args.disable:
            disable(root)
        else:
            restore(root)
    except (OSError, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
