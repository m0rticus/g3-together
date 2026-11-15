#!/usr/bin/env python3
"""Diagnose a hung 32-bit Gothic 3 process without launching or driving it.

Examples:
  python tools/hangstack.py --pid 1234
  python tools/hangstack.py --pid 1234 --output hang.txt
"""

from __future__ import annotations

import argparse
import ctypes
import os
import sys
from ctypes import wintypes as W
from pathlib import Path


PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
THREAD_SUSPEND_RESUME = 0x0002
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_INFORMATION = 0x0040
TH32CS_SNAPTHREAD = 0x4
LIST_MODULES_32BIT = 1
WOW64_CONTEXT_FULL = 0x10000 | 0x1 | 0x2
STACK_BYTES = 64 * 1024
FRAMES = 60
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class FloatSave(ctypes.Structure):
    _fields_ = [
        ("ControlWord", W.DWORD), ("StatusWord", W.DWORD), ("TagWord", W.DWORD),
        ("ErrorOffset", W.DWORD), ("ErrorSelector", W.DWORD), ("DataOffset", W.DWORD),
        ("DataSelector", W.DWORD), ("RegisterArea", ctypes.c_ubyte * 80),
        ("Cr0NpxState", W.DWORD),
    ]


class Wow64Context(ctypes.Structure):
    _fields_ = [
        ("ContextFlags", W.DWORD), ("Dr0", W.DWORD), ("Dr1", W.DWORD), ("Dr2", W.DWORD),
        ("Dr3", W.DWORD), ("Dr6", W.DWORD), ("Dr7", W.DWORD), ("FloatSave", FloatSave),
        ("SegGs", W.DWORD), ("SegFs", W.DWORD), ("SegEs", W.DWORD), ("SegDs", W.DWORD),
        ("Edi", W.DWORD), ("Esi", W.DWORD), ("Ebx", W.DWORD), ("Edx", W.DWORD),
        ("Ecx", W.DWORD), ("Eax", W.DWORD), ("Ebp", W.DWORD), ("Eip", W.DWORD),
        ("SegCs", W.DWORD), ("EFlags", W.DWORD), ("Esp", W.DWORD), ("SegSs", W.DWORD),
        ("ExtendedRegisters", ctypes.c_ubyte * 512),
    ]


class ThreadEntry(ctypes.Structure):
    _fields_ = [
        ("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ThreadID", W.DWORD),
        ("th32OwnerProcessID", W.DWORD), ("tpBasePri", W.LONG), ("tpDeltaPri", W.LONG),
        ("dwFlags", W.DWORD),
    ]


class ModuleInfo(ctypes.Structure):
    _fields_ = [
        ("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", W.DWORD),
        ("EntryPoint", ctypes.c_void_p),
    ]


