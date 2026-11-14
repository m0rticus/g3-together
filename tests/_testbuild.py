# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared parallel content-addressed native test builder and cached PE/source readers."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import threading
import unittest
from typing import Iterable, Sequence

ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
for _sys_dir in (str(ROOT), str(TOOLS_DIR)):
    if _sys_dir not in sys.path:
        sys.path.insert(0, _sys_dir)

CACHE_ROOT = ROOT / "build" / "test-cache"
OBJ_DIR = CACHE_ROOT / "obj"
BIN_DIR = CACHE_ROOT / "bin"
GEN_DIR = CACHE_ROOT / "gen"
ENV_DIR = CACHE_ROOT / "env"

CACHE_SCHEMA_VERSION = "g3t-testcache-v1"
MAX_BUILD_WORKERS = max(2, min(os.cpu_count() or 8, 16))

_INCLUDE_RE = re.compile(rb'^\s*#\s*include\s*(?:"([^"]+)"|<([^>]+)>)', re.MULTILINE)
_WARN_LINE_RE = re.compile(r"\bwarning(?:\s+[A-Z]+\d+)?:", re.IGNORECASE)

_CACHE_LOCK = threading.RLock()
_KEY_LOCKS: dict[str, threading.Lock] = {}
_FILE_META_CACHE: dict[str, tuple[int, int, str, tuple[tuple[bool, str], ...]]] = {}
_BYTES_CACHE: dict[str, tuple[int, int, bytes]] = {}
_TEXT_CACHE: dict[str, tuple[int, int, str]] = {}
_SHA256_CACHE: dict[str, tuple[int, int, str]] = {}
_PE_CACHE: dict[str, tuple[int, int, "CachedPe"]] = {}
_COMPILER_SIG_CACHE: dict[str, str] = {}
_TOOLCHAIN_CACHE: dict[tuple, "ResolvedToolchain"] = {}
_INCLUDE_RESOLVE_CACHE: dict[tuple[str, bool, str, tuple[str, ...]], Path | None] = {}
_TRANSITIVE_DIGEST_CACHE: dict[tuple[str, tuple[str, ...], tuple[str, ...], int, int], str] = {}
_NORM_ROOT_PREFIX = os.path.normcase(os.path.abspath(ROOT)) + os.sep
_WIN32_BASE_ENV_CACHE: dict[str, str] | None = None

_REGISTERED_TARGETS: list["NativeTarget"] = []
_BATCH_WARMED_UP_TO = 0


def _norm_key(path: str | Path) -> str:
    return os.path.normcase(os.path.abspath(path))


def _key_lock(key: str) -> threading.Lock:
    with _CACHE_LOCK:
        lock = _KEY_LOCKS.get(key)
        if lock is None:
            lock = threading.Lock()
            _KEY_LOCKS[key] = lock
        return lock


def _ensure_cache_dirs() -> None:
    OBJ_DIR.mkdir(parents=True, exist_ok=True)
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    GEN_DIR.mkdir(parents=True, exist_ok=True)
    ENV_DIR.mkdir(parents=True, exist_ok=True)


# ---------------------------------------------------------------------------
# Cached file / source / PE readers
# ---------------------------------------------------------------------------

def cached_bytes(path: str | Path) -> bytes:
    p = Path(path)
    key = _norm_key(p)
    st = p.stat()
    mtime_ns, size = st.st_mtime_ns, st.st_size
    with _CACHE_LOCK:
        hit = _BYTES_CACHE.get(key)
        if hit is not None and hit[0] == mtime_ns and hit[1] == size:
            return hit[2]
    data = p.read_bytes()
    with _CACHE_LOCK:
        _BYTES_CACHE[key] = (mtime_ns, size, data)
    return data


def cached_text(path: str | Path, encoding: str = "utf-8") -> str:
    p = Path(path)
    key = f"{_norm_key(p)}::{encoding}"
    st = p.stat()
    mtime_ns, size = st.st_mtime_ns, st.st_size
    with _CACHE_LOCK:
        hit = _TEXT_CACHE.get(key)
        if hit is not None and hit[0] == mtime_ns and hit[1] == size:
            return hit[2]
    text = p.read_text(encoding=encoding)
    with _CACHE_LOCK:
        _TEXT_CACHE[key] = (mtime_ns, size, text)
    return text


def src_text(name: str) -> str:
    return cached_text(ROOT / "src" / name)


def cached_sha256(path: str | Path) -> str:
    p = Path(path)
    key = _norm_key(p)
    st = p.stat()
    mtime_ns, size = st.st_mtime_ns, st.st_size
    with _CACHE_LOCK:
        hit = _SHA256_CACHE.get(key)
        if hit is not None and hit[0] == mtime_ns and hit[1] == size:
            return hit[2]
        bytes_hit = _BYTES_CACHE.get(key)
    if bytes_hit is not None and bytes_hit[0] == mtime_ns and bytes_hit[1] == size:
        digest = hashlib.sha256(bytes_hit[2]).hexdigest()
    else:
        h = hashlib.sha256()
        with p.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                h.update(chunk)
        digest = h.hexdigest()
    with _CACHE_LOCK:
        _SHA256_CACHE[key] = (mtime_ns, size, digest)
    return digest


