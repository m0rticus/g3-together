"""Immutable saved-world fixtures; tests only modify copies in isolated profiles."""
import hashlib
import json
import os
import shutil
import tempfile
from pathlib import Path

from isolation import regular_path


SCHEMA = 1
EXTENSIONS = ('.g3savcp', '.g3savcpdat', '.g3t')
ALTERNATIVE_EXTENSIONS = ('.g3savcpx', '.g3savcpxdat', '.g3t')
NATIVE_FILES = ('Game.dll', 'Engine.dll', 'Script.dll', 'scripts/Script_Game.dll')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def native_identity(root):
    return {name: digest(Path(root)/name) for name in NATIVE_FILES}


def read_fixture(path, slot, identity):
    path = regular_path(path)
    meta = json.loads(regular_path(path/'meta.json').read_text(encoding='utf-8'))
    if meta.get('schema') != SCHEMA or meta.get('slot') != slot:
        raise RuntimeError(f'Unsupported fixture; rebuild with --suite sandboxsave: {path}')
    if meta.get('native_files') != identity:
        raise RuntimeError(f'Fixture native game build differs; rebuild with --suite sandboxsave: {path}')
    if meta.get('save_reload_verified') is not True:
        raise RuntimeError(f'Fixture has not passed a real save/load check: {path}')
    if not isinstance(meta.get('baseline'), dict) or set(meta['baseline']) != {'host', 'client'}:
        raise RuntimeError(f'Fixture baseline missing: {path}')
    for tag in ('host', 'client'):
        files = meta.get('files', {}).get(tag)
        if not isinstance(files, dict) or not files:
            raise RuntimeError(f'Fixture files missing for {tag}: {path}')
        expected = ([{slot+ext for ext in extensions} for extensions in (EXTENSIONS, ALTERNATIVE_EXTENSIONS)] if tag == 'host'
                    else [{f'g3t-hero-G3T_host_{slot}.txt'}])
        matches = sum(names <= files.keys() for names in expected)
        if matches != 1:
            raise RuntimeError(f'Fixture save or hero record missing for {tag}: {path}')
        for name, sha in files.items():
            if not name or Path(name).name != name or '/' in name or '\\' in name or slot not in name:
                raise RuntimeError(f'Unsafe fixture file name: {name}')
            file = regular_path(path/tag/name)
            if not file.is_file() or file.stat().st_size == 0 or digest(file) != sha:
                raise RuntimeError(f'Fixture file missing or damaged: {file}')
    return meta


def publish_fixture(path, slot, folders, metadata):
    """Validate a staged copy, then replace the fixture without losing a previous good save on failure."""
    path = regular_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=path.name+'-staging-', dir=path.parent) as temp:
        staged = Path(temp)/'new'
        staged.mkdir()
        files = {}
        for tag, folder in folders.items():
            (staged/tag).mkdir()
            files[tag] = {}
            for item in sorted(Path(folder).glob('*'+slot+'*')):
                regular_path(item)
                if item.is_file() and item.suffix != '.tmp':
                    target = staged/tag/item.name
                    shutil.copy2(item, target)
                    files[tag][item.name] = digest(target)
        meta = {**metadata, 'schema': SCHEMA, 'slot': slot, 'files': files}
        (staged/'meta.json').write_text(json.dumps(meta, indent=2), encoding='utf-8')
        read_fixture(staged, slot, metadata['native_files'])
        previous = Path(temp)/'previous'
        if path.exists():
            path.rename(previous)
        try:
            staged.rename(path)
        except OSError:
            if previous.exists():
                previous.rename(path)
            raise
    return meta


def restore_fixture(path, slot, identity, folders):
    """Check all hashes before touching either profile; overwrite test-local copies, never the baseline."""
    meta = read_fixture(path, slot, identity)
    for tag, folder in folders.items():
        folder = regular_path(folder)
        for name in meta['files'][tag]:
            target = regular_path(folder/name)
            with tempfile.NamedTemporaryFile(dir=folder, prefix='.g3t-fixture-', delete=False) as out:
                temporary = Path(out.name)
                with (Path(path)/tag/name).open('rb') as source:
                    shutil.copyfileobj(source, out)
            try:
                os.replace(temporary, target)
            finally:
                temporary.unlink(missing_ok=True)
    return meta