def modules(kernel, psapi, process):
    handles = (W.HMODULE * 1024)()
    needed = W.DWORD()
    if not psapi.EnumProcessModulesEx(process, handles, ctypes.sizeof(handles), ctypes.byref(needed), LIST_MODULES_32BIT):
        return []
    found = []
    for module in handles[:needed.value // ctypes.sizeof(W.HMODULE)]:
        name = ctypes.create_unicode_buffer(260)
        info = ModuleInfo()
        if psapi.GetModuleBaseNameW(process, module, name, 260) and psapi.GetModuleInformation(
            process, module, ctypes.byref(info), ctypes.sizeof(info)
        ):
            found.append((info.lpBaseOfDll or 0, info.SizeOfImage, name.value))
    return sorted(found)


def where(address, mods):
    for base, size, name in mods:
        if base <= address < base + size:
            return f"{name}+{address - base:#x}"
    return None


def read(process, address, size, kernel):
    buffer = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    if not kernel.ReadProcessMemory(
        process, ctypes.c_void_p(address), buffer, size, ctypes.byref(got)
    ):
        return b""
    return buffer.raw[:got.value]


def read_stack(process, esp, kernel):
    data = read(process, esp, 0x1000 - (esp & 0xFFF), kernel)
    while data and len(data) < STACK_BYTES:
        page = read(process, esp + len(data), 0x1000, kernel)
        if not page:
            break
        data += page
    return data


def after_call(process, address, kernel):
    data = read(process, address - 7, 7, kernel)
    if len(data) < 7:
        return False
    b = data
    return (
        b[2] == 0xE8
        or (b[1] == 0xFF and b[2] in (0x15, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97))
        or (b[5] == 0xFF and (b[6] & 0xF8) == 0xD0)
        or (b[4] == 0xFF and (b[5] & 0xF8) in (0x50, 0x10) and (b[5] & 0x38) == 0x10)
        or (b[5] == 0xFF and (b[6] & 0x38) == 0x10 and b[6] < 0xC0 and (b[6] & 7) not in (4, 5))
    )


def thread_ids(pid, kernel):
    snap = kernel.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if not snap or snap == INVALID_HANDLE_VALUE or snap == -1:
        return None
    entry = ThreadEntry()
    entry.dwSize = ctypes.sizeof(entry)
    ids = []
    ok = kernel.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            ids.append(entry.th32ThreadID)
        ok = kernel.Thread32Next(snap, ctypes.byref(entry))
    kernel.CloseHandle(snap)
    return ids


def created(thread, kernel):
    times = [W.FILETIME() for _ in range(4)]
    kernel.GetThreadTimes(thread, *[ctypes.byref(t) for t in times])
    return (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime


def sample(pid: int) -> str:
    if os.name != "nt":
        return f"hangstack: process diagnostics require Windows; PID {pid} was not sampled\n"

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    kernel.OpenProcess.restype = W.HANDLE
    kernel.OpenThread.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    kernel.OpenThread.restype = W.HANDLE
    kernel.CreateToolhelp32Snapshot.argtypes = [W.DWORD, W.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = W.HANDLE
    kernel.Thread32First.argtypes = [W.HANDLE, ctypes.POINTER(ThreadEntry)]
    kernel.Thread32First.restype = W.BOOL
    kernel.Thread32Next.argtypes = [W.HANDLE, ctypes.POINTER(ThreadEntry)]
    kernel.Thread32Next.restype = W.BOOL
    kernel.CloseHandle.argtypes = [W.HANDLE]
    kernel.CloseHandle.restype = W.BOOL
    kernel.Wow64SuspendThread.argtypes = [W.HANDLE]
    kernel.Wow64SuspendThread.restype = W.DWORD
    kernel.Wow64GetThreadContext.argtypes = [W.HANDLE, ctypes.POINTER(Wow64Context)]
    kernel.Wow64GetThreadContext.restype = W.BOOL
    kernel.ResumeThread.argtypes = [W.HANDLE]
    kernel.ResumeThread.restype = W.DWORD
    kernel.GetThreadTimes.argtypes = [W.HANDLE] + [ctypes.POINTER(W.FILETIME)] * 4
    kernel.GetThreadTimes.restype = W.BOOL
    kernel.ReadProcessMemory.argtypes = [
        W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    kernel.ReadProcessMemory.restype = W.BOOL
    psapi.EnumProcessModulesEx.argtypes = [
        W.HANDLE, ctypes.POINTER(W.HMODULE), W.DWORD, ctypes.POINTER(W.DWORD), W.DWORD,
    ]
    psapi.GetModuleBaseNameW.argtypes = [W.HANDLE, W.HMODULE, W.LPWSTR, W.DWORD]
    psapi.GetModuleInformation.argtypes = [
        W.HANDLE, W.HMODULE, ctypes.POINTER(ModuleInfo), W.DWORD,
    ]

    process = kernel.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not process:
        return f"hangstack: cannot open process {pid} (error {ctypes.get_last_error()})\n"

    mods = modules(kernel, psapi, process)
    blocks = []
    tids = thread_ids(pid, kernel)
    if tids is None:
        kernel.CloseHandle(process)
        return f"hangstack: cannot snapshot threads for process {pid}\n"
    for tid in tids:
        thread = kernel.OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
            False,
            tid,
        )
        if not thread:
            continue
        context = Wow64Context()
        context.ContextFlags = WOW64_CONTEXT_FULL
        stack = b""
        context_ok = False
        failure = ""
        if kernel.Wow64SuspendThread(thread) not in (-1, 0xFFFFFFFF):
            try:
                if kernel.Wow64GetThreadContext(thread, ctypes.byref(context)):
                    context_ok = True
                    stack = read_stack(process, context.Esp, kernel)
                else:
                    failure = f"context-failed error={ctypes.get_last_error()}"
            finally:
                kernel.ResumeThread(thread)
        else:
            failure = f"suspend-failed error={ctypes.get_last_error()}"
        born = created(thread, kernel)
        kernel.CloseHandle(thread)
        if not context_ok:
            blocks.append((born, f"thread {tid} {failure}"))
            continue
        frames = []
        for at in range(0, len(stack) - 3, 4):
            value = int.from_bytes(stack[at:at + 4], "little")
            name = where(value, mods)
            if name and after_call(process, value, kernel):
                frames.append(f"  esp+{at:#06x} {name}")
                if len(frames) == FRAMES:
                    break
        head = (
            f"thread {tid} eip={where(context.Eip, mods) or hex(context.Eip)} "
            f"esp={context.Esp:#x} ebp={context.Ebp:#x}"
        )
        blocks.append((born, head + ("\n" + "\n".join(frames) if frames else "")))
    kernel.CloseHandle(process)
    return (
        f"hangstack: process {pid}, {len(blocks)} threads, {len(mods)} modules; oldest thread first\n"
        + "\n".join(text for _, text in sorted(blocks))
        + "\n"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Manually sample a hung 32-bit Gothic 3 process."
    )
    parser.add_argument("pid_positional", nargs="?", type=int, help="process ID")
    parser.add_argument("output_positional", nargs="?", type=Path, help="optional output file")
    parser.add_argument("--pid", type=int, help="explicit process ID")
    parser.add_argument("-o", "--output", type=Path, help="explicit output file")
    args = parser.parse_args(argv)

    pid = args.pid if args.pid is not None else args.pid_positional
    output = args.output if args.output is not None else args.output_positional
    if pid is None:
        parser.error("a process ID is required via --pid or the positional PID")
    if pid < 1:
        parser.error("PID must be a positive integer")

    text = sample(pid)
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 1 if text.startswith(("hangstack: cannot open process", "hangstack: cannot snapshot", "hangstack: process diagnostics require")) else 0


if __name__ == "__main__":
    raise SystemExit(main())