class CachedPe:
    """Fast read-only PE32 inspector with lazy cached exports, imports, and relocations."""

    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.data = cached_bytes(self.path)
        if len(self.data) < 0x40 or self.u16(0) != 0x5A4D:
            raise ValueError(f"not an MZ image: {self.path}")
        pe = self.u32(0x3C)
        if pe + 24 > len(self.data) or self.u32(pe) != 0x4550 or self.u16(pe + 4) != 0x14C:
            raise ValueError(f"not a 32-bit x86 PE: {self.path}")
        optional = pe + 24
        opt_size = self.u16(pe + 20)
        if opt_size < 96 or optional + opt_size > len(self.data) or self.u16(optional) != 0x10B:
            raise ValueError(f"not PE32: {self.path}")
        self.base = self.u32(optional + 28)
        self.image_base = self.base
        count_dirs = self.u32(optional + 92)
        self.export_rva, self.export_size = (
            (self.u32(optional + 96), self.u32(optional + 100)) if count_dirs > 0 else (0, 0)
        )
        self.import_rva, self.import_size = (
            (self.u32(optional + 104), self.u32(optional + 108)) if count_dirs > 1 else (0, 0)
        )
        self.reloc_rva, self.reloc_size = (
            (self.u32(optional + 136), self.u32(optional + 140)) if count_dirs > 5 else (0, 0)
        )
        section_table = optional + opt_size
        sec_count = self.u16(pe + 6)
        self.sections: list[tuple[int, int, int, int]] = []
        for i in range(sec_count):
            sec = section_table + i * 40
            virtual_size, va, raw_size, raw_at = struct.unpack_from("<IIII", self.data, sec + 8)
            self.sections.append((va, virtual_size, raw_size, raw_at))
        self._exports_cache: dict[str, int] | None = None
        self._imports_iat_cache: dict[int, tuple[str, str]] | None = None
        self._imports_va_bytes_cache: dict[int, bytes] | None = None
        self._relocs_cache: frozenset[int] | None = None

    def u16(self, offset: int) -> int:
        return struct.unpack_from("<H", self.data, offset)[0]

    def u32(self, offset: int) -> int:
        return struct.unpack_from("<I", self.data, offset)[0]

    def offset(self, rva: int, size: int = 1) -> int:
        for va, _vsize, raw_size, raw_at in self.sections:
            if va <= rva and rva - va + size <= raw_size and raw_at + rva - va + size <= len(self.data):
                return raw_at + rva - va
        raise ValueError(f"unmapped RVA {rva:#x}")

    def read(self, rva: int, size: int) -> bytes:
        for va, vsize, raw_size, raw_at in self.sections:
            span = max(vsize, raw_size)
            if va <= rva < va + span:
                rel = rva - va
                if rel + size <= raw_size and raw_at + rel + size <= len(self.data):
                    start = raw_at + rel
                    return self.data[start:start + size]
                if rel + size <= span:
                    avail = max(0, min(raw_size - rel, size, len(self.data) - (raw_at + rel)))
                    head = self.data[raw_at + rel:raw_at + rel + avail] if avail > 0 else b""
                    return head + b"\0" * (size - len(head))
                break
        raise ValueError(f"unmapped RVA {rva:#x}")

    def get_data(self, rva: int, length: int) -> bytes:
        return self.read(rva, length)

    def call_target(self, rva: int) -> int | None:
        instruction = self.read(rva, 5)
        if instruction[0] != 0xE8:
            return None
        return rva + 5 + struct.unpack("<i", instruction[1:])[0]

    def exports(self) -> dict[str, int]:
        if self._exports_cache is not None:
            return self._exports_cache
        if not self.export_rva:
            self._exports_cache = {}
            return self._exports_cache
        directory = self.offset(self.export_rva, 40)
        function_count, name_count = self.u32(directory + 20), self.u32(directory + 24)
        functions, names, ordinals = (
            self.u32(directory + 28),
            self.u32(directory + 32),
            self.u32(directory + 36),
        )
        found: dict[str, int] = {}
        for i in range(name_count):
            name_rva = self.u32(self.offset(names + 4 * i, 4))
            start = self.offset(name_rva)
            end = self.data.find(b"\0", start)
            if end < 0:
                raise ValueError("unterminated export name")
            name = self.data[start:end].decode("ascii")
            ordinal = self.u16(self.offset(ordinals + 2 * i, 2))
            if ordinal >= function_count or name in found:
                raise ValueError("invalid or duplicate export")
            found[name] = self.u32(self.offset(functions + 4 * ordinal, 4))
        self._exports_cache = found
        return found

    def _ensure_imports(self) -> None:
        if self._imports_iat_cache is not None:
            return
        by_iat: dict[int, tuple[str, str]] = {}
        by_va_bytes: dict[int, bytes] = {}
        if self.import_rva:
            for i in range(4096):
                descriptor = self.offset(self.import_rva + i * 20, 20)
                original, _, _, name, first = struct.unpack_from("<IIIII", self.data, descriptor)
                if not original and not name and not first:
                    break
                start = self.offset(name)
                dll = self.data[start:self.data.index(b"\0", start)].decode("ascii")
                for n in range(65536):
                    thunk = self.u32(self.offset((original or first) + n * 4, 4))
                    if not thunk:
                        break
                    slot_rva = first + n * 4
                    if thunk & 0x80000000:
                        by_iat[slot_rva] = (dll, f"<ordinal:{thunk & 0xFFFF}>")
                    else:
                        s_start = self.offset(thunk, 3) + 2
                        s_end = self.data.index(b"\0", s_start)
                        raw_sym = self.data[s_start:s_end]
                        by_iat[slot_rva] = (dll, raw_sym.decode("ascii"))
                        by_va_bytes[self.base + slot_rva] = raw_sym
        self._imports_iat_cache = by_iat
        self._imports_va_bytes_cache = by_va_bytes

    def imported_at(self, iat_rva: int) -> tuple[str, str]:
        self._ensure_imports()
        assert self._imports_iat_cache is not None
        hit = self._imports_iat_cache.get(iat_rva)
        if hit is None:
            raise ValueError("missing IAT slot")
        if hit[1].startswith("<ordinal:"):
            raise ValueError("ordinal import")
        return hit

    def imports_by_va_bytes(self) -> dict[int, bytes]:
        self._ensure_imports()
        assert self._imports_va_bytes_cache is not None
        return self._imports_va_bytes_cache

    def has_highlow_relocation(self, target: int) -> bool:
        if self._relocs_cache is None:
            targets: set[int] = set()
            pos = 0
            while pos + 8 <= self.reloc_size:
                block = self.offset(self.reloc_rva + pos, 8)
                page, size = struct.unpack_from("<II", self.data, block)
                if size < 8 or size % 2 or pos + size > self.reloc_size:
                    raise ValueError("invalid relocation block")
                entries_off = self.offset(self.reloc_rva + pos + 8, size - 8)
                count = (size - 8) // 2
                for (entry,) in struct.iter_unpack("<H", self.data[entries_off:entries_off + count * 2]):
                    if (entry >> 12) == 3:
                        targets.add(page + (entry & 0xFFF))
                pos += size
            self._relocs_cache = frozenset(targets)
        return target in self._relocs_cache


