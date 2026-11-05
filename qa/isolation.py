"""Fail closed before launching the disposable Gothic 3 copies."""
import hashlib
import json
import stat
import struct
from pathlib import Path


FOLDER_MODULES = ('Game.dll', 'Engine.dll', 'SharedBase.dll', 'Dialogs.dll', 'paru_s2.dll')


def regular_path(path):
    path = Path(path)
    for part in (path, *path.parents):
        if part.exists() or part.is_symlink():
            info = part.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
                raise RuntimeError(f'QA path is a link or reparse point: {part}')
    return path


def imports(path):
    """Read PE32 imports without loading executable code."""
    data = Path(path).read_bytes()
    if data[:2] != b'MZ':
        raise ValueError(f'Not a PE file: {path}')
    pe = struct.unpack_from('<I', data, 60)[0]
    if data[pe:pe+4] != b'PE\0\0' or struct.unpack_from('<H', data, pe+24)[0] != 0x10b:
        raise ValueError(f'Not a PE32 file: {path}')
    count = struct.unpack_from('<H', data, pe+6)[0]
    sections = pe+24+struct.unpack_from('<H', data, pe+20)[0]

    def offset(rva):
        for i in range(count):
            size, address, raw_size, raw = struct.unpack_from('<IIII', data, sections+40*i+8)
            if address <= rva < address+max(size, raw_size):
                return raw+rva-address
        raise ValueError(f'Unmapped PE RVA in {path}: {rva}')

    def text(rva):
        start = offset(rva)
        return data[start:data.index(b'\0', start)].decode('ascii')

    cursor = offset(struct.unpack_from('<I', data, pe+24+104)[0])
    found = {}
    while any(struct.unpack_from('<IIIII', data, cursor)):
        lookup, _, _, name, table = struct.unpack_from('<IIIII', data, cursor)
        functions = []
        at = offset(lookup or table)
        while value := struct.unpack_from('<I', data, at)[0]:
            if not value & 0x80000000:
                functions.append(text(value+2))
            at += 4
        found[text(name).lower()] = functions
        cursor += 20
    return found


def verify_copies(workspace, copies):
    config = json.loads((Path(workspace)/'project.json').read_text(encoding='utf-8-sig'))
    protected = Path(config['protected_save_dir'])
    if not protected.is_absolute():
        raise RuntimeError('protected_save_dir must be an absolute path')
    protected = protected.resolve()
    roots = [regular_path(root).resolve() for root in copies.values()]
    for i, root in enumerate(roots):
        if root.is_relative_to(protected) or protected.is_relative_to(root):
            raise RuntimeError(f'Test copy overlaps protected saves: {root}')
        if any(root.is_relative_to(other) or other.is_relative_to(root) for other in roots[:i]):
            raise RuntimeError('Test copies must be separate directories')
    hashes = []
    for root in roots:
        for name in ('Gothic3.exe', 'g3qa32.dll', 'scripts/Script_G3Together.dll', *FOLDER_MODULES):
            if not regular_path(root/name).is_file():
                raise RuntimeError(f'Missing QA game file: {root/name}')
        if (root/'scripts'/'Script_G3Coop.dll').exists():
            raise RuntimeError(f'Another co-op mod is active: {root}')
        for name in FOLDER_MODULES:
            found = imports(root/name)
            if 'SHGetFolderPathA' not in found.get('g3qa32.dll', ()):
                raise RuntimeError(f'Unisolated folder import: {root/name}')
            if any(f.startswith(('SHGetFolderPath', 'SHGetKnownFolderPath'))
                   for dll, functions in found.items() if dll != 'g3qa32.dll' for f in functions):
                raise RuntimeError(f'Native folder import remains: {root/name}')
        profile = regular_path(root/'isolated-profile'/'Documents'/'gothic3')
        if not profile.is_dir():
            raise RuntimeError(f'Missing isolated profile: {profile}')
        for path in (root/'isolated-profile').rglob('*'):
            regular_path(path)
            if path.is_file() and path.stat().st_nlink != 1:
                raise RuntimeError(f'Isolated profile file has hard links: {path}')
        for name in ('g3together.ini', 'qa-command.txt', 'qa-enable.txt', 'qa-engine.log', 'g3together.log',
                     'g3trace.log', 'Ini/ge3.ini', 'isolated-profile/Documents/gothic3/UserOptions.ini'):
            path = regular_path(root/name)
            if path.exists() and path.stat().st_nlink != 1:
                raise RuntimeError(f'Writable QA file has hard links: {path}')
        hashes.append(hashlib.sha256((root/'scripts'/'Script_G3Together.dll').read_bytes()).hexdigest())
    if len(set(hashes)) != 1:
        raise RuntimeError('Host and client mod DLLs differ')
    return hashes[0], protected
