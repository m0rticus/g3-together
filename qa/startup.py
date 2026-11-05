"""Prepare disposable game copies for video-free QA starts."""
import os
import stat
import tempfile
from pathlib import Path


BACKUP_SUFFIX = '.g3t-qa-video-backup'


def _reject_redirect(path):
    if not path.exists() and not path.is_symlink():
        return
    info = path.lstat()
    reparse = getattr(info, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)
    if stat.S_ISLNK(info.st_mode) or reparse:
        raise RuntimeError(f'QA video path is a link or reparse point: {path}')


def disable_startup_videos(root):
    """Disable logos and the new-game intro, preserving originals beside the test copy."""
    root = Path(root)
    ini_dir = root/'Ini'
    video_dir = root/'Data'/'Video'
    for directory in (root, ini_dir, root/'Data', video_dir):
        _reject_redirect(directory)
        if not directory.is_dir():
            raise FileNotFoundError(f'QA game directory missing: {directory}')

    logos = ini_dir/'logo.ini'
    logos_backup = logos.with_name(logos.name + BACKUP_SUFFIX)
    intro = video_dir/'G3_Intro.bik'
    intro_backup = intro.with_name(intro.name + BACKUP_SUFFIX)
    for path in (logos, logos_backup, intro, intro_backup):
        _reject_redirect(path)
        if path.exists() and not path.is_file():
            raise RuntimeError(f'QA video path is not a file: {path}')
    if not logos.is_file():
        raise FileNotFoundError(f'QA logo list missing: {logos}')
    if intro.exists() and intro_backup.exists():
        raise RuntimeError(f'Both QA intro and its backup exist: {intro}')

    original_logos = logos.read_bytes()
    if original_logos and logos_backup.exists() and logos_backup.read_bytes() != original_logos:
        raise RuntimeError(f'QA logo list differs from its existing backup: {logos}')

    if original_logos:
        if not logos_backup.exists():
            with logos_backup.open('xb') as backup:
                backup.write(original_logos)
        with tempfile.NamedTemporaryFile(dir=ini_dir, prefix='.g3t-qa-logo-', delete=False) as temporary:
            temporary_path = Path(temporary.name)
        try:
            os.replace(temporary_path, logos)
        finally:
            temporary_path.unlink(missing_ok=True)
    if intro.exists():
        intro.rename(intro_backup)

    return {'logo_list_empty': not logos.read_bytes(), 'intro_file_absent': not intro.exists()}