def cached_pe(path: str | Path) -> CachedPe:
    p = Path(path)
    key = _norm_key(p)
    st = p.stat()
    mtime_ns, size = st.st_mtime_ns, st.st_size
    with _CACHE_LOCK:
        hit = _PE_CACHE.get(key)
        if hit is not None and hit[0] == mtime_ns and hit[1] == size:
            return hit[2]
    pe = CachedPe(p)
    with _CACHE_LOCK:
        _PE_CACHE[key] = (mtime_ns, size, pe)
    return pe


# ---------------------------------------------------------------------------
# Transitive #include dependency hashing
# ---------------------------------------------------------------------------

def _file_meta(path: Path) -> tuple[str, tuple[tuple[bool, str], ...]]:
    key = _norm_key(path)
    st = path.stat()
    mtime_ns, size = st.st_mtime_ns, st.st_size
    with _CACHE_LOCK:
        hit = _FILE_META_CACHE.get(key)
        if hit is not None and hit[0] == mtime_ns and hit[1] == size:
            return hit[2], hit[3]
    raw = path.read_bytes()
    sha = hashlib.sha256(raw).hexdigest()
    includes: list[tuple[bool, str]] = []
    for match in _INCLUDE_RE.finditer(raw):
        quoted, angled = match.group(1), match.group(2)
        if quoted is not None:
            includes.append((False, quoted.decode("utf-8", errors="replace")))
        elif angled is not None:
            includes.append((True, angled.decode("utf-8", errors="replace")))
    inc_tuple = tuple(includes)
    with _CACHE_LOCK:
        _FILE_META_CACHE[key] = (mtime_ns, size, sha, inc_tuple)
        _SHA256_CACHE[key] = (mtime_ns, size, sha)
    return sha, inc_tuple


def _resolve_include(
    including_dir: Path,
    is_angled: bool,
    inc_path: str,
    include_dirs: Sequence[Path],
    inc_dirs_key: tuple[str, ...] = (),
) -> Path | None:
    cache_key = (_norm_key(including_dir), is_angled, inc_path, inc_dirs_key)
    with _CACHE_LOCK:
        if cache_key in _INCLUDE_RESOLVE_CACHE:
            return _INCLUDE_RESOLVE_CACHE[cache_key]
    found: Path | None = None
    if not is_angled:
        cand = including_dir / inc_path
        try:
            if cand.is_file():
                found = Path(os.path.abspath(cand))
        except OSError:
            pass
    if found is None:
        for inc_dir in include_dirs:
            cand = inc_dir / inc_path
            try:
                if cand.is_file():
                    found = Path(os.path.abspath(cand))
                    break
            except OSError:
                continue
    with _CACHE_LOCK:
        _INCLUDE_RESOLVE_CACHE[cache_key] = found
    return found


def transitive_source_digest(
    root_source: Path,
    include_dirs: Sequence[Path],
    extra_deps: Sequence[Path] = (),
) -> str:
    abs_root = Path(os.path.abspath(root_source))
    norm_root = _norm_key(abs_root)
    resolved_includes = tuple(Path(os.path.abspath(p)) for p in include_dirs)
    inc_dirs_key = tuple(_norm_key(p) for p in resolved_includes)
    abs_extra = tuple(Path(os.path.abspath(p)) for p in extra_deps)
    extra_key = tuple(_norm_key(p) for p in abs_extra)
    in_repo = norm_root.startswith(_NORM_ROOT_PREFIX) and not abs_extra
    root_st = abs_root.stat()
    td_key = (norm_root, inc_dirs_key, extra_key, root_st.st_mtime_ns, root_st.st_size)
    if in_repo:
        with _CACHE_LOCK:
            cached = _TRANSITIVE_DIGEST_CACHE.get(td_key)
            if cached is not None:
                return cached
    visited: dict[str, str] = {}
    queue: list[Path] = [abs_root, *abs_extra]
    while queue:
        current = queue.pop()
        norm = _norm_key(current)
        if norm in visited:
            continue
        sha, includes = _file_meta(current)
        visited[norm] = sha
        parent = current.parent
        for is_angled, inc_name in includes:
            target = _resolve_include(parent, is_angled, inc_name, resolved_includes, inc_dirs_key)
            if target is not None and _norm_key(target) not in visited:
                queue.append(target)
    hasher = hashlib.sha256()
    for norm in sorted(visited):
        hasher.update(norm.encode("utf-8"))
        hasher.update(b"\0")
        hasher.update(visited[norm].encode("ascii"))
        hasher.update(b"\n")
    digest = hasher.hexdigest()
    if in_repo:
        with _CACHE_LOCK:
            _TRANSITIVE_DIGEST_CACHE[td_key] = digest
    return digest


# ---------------------------------------------------------------------------
# Toolchain resolution
# ---------------------------------------------------------------------------

def win32_cl_environment(temp_dir: Path | None = None) -> dict[str, str]:
    global _WIN32_BASE_ENV_CACHE
    _ensure_cache_dirs()
    scratch = temp_dir or ENV_DIR
    scratch.mkdir(parents=True, exist_ok=True)
    with _CACHE_LOCK:
        if _WIN32_BASE_ENV_CACHE is None:
            from _g3copy import win32_tool_environment
            _WIN32_BASE_ENV_CACHE = win32_tool_environment(ENV_DIR)
        env = _WIN32_BASE_ENV_CACHE.copy()
    env["TEMP"] = env["TMP"] = str(scratch)
    return env


def _compiler_signature(compiler_path: str | Path) -> str:
    p = Path(compiler_path).resolve()
    key = _norm_key(p)
    with _CACHE_LOCK:
        sig = _COMPILER_SIG_CACHE.get(key)
        if sig is not None:
            return sig
    st = p.stat()
    sig = f"{key}:{st.st_mtime_ns}:{st.st_size}"
    with _CACHE_LOCK:
        _COMPILER_SIG_CACHE[key] = sig
    return sig


@dataclass(frozen=True)
class ResolvedToolchain:
    family: str  # "gnu" or "msvc"
    arch_tag: str  # "host" or "win32_x86"
    cxx: str | None
    cc: str | None
    env: dict[str, str] = field(repr=False, hash=False, compare=False)
    signature: str = ""


def resolve_toolchain(
    kind: str = "cxx",  # "cxx", "c", "win32_cl"
    *,
    allow_msvc: bool = True,
    msvc_vcvars: bool = True,
    respect_env: bool = True,
    needs_c: bool = False,
    skip_message: str = "C/C++ compiler required",
) -> ResolvedToolchain:
    tc_cache_key = (
        kind,
        allow_msvc,
        msvc_vcvars,
        respect_env,
        needs_c,
        os.environ.get("CC", "") if respect_env else "",
        os.environ.get("CXX", "") if respect_env else "",
    )
    with _CACHE_LOCK:
        cached_tc = _TOOLCHAIN_CACHE.get(tc_cache_key)
        if cached_tc is not None:
            return cached_tc
    resolved = _resolve_toolchain_uncached(
        kind,
        allow_msvc=allow_msvc,
        msvc_vcvars=msvc_vcvars,
        respect_env=respect_env,
        needs_c=needs_c,
        skip_message=skip_message,
    )
    with _CACHE_LOCK:
        _TOOLCHAIN_CACHE[tc_cache_key] = resolved
    return resolved


def _resolve_toolchain_uncached(
    kind: str = "cxx",
    *,
    allow_msvc: bool = True,
    msvc_vcvars: bool = True,
    respect_env: bool = True,
    needs_c: bool = False,
    skip_message: str = "C/C++ compiler required",
) -> ResolvedToolchain:
    if kind == "win32_cl":
        if os.name != "nt":
            raise unittest.SkipTest(skip_message)
        env = win32_cl_environment()
        cl = shutil.which("cl.exe", path=env.get("PATH") or env.get("Path"))
        if not cl:
            raise RuntimeError("Win32 compiler not found after vcvars32")
        sig = f"msvc:win32_x86:{_compiler_signature(cl)}"
        return ResolvedToolchain("msvc", "win32_x86", cl, cl, env, sig)

    if kind == "c":
        candidates: list[str] = []
        if respect_env and os.environ.get("CC"):
            candidates.append(os.environ["CC"])
        candidates.extend(["clang", "gcc"])
        for cand in candidates:
            found = shutil.which(cand)
            if found and Path(found).stem.lower() != "cl":
                sig = f"gnu:host:{_compiler_signature(found)}"
                return ResolvedToolchain("gnu", "host", None, found, os.environ.copy(), sig)
        if allow_msvc:
            cl = shutil.which("cl")
            env = os.environ.copy()
            arch = "host"
            if not cl and msvc_vcvars and os.name == "nt":
                try:
                    env = win32_cl_environment()
                    cl = shutil.which("cl.exe", path=env.get("PATH") or env.get("Path"))
                    arch = "win32_x86"
                except RuntimeError:
                    cl = None
            if cl:
                sig = f"msvc:{arch}:{_compiler_signature(cl)}"
                return ResolvedToolchain("msvc", arch, cl, cl, env, sig)
        raise unittest.SkipTest(skip_message)

    # kind == "cxx"
    cxx_candidates: list[str] = []
    if respect_env and os.environ.get("CXX"):
        cxx_candidates.append(os.environ["CXX"])
    cxx_candidates.extend(["clang++", "g++"])
    for cand in cxx_candidates:
        found = shutil.which(cand)
        if found:
            stem = Path(found).stem.lower()
            if stem == "cl":
                if not allow_msvc:
                    continue
                sig = f"msvc:host:{_compiler_signature(found)}"
                return ResolvedToolchain("msvc", "host", found, found, os.environ.copy(), sig)
            cc: str | None = None
            if needs_c:
                if respect_env and os.environ.get("CC"):
                    cc = shutil.which(os.environ["CC"]) or os.environ["CC"]
                else:
                    c_stem = "clang" if "clang" in stem else "gcc"
                    sibling = Path(found).with_name(c_stem + (".exe" if os.name == "nt" else ""))
                    if sibling.is_file():
                        cc = str(sibling)
                    else:
                        cc = shutil.which(c_stem)
                if not cc:
                    raise unittest.SkipTest("Companion C compiler required")
            sig_parts = [f"gnu:host:{_compiler_signature(found)}"]
            if cc:
                sig_parts.append(_compiler_signature(cc))
            return ResolvedToolchain("gnu", "host", found, cc, os.environ.copy(), "|".join(sig_parts))

    if allow_msvc:
        cl = shutil.which("cl")
        env = os.environ.copy()
        arch = "host"
        if not cl and msvc_vcvars and os.name == "nt":
            try:
                env = win32_cl_environment()
                cl = shutil.which("cl.exe", path=env.get("PATH") or env.get("Path"))
                arch = "win32_x86"
            except RuntimeError:
                cl = None
        if cl:
            sig = f"msvc:{arch}:{_compiler_signature(cl)}"
            return ResolvedToolchain("msvc", arch, cl, cl, env, sig)

    raise unittest.SkipTest(skip_message)


# ---------------------------------------------------------------------------
# Declarative SourceUnit & NativeTarget + Parallel Build Engine
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class SourceUnit:
    path: Path | None = None
    inline_name: str | None = None
    inline_code: str | None = None
    lang: str | None = None  # "c++" or "c"; inferred from suffix when None
    std: str | None = None  # default "c++17" for c++, "c11" for c on gnu
    opt: str | None = "-O2"  # e.g. "-O2"
    warnings: str = "strict"  # "strict", "warn", "w3", "none"
    pedantic: bool = False
    stdcall_gz: bool = False
    defines: tuple[str, ...] = ()
    includes: tuple[Path, ...] = ()
    extra_gnu_flags: tuple[str, ...] = ()
    extra_msvc_flags: tuple[str, ...] = ()
    extra_deps: tuple[Path, ...] = ()

    def materialize_path(self) -> Path:
        if self.path is not None:
            return (ROOT / self.path if not self.path.is_absolute() else self.path).resolve()
        assert self.inline_name and self.inline_code is not None
        _ensure_cache_dirs()
        digest = hashlib.sha256(self.inline_code.encode("utf-8")).hexdigest()[:20]
        stem = Path(self.inline_name).stem
        suffix = Path(self.inline_name).suffix or ".cpp"
        gen_path = GEN_DIR / f"{stem}-{digest}{suffix}"
        if not gen_path.is_file():
            lock = _key_lock(f"gen:{gen_path.name}")
            with lock:
                if not gen_path.is_file():
                    tmp = GEN_DIR / f"{gen_path.name}.tmp.{os.getpid()}.{threading.get_ident()}"
                    tmp.write_text(self.inline_code, encoding="utf-8")
                    os.replace(tmp, gen_path)
        return gen_path

    def effective_lang(self) -> str:
        if self.lang is not None:
            return self.lang
        name = self.inline_name if self.path is None else self.path.name
        assert name is not None
        return "c" if name.lower().endswith(".c") else "c++"


def unit(
    source: str | Path,
    *,
    lang: str | None = None,
    std: str | None = None,
    opt: str | None = "-O2",
    warnings: str = "strict",
    pedantic: bool = False,
    stdcall_gz: bool = False,
    defines: Sequence[str] = (),
    includes: Sequence[str | Path] = (),
    extra_gnu_flags: Sequence[str] = (),
    extra_cxx_gnu: Sequence[str] = (),
    extra_msvc_flags: Sequence[str] = (),
    extra_deps: Sequence[str | Path] = (),
) -> SourceUnit:
    p = Path(source)
    if not p.is_absolute():
        p = ROOT / p
    inc = tuple((ROOT / i if not Path(i).is_absolute() else Path(i)).resolve() for i in includes)
    deps = tuple((ROOT / d if not Path(d).is_absolute() else Path(d)).resolve() for d in extra_deps)
    gnu_flags = (*extra_gnu_flags, *extra_cxx_gnu)
    return SourceUnit(
        path=p.resolve(),
        lang=lang,
        std=std,
        opt=opt,
        warnings=warnings,
        pedantic=pedantic,
        stdcall_gz=stdcall_gz,
        defines=tuple(defines),
        includes=inc,
        extra_gnu_flags=gnu_flags,
        extra_msvc_flags=tuple(extra_msvc_flags),
        extra_deps=deps,
    )


def inline_unit(
    name: str,
    code: str,
    *,
    lang: str = "c++",
    std: str | None = "c++17",
    opt: str | None = "-O2",
    warnings: str = "strict",
    pedantic: bool = False,
    defines: Sequence[str] = (),
    includes: Sequence[str | Path] = (),
    extra_deps: Sequence[str | Path] = (),
) -> SourceUnit:
    inc = tuple((ROOT / i if not Path(i).is_absolute() else Path(i)).resolve() for i in includes)
    deps = tuple((ROOT / d if not Path(d).is_absolute() else Path(d)).resolve() for d in extra_deps)
    return SourceUnit(
        inline_name=name,
        inline_code=code,
        lang=lang,
        std=std,
        opt=opt,
        warnings=warnings,
        pedantic=pedantic,
        defines=tuple(defines),
        includes=inc,
        extra_deps=deps,
    )


@dataclass
class _ObjectPlan:
    key: str
    obj_path: Path
    command: list[str]
    env: dict[str, str]
    cwd: Path


def _plan_object(tc: ResolvedToolchain, su: SourceUnit, pthread_mode: str | None) -> _ObjectPlan:
    src_path = su.materialize_path()
    lang = su.effective_lang()
    default_inc = list(su.includes)
    if su.inline_code is not None:
        for fallback_dir in (ROOT / "tests", ROOT / "src"):
            if fallback_dir.resolve() not in default_inc:
                default_inc.append(fallback_dir.resolve())

    src_digest = transitive_source_digest(src_path, default_inc, su.extra_deps)

    use_pthread = (
        pthread_mode == "always" or (pthread_mode == "posix" and os.name != "nt")
    )

    if tc.family == "gnu":
        compiler = tc.cc if lang == "c" else tc.cxx
        assert compiler is not None
        std = su.std or ("c11" if lang == "c" else "c++17")
        cmd = [compiler, "-c", f"-std={std}"]
        if su.warnings == "strict":
            cmd.extend(["-Wall", "-Wextra", "-Werror"])
        elif su.warnings == "warn":
            cmd.extend(["-Wall", "-Wextra"])
        if su.pedantic:
            cmd.append("-pedantic")
        if su.opt:
            cmd.append(su.opt)
        if use_pthread:
            cmd.append("-pthread")
        cmd.extend(su.extra_gnu_flags)
        for d in su.defines:
            cmd.append(f"-D{d}")
        for inc in default_inc:
            cmd.extend(["-I", str(inc)])
        ext = ".o"
    else:
        compiler = tc.cc if lang == "c" else tc.cxx
        assert compiler is not None
        cmd = [compiler, "/nologo", "/TC" if lang == "c" else "/TP", "/c"]
        if lang == "c++":
            std = su.std or "c++17"
            cmd.extend([f"/std:{std}", "/EHsc"])
        if su.stdcall_gz:
            cmd.append("/Gz")
        if su.opt:
            cmd.append("/O2" if su.opt == "-O2" else su.opt)
        if su.warnings == "strict":
            cmd.extend(["/W4", "/WX"])
        elif su.warnings == "warn":
            cmd.append("/W4")
        elif su.warnings == "w3":
            cmd.append("/W3")
        cmd.extend(su.extra_msvc_flags)
        for d in su.defines:
            cmd.append(f"/D{d}")
        for inc in default_inc:
            cmd.append(f"/I{inc}")
        ext = ".obj"

    key_payload = json.dumps(
        {
            "schema": CACHE_SCHEMA_VERSION,
            "tc": tc.signature,
            "lang": lang,
            "flags": cmd[1:],
            "src_digest": src_digest,
        },
        sort_keys=True,
    )
    obj_key = hashlib.sha256(key_payload.encode("utf-8")).hexdigest()
    obj_path = OBJ_DIR / f"{src_path.stem}-{obj_key[:20]}{ext}"
    return _ObjectPlan(obj_key, obj_path, cmd + [str(src_path)], tc.env, OBJ_DIR)


def _obj_warn_path(obj_path: Path) -> Path:
    return obj_path.with_name(obj_path.name + ".warn")


def _read_obj_warn(obj_path: Path) -> str:
    wp = _obj_warn_path(obj_path)
    if wp.is_file():
        try:
            return wp.read_text(encoding="utf-8")
        except OSError:
            return ""
    return ""


def _compile_object(plan: _ObjectPlan) -> str:
    if plan.obj_path.is_file():
        return _read_obj_warn(plan.obj_path)
    lock = _key_lock(f"obj:{plan.key}")
    with lock:
        if plan.obj_path.is_file():
            return _read_obj_warn(plan.obj_path)
        _ensure_cache_dirs()
        suffix = plan.obj_path.suffix
        tag = f"{os.getpid()}.{threading.get_ident()}"
        tmp = OBJ_DIR / f"{plan.obj_path.stem}.tmp.{tag}{suffix}"
        env = plan.env.copy()
        if "VSCMD_ARG_TGT_ARCH" in env:
            env["TEMP"] = env["TMP"] = str(OBJ_DIR)
        if plan.command[0].lower().endswith(("cl.exe", "cl")):
            cmd = [*plan.command, f"/Fo{tmp}"]
        else:
            cmd = [*plan.command, "-o", str(tmp)]
        try:
            result = subprocess.run(
                cmd, cwd=plan.cwd, env=env, capture_output=True, text=True, timeout=180
            )
            output = result.stdout + result.stderr
            if result.returncode != 0:
                raise RuntimeError(f"Compile failed ({' '.join(cmd)}):\n{output}")
            warn_lines = [ln for ln in output.splitlines() if _WARN_LINE_RE.search(ln)]
            if warn_lines:
                tmp_warn = OBJ_DIR / f"{plan.obj_path.stem}.warn.tmp.{tag}"
                tmp_warn.write_text("\n".join(warn_lines), encoding="utf-8")
                os.replace(tmp_warn, _obj_warn_path(plan.obj_path))
            os.replace(tmp, plan.obj_path)
            return "\n".join(warn_lines)
        finally:
            tmp.unlink(missing_ok=True)


@dataclass
class BuildResult:
    executable: Path
    env: dict[str, str]
    warnings: list[str]

    @property
    def run_env(self) -> dict[str, str]:
        return self.env


class NativeTarget:
    def __init__(
        self,
        name: str,
        sources: Sequence[SourceUnit | str | Path],
        *,
        toolchain: str = "auto",  # "auto", "cxx", "c", "win32_cl"
        opt: str | None = "-O2",
        warnings: str = "strict",
        pedantic: bool = False,
        stdcall_gz: bool = False,
        defines: Sequence[str] = (),
        includes: Sequence[str | Path] = (),
        libs: Sequence[str] = (),
        libs_windows_only: bool = True,
        pthread: str | None = None,  # None, "always", "posix"
        extra_link_gnu: Sequence[str] = (),
        allow_msvc: bool = True,
        msvc_vcvars: bool = True,
        respect_env: bool = True,
        skip_message: str = "C/C++17 compiler required",
        register: bool = True,
    ):
        self.name = name
        self.toolchain_kind = toolchain
        self.libs = tuple(libs)
        self.libs_windows_only = libs_windows_only
        self.pthread = pthread
        self.extra_link_gnu = tuple(extra_link_gnu)
        self.allow_msvc = allow_msvc
        self.msvc_vcvars = msvc_vcvars
        self.respect_env = respect_env
        self.skip_message = skip_message

        normalized_units: list[SourceUnit] = []
        for s in sources:
            if isinstance(s, SourceUnit):
                normalized_units.append(s)
            else:
                normalized_units.append(
                    unit(
                        s,
                        opt=opt,
                        warnings=warnings,
                        pedantic=pedantic,
                        stdcall_gz=stdcall_gz,
                        defines=defines,
                        includes=includes,
                    )
                )
        self.units = tuple(normalized_units)
        self._registered = bool(register)
        self._result: BuildResult | None = None
        self._deferred_exc: BaseException | None = None
        if self._registered:
            _REGISTERED_TARGETS.append(self)

    def _prepare(self) -> tuple[ResolvedToolchain, list[_ObjectPlan], Path, Path, str]:
        has_c = any(u.effective_lang() == "c" for u in self.units)
        has_cxx = any(u.effective_lang() == "c++" for u in self.units)
        tc_kind = ("c" if (has_c and not has_cxx) else "cxx") if self.toolchain_kind == "auto" else self.toolchain_kind
        tc = resolve_toolchain(
            tc_kind,
            allow_msvc=self.allow_msvc,
            msvc_vcvars=self.msvc_vcvars,
            respect_env=self.respect_env,
            needs_c=(has_c and has_cxx),
            skip_message=self.skip_message,
        )
        obj_plans = [_plan_object(tc, u, self.pthread) for u in self.units]
        exe_suffix = ".exe" if os.name == "nt" else ""
        key_payload = json.dumps(
            {
                "schema": CACHE_SCHEMA_VERSION,
                "tc": tc.signature,
                "objs": [p.key for p in obj_plans],
                "libs": self.libs,
                "libs_win": self.libs_windows_only,
                "pthread": self.pthread,
                "link_gnu": self.extra_link_gnu,
                "has_cxx": has_cxx,
            },
            sort_keys=True,
        )
        exe_key = hashlib.sha256(key_payload.encode("utf-8")).hexdigest()
        exe_path = BIN_DIR / f"{self.name}-{exe_key[:20]}{exe_suffix}"
        meta_path = BIN_DIR / f"{exe_path.name}.meta.json"
        return tc, obj_plans, exe_path, meta_path, exe_key

    def _link(
        self,
        tc: ResolvedToolchain,
        obj_plans: list[_ObjectPlan],
        exe_path: Path,
        meta_path: Path,
        exe_key: str,
        compile_outputs: list[str],
    ) -> BuildResult:
        if exe_path.is_file():
            warnings: list[str] = []
            if meta_path.is_file():
                try:
                    warnings = json.loads(meta_path.read_text(encoding="utf-8")).get("warnings", [])
                except OSError:
                    pass
            return BuildResult(exe_path, tc.env, warnings)

        lock = _key_lock(f"exe:{exe_key}")
        with lock:
            if exe_path.is_file():
                warnings = []
                if meta_path.is_file():
                    try:
                        warnings = json.loads(meta_path.read_text(encoding="utf-8")).get("warnings", [])
                    except OSError:
                        pass
                return BuildResult(exe_path, tc.env, warnings)

            _ensure_cache_dirs()
            has_cxx = any(u.effective_lang() == "c++" for u in self.units)
            use_libs = self.libs if (not self.libs_windows_only or os.name == "nt") else ()
            use_pthread = (
                self.pthread == "always" or (self.pthread == "posix" and os.name != "nt")
            )
            tag = f"{os.getpid()}.{threading.get_ident()}"
            tmp_exe = BIN_DIR / f"{exe_path.stem}.tmp.{tag}{exe_path.suffix}"
            link_scratch = BIN_DIR / f".link-{exe_key[:12]}-{tag}"
            link_scratch.mkdir(parents=True, exist_ok=True)
            env = tc.env.copy()
            if "VSCMD_ARG_TGT_ARCH" in env:
                env["TEMP"] = env["TMP"] = str(link_scratch)

            if tc.family == "gnu":
                linker = tc.cxx if has_cxx else tc.cc
                assert linker is not None
                cmd = [linker]
                if use_pthread:
                    cmd.append("-pthread")
                cmd.extend(self.extra_link_gnu)
                cmd.extend(str(p.obj_path) for p in obj_plans)
                cmd.extend(["-o", str(tmp_exe)])
                for lib in use_libs:
                    cmd.append(lib if lib.startswith("-l") else f"-l{lib}")
            else:
                linker = tc.cxx or tc.cc
                assert linker is not None
                cmd = [linker, "/nologo", *(str(p.obj_path) for p in obj_plans), f"/Fe{tmp_exe}"]
                if use_libs:
                    cmd.append("/link")
                    for lib in use_libs:
                        cmd.append(lib if lib.lower().endswith(".lib") else f"{lib}.lib")

            try:
                result = subprocess.run(
                    cmd, cwd=link_scratch, env=env, capture_output=True, text=True, timeout=180
                )
                link_out = result.stdout + result.stderr
                if result.returncode != 0:
                    raise RuntimeError(f"Link failed ({' '.join(cmd)}):\n{link_out}")
                all_lines = "\n".join([*compile_outputs, link_out]).splitlines()
                warnings = [ln for ln in all_lines if _WARN_LINE_RE.search(ln)]
                tmp_meta = link_scratch / "meta.json"
                tmp_meta.write_text(json.dumps({"warnings": warnings}), encoding="utf-8")
                os.replace(tmp_meta, meta_path)
                os.replace(tmp_exe, exe_path)
                return BuildResult(exe_path, tc.env, warnings)
            finally:
                tmp_exe.unlink(missing_ok=True)
                shutil.rmtree(link_scratch, ignore_errors=True)

    def build(self) -> BuildResult:
        if (
            self._registered
            and len(_REGISTERED_TARGETS) > 1
            and _BATCH_WARMED_UP_TO < len(_REGISTERED_TARGETS)
        ):
            warm_registered_targets()
        if self._deferred_exc is not None:
            raise self._deferred_exc
        if self._result is not None and self._result.executable.is_file():
            return self._result
        tc, obj_plans, exe_path, meta_path, exe_key = self._prepare()
        if exe_path.is_file():
            res = self._link(tc, obj_plans, exe_path, meta_path, exe_key, [])
            self._result = res
            return res
        missing = [p for p in obj_plans if not p.obj_path.is_file()]
        if len(missing) == 1:
            _compile_object(missing[0])
        elif len(missing) > 1:
            with ThreadPoolExecutor(max_workers=min(len(missing), MAX_BUILD_WORKERS)) as pool:
                futs = [pool.submit(_compile_object, p) for p in missing]
                for fut in futs:
                    fut.result()
        compile_outputs = [_read_obj_warn(p.obj_path) for p in obj_plans]
        res = self._link(tc, obj_plans, exe_path, meta_path, exe_key, compile_outputs)
        self._result = res
        return res

    def executable(self) -> Path:
        return self.build().executable

    def run(
        self,
        args: Sequence[str] = (),
        *,
        cwd: str | Path | None = None,
        env: dict[str, str] | None = None,
        timeout: float = 60,
        check: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        built = self.build()
        run_env = built.env.copy() if env is None else env
        return subprocess.run(
            [str(built.executable), *(str(a) for a in args)],
            cwd=str(cwd or ROOT),
            env=run_env,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=check,
        )


def register_target(name: str, sources: Sequence[SourceUnit | str | Path], **kwargs) -> NativeTarget:
    kwargs.setdefault("register", True)
    return NativeTarget(name, sources, **kwargs)


def warm_registered_targets() -> None:
    """Compile all unique missing .o/.obj files and link all missing .exe targets in parallel."""
    global _BATCH_WARMED_UP_TO
    with _CACHE_LOCK:
        if _BATCH_WARMED_UP_TO >= len(_REGISTERED_TARGETS):
            return
        batch = list(_REGISTERED_TARGETS[_BATCH_WARMED_UP_TO:])
        _BATCH_WARMED_UP_TO = len(_REGISTERED_TARGETS)

    _ensure_cache_dirs()
    prepared: list[tuple[NativeTarget, ResolvedToolchain, list[_ObjectPlan], Path, Path, str]] = []
    unique_objs: dict[str, _ObjectPlan] = {}

    for target in batch:
        try:
            tc, obj_plans, exe_path, meta_path, exe_key = target._prepare()
        except BaseException as exc:
            target._deferred_exc = exc
            continue
        if exe_path.is_file():
            target._result = target._link(tc, obj_plans, exe_path, meta_path, exe_key, [])
            continue
        prepared.append((target, tc, obj_plans, exe_path, meta_path, exe_key))
        for plan in obj_plans:
            if not plan.obj_path.is_file() and plan.key not in unique_objs:
                unique_objs[plan.key] = plan

    if not prepared:
        return

    obj_errors: dict[str, BaseException] = {}
    if unique_objs:
        with ThreadPoolExecutor(max_workers=min(len(unique_objs), MAX_BUILD_WORKERS)) as pool:
            fut_to_key = {
                pool.submit(_compile_object, plan): key for key, plan in unique_objs.items()
            }
            for fut in as_completed(fut_to_key):
                key = fut_to_key[fut]
                try:
                    fut.result()
                except BaseException as exc:
                    obj_errors[key] = exc

    linkable: list[tuple[NativeTarget, ResolvedToolchain, list[_ObjectPlan], Path, Path, str, list[str]]] = []
    for target, tc, obj_plans, exe_path, meta_path, exe_key in prepared:
        failed = next((obj_errors[p.key] for p in obj_plans if p.key in obj_errors), None)
        if failed is not None:
            target._deferred_exc = failed
            continue
        outputs = [_read_obj_warn(p.obj_path) for p in obj_plans]
        linkable.append((target, tc, obj_plans, exe_path, meta_path, exe_key, outputs))

    if linkable:
        with ThreadPoolExecutor(max_workers=min(len(linkable), MAX_BUILD_WORKERS)) as pool:
            fut_to_target = {
                pool.submit(target._link, tc, obj_plans, exe_path, meta_path, exe_key, outputs): target
                for target, tc, obj_plans, exe_path, meta_path, exe_key, outputs in linkable
            }
            for fut in as_completed(fut_to_target):
                target = fut_to_target[fut]
                try:
                    target._result = fut.result()
                except BaseException as exc:
                    target._deferred_exc = exc


# ---------------------------------------------------------------------------
# Canonical shared source units (maximize .o / .obj deduplication across tests)
# ---------------------------------------------------------------------------

SRC_DIR = ROOT / "src"
TESTS_DIR = ROOT / "tests"
ENET_DIR = ROOT / "thirdparty" / "enet"
ENET_INCLUDE_DIR = ENET_DIR / "include"
ENET_INCLUDE = ENET_INCLUDE_DIR

ENET_C_FILES: tuple[str, ...] = (
    "callbacks.c",
    "compress.c",
    "host.c",
    "list.c",
    "packet.c",
    "peer.c",
    "protocol.c",
    "win32.c",
)

NPCWIRE_BONES_UNIT: SourceUnit = unit("src/npcwire_bones.cpp", opt="-O2", warnings="strict", includes=(SRC_DIR,))
NPCWIRE_CONTROL_UNIT: SourceUnit = unit("src/npcwire_control.cpp", opt="-O2", warnings="strict", includes=(SRC_DIR,))
NPCWIRE_ADAPTER_UNIT: SourceUnit = unit("src/npcwire_adapter.cpp", opt="-O2", warnings="strict", includes=(SRC_DIR,))

NPCWIRE_UNITS: tuple[SourceUnit, ...] = (
    NPCWIRE_BONES_UNIT,
    NPCWIRE_CONTROL_UNIT,
    NPCWIRE_ADAPTER_UNIT,
)

SNAPSHOT_UNIT: SourceUnit = unit(
    "src/snapshot.cpp", opt="-O2", warnings="strict", includes=(SRC_DIR,)
)

ENET_COMPRESS_UNIT: SourceUnit = unit(
    "thirdparty/enet/compress.c",
    lang="c",
    opt="-O2",
    warnings="none",
    includes=(ENET_INCLUDE_DIR,),
)


def enet_units(
    *,
    exclude: Iterable[str] = (),
    opt: str | None = "-O2",
    warnings: str = "none",
    stdcall_gz: bool = False,
    defines: Sequence[str] = (),
    extra_gnu_flags: Sequence[str] = (),
    source_root: Path | None = None,
) -> tuple[SourceUnit, ...]:
    excluded = set(exclude)
    base = (source_root or ROOT) / "thirdparty" / "enet"
    inc = base / "include"
    return tuple(
        unit(
            base / name,
            lang="c",
            opt=opt,
            warnings=warnings,
            stdcall_gz=stdcall_gz,
            defines=defines,
            includes=(inc,),
            extra_gnu_flags=extra_gnu_flags,
        )
        for name in ENET_C_FILES
        if name not in excluded
    )
