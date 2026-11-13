# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import uuid


ROOT = Path(__file__).resolve().parents[1]
HOST_ID = 76561198011111111
CLIENT_ID = 76561198022222222
SCENARIOS = ("connect", "metadata", "compressed-connect", "saturation",
             "bad", "rejoin", "compressible", "refusal41")


def win32_tool_environment(build):
    env = os.environ.copy()
    env["TEMP"] = env["TMP"] = str(build)
    if env.get("VSCMD_ARG_TGT_ARCH") == "x86" and shutil.which("cl", path=env.get("PATH")):
        return env
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        raise RuntimeError("Visual Studio vswhere is required to locate the Win32 toolchain")
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
    result = subprocess.run(command, cwd=build, env=env,
                            capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError("Win32 toolchain setup failed:\n" + result.stdout + result.stderr)
    for line in result.stdout.splitlines():
        if "=" in line and not line.startswith("="):
            name, value = line.split("=", 1)
            for existing in list(env):
                if existing.lower() == name.lower() and existing != name:
                    del env[existing]
            env[name] = value
    env["TEMP"] = env["TMP"] = str(build)
    return env


def source_revision(source_root):
    result = subprocess.run(["git", "-C", str(source_root), "rev-parse", "HEAD"],
                            capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError("git rev-parse HEAD failed for source root " +
                           str(source_root) + ": " +
                           (result.stderr.strip() or result.stdout.strip()))
    revision = result.stdout.strip()
    dirty = subprocess.run(["git", "-C", str(source_root), "status", "--porcelain"],
                           capture_output=True, text=True, timeout=30)
    if dirty.returncode:
        raise RuntimeError("git status --porcelain failed for source root " +
                           str(source_root) + ": " +
                           (dirty.stderr.strip() or dirty.stdout.strip()))
    if dirty.stdout.strip():
        revision += "-dirty"
    return revision


def find_fake_header(fake_dll):
    explicit = os.environ.get("G3T_REALISM_FAKE_DIAG_HEADER")
    if explicit:
        candidate = Path(explicit).resolve()
        if candidate.is_file():
            return candidate
        raise RuntimeError("G3T_REALISM_FAKE_DIAG_HEADER is set but not a file: " +
                           str(candidate))
    candidate = (fake_dll.parent / "fake_steam_diag.h").resolve()
    if candidate.is_file():
        return candidate
    return None


def build_peer(source_root, output_dir, fake_dll, header):
    source_root = source_root.resolve()
    output_dir = output_dir.resolve()
    build = output_dir / "build"
    build.mkdir(parents=True, exist_ok=True)
    env = win32_tool_environment(build)
    compiler = shutil.which("cl.exe", path=env.get("PATH") or env.get("Path"))
    if not compiler:
        raise RuntimeError("Win32 compiler not found after vcvars32")

    enet = source_root / "thirdparty" / "enet"
    include_enet = enet / "include"
    sources = sorted(enet.glob("*.c"))
    if not sources:
        raise RuntimeError("historical ENet sources not found at " + str(enet))
    objects = [build / (source.stem + ".obj") for source in sources]
    enet_command = [
        compiler, "/nologo", "/TC", "/c", "/Gz", "/O2", "/W3",
        "/D_CRT_SECURE_NO_WARNINGS", "/D_WINSOCK_DEPRECATED_NO_WARNINGS",
        "/Denet_socket_send=RealismNativeSocketSend",
        "/I" + str(include_enet),
        *(str(source) for source in sources if source.name != "win32.c"),
        "/Fo" + str(build) + "\\",
    ]
    enet_result = subprocess.run(enet_command, cwd=build, env=env,
                                 capture_output=True, text=True, timeout=180)
    if enet_result.returncode:
        raise RuntimeError("ENet build failed:\n" +
                           enet_result.stdout + enet_result.stderr)
    win32_source = enet / "win32.c"
    if win32_source.is_file():
        win32_command = [
            compiler, "/nologo", "/TC", "/c", "/Gz", "/O2", "/W3",
            "/D_CRT_SECURE_NO_WARNINGS", "/D_WINSOCK_DEPRECATED_NO_WARNINGS",
            "/I" + str(include_enet),
            str(win32_source),
            "/Fo" + str(build) + "\\",
        ]
        win32_result = subprocess.run(win32_command, cwd=build, env=env,
                                      capture_output=True, text=True,
                                      timeout=180)
        if win32_result.returncode:
            raise RuntimeError("ENet Win32 build failed:\n" +
                               win32_result.stdout + win32_result.stderr)

    peer_cpp = ROOT / "tests" / "steam_realism_peer.cpp"
    source_cpp = (source_root / "src" / "steam.cpp").resolve()
    if not source_cpp.is_file():
        raise RuntimeError("steam.cpp not found at " + str(source_cpp))
    header_dir = header.parent
    executable = build / "steam_realism_peer.exe"
    peer_command = [
        compiler, "/nologo", "/std:c++17", "/EHsc", "/Gz", "/O2", "/W3",
        "/D_CRT_SECURE_NO_WARNINGS", "/D_WINSOCK_DEPRECATED_NO_WARNINGS",
        "/DG3T_REALISM_SOURCE=\"" +
        str(source_cpp) + "\"",
        "/I" + str(source_root / "src"),
        "/I" + str(include_enet),
        "/I" + str(header_dir),
        str(peer_cpp),
        *(str(obj) for obj in objects),
        "/Fo" + str(build / "steam_realism_peer.obj"),
        "/Fe" + str(executable),
        "/link", "ws2_32.lib", "winmm.lib",
    ]
    peer_result = subprocess.run(peer_command, cwd=build, env=env,
                                 capture_output=True, text=True, timeout=180)
    output = peer_result.stdout + peer_result.stderr
    warnings = [line for line in output.splitlines()
                if "warning C5287:" in line]
    if peer_result.returncode:
        raise RuntimeError("peer build failed:\n" + output)
    return executable, env, warnings


def scenario_environment(fake_dll, output_dir, role, peer_id,
                        peer_port, remote_port, remote_id, scenario, seconds):
    env = os.environ.copy()
    for inherited in list(env):
        if inherited.startswith("G3T_FAKESTEAM_"):
            env.pop(inherited, None)
    env["G3T_REALISM_FAKE_DLL"] = str(fake_dll)
    env["G3T_STEAM_DLL"] = str(fake_dll)
    env["G3T_FAKESTEAM_ID"] = str(peer_id)
    env["G3T_FAKESTEAM_NAME"] = "Realism" + ("Host" if role == "host" else "Client")
    env["G3T_FAKESTEAM_DIR"] = str(output_dir / "fakesteam")
    env["G3T_FAKESTEAM_PORT"] = str(peer_port)
    env["G3T_FAKESTEAM_PROFILE"] = "steam-typical"
    env["G3T_FAKESTEAM_REMOTE"] = f"127.0.0.1:{remote_port}:{remote_id}"
    env["G3T_FAKESTEAM_LISTREMOTE"] = "1"
    for name in (
            "G3T_FAKESTEAM_LATENCY_MS", "G3T_FAKESTEAM_JITTER_MS",
            "G3T_FAKESTEAM_LOSS_PERMILLE", "G3T_FAKESTEAM_ACK_LOSS_PERMILLE",
            "G3T_FAKESTEAM_OUTAGE_MS", "G3T_FAKESTEAM_RECONNECT_MS",
            "G3T_FAKESTEAM_METADATA_DELAY_MS", "G3T_FAKESTEAM_NODELAY_POLICY",
            "G3T_FAKESTEAM_JOIN_FRIEND", "G3T_FAKESTEAM_JOIN_CONNECT",
            "G3T_FAKESTEAM_JOIN_DELAY_MS", "G3T_FAKESTEAM_PHYSICAL_RATE_BPS",
            "G3T_FAKESTEAM_SETUP_MS"):
        env.pop(name, None)
    env["G3T_FAKESTEAM_LATENCY_MS"] = "20"
    env["G3T_FAKESTEAM_JITTER_MS"] = "7"
    env["G3T_FAKESTEAM_LOSS_PERMILLE"] = "0"
    env["G3T_FAKESTEAM_ACK_LOSS_PERMILLE"] = "0"
    env["G3T_FAKESTEAM_SETUP_MS"] = "100"
    env["G3T_FAKESTEAM_METADATA_DELAY_MS"] = "0"
    env["G3T_FAKESTEAM_RECONNECT_MS"] = "0"
    env["G3T_FAKESTEAM_OUTAGE_MS"] = "0"

    if role == "client":
        env["G3T_FAKESTEAM_JOIN_FRIEND"] = str(HOST_ID)
        env["G3T_FAKESTEAM_JOIN_CONNECT"] = (
            "+g3t_connect " + str(HOST_ID) + " realism")
        env["G3T_FAKESTEAM_JOIN_DELAY_MS"] = "0"
    if scenario == "metadata":
        env["G3T_FAKESTEAM_METADATA_DELAY_MS"] = "300"
    elif scenario == "bad":
        env["G3T_FAKESTEAM_PROFILE"] = "steam-bad"
        env["G3T_FAKESTEAM_METADATA_DELAY_MS"] = "300"
        env["G3T_FAKESTEAM_LATENCY_MS"] = "100"
        env["G3T_FAKESTEAM_JITTER_MS"] = "150"
        env["G3T_FAKESTEAM_LOSS_PERMILLE"] = "20"
        env["G3T_FAKESTEAM_ACK_LOSS_PERMILLE"] = "20"
        env["G3T_FAKESTEAM_SETUP_MS"] = "300"
        env["G3T_FAKESTEAM_RECONNECT_MS"] = "10000"
        env["G3T_FAKESTEAM_OUTAGE_MS"] = "3000"
        env["G3T_FAKESTEAM_PHYSICAL_RATE_BPS"] = "102400"
    elif scenario == "compressible":
        env["G3T_FAKESTEAM_PROFILE"] = "steam-typical"
        env["G3T_FAKESTEAM_PHYSICAL_RATE_BPS"] = "262144"
    elif scenario == "refusal41":
        env["G3T_FAKESTEAM_PROFILE"] = "steam-typical"
        env["G3T_FAKESTEAM_NODELAY_POLICY"] = "strict-no-delay-reject"
    env["G3T_FAKESTEAM_DIR"] = str(output_dir)
    env["TEMP"] = env["TMP"] = str(output_dir)
    return env


def peer_args(role, scenario, seconds, leave_ms, offered_bps=None):
    args = [
        "--role", role,
        "--scenario", scenario,
        "--session", "realism",
        "--profile", "compressible" if scenario == "compressible" else
                     "refusal41" if scenario == "refusal41" else "incompressible",
        "--seconds", str(seconds),
        "--app-bps", "400000",
        "--physical-bps", "262144",
        "--host-id", str(HOST_ID),
        "--client-id", str(CLIENT_ID),
    ]
    if scenario == "bad":
        args.extend(["--app-bps", "40000", "--physical-bps", "102400"])
    elif scenario not in ("saturation", "compressible", "refusal41"):
        args.extend(["--app-bps", "40000"])
    if scenario == "refusal41":
        args.extend(["--app-bps", "12000"])
    if scenario == "steady-load":
        if offered_bps is None:
            raise ValueError("steady-load requires an explicit offered rate")
        args.extend(["--app-bps", str(offered_bps)])
    if role == "client" and scenario == "rejoin":
        args.extend(["--leave-at-ms", str(leave_ms)])
    return args


def run_peer(executable, base_env, role, scenario, seconds, output_dir,
             case_dir, leave_ms, peer_port, remote_port, remote_id,
             offered_bps=None):
    namespace = uuid.uuid4().hex
    is_host = role == "host"
    peer_id = HOST_ID if is_host else CLIENT_ID
    env = scenario_environment(
        Path(base_env["G3T_REALISM_FAKE_DLL"]), case_dir, role, peer_id,
        peer_port, remote_port, remote_id, scenario, seconds)
    for name, value in base_env.items():
        if name not in env and not name.upper().startswith("G3T_FAKESTEAM_"):
            env[name] = value
    args = [str(executable), *peer_args(
                role, scenario, seconds, leave_ms, offered_bps),
            "--out", str(case_dir / (role + "-metrics.json"))]
    log = case_dir / (role + ".log")
    with log.open("w", encoding="utf-8", errors="replace") as capture:
        proc = subprocess.Popen(args, cwd=str(case_dir), env=env,
                                stdout=capture, stderr=subprocess.STDOUT,
                                text=True)
    try:
        proc.wait(timeout=seconds + 45)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
        return {"timed_out": True, "returncode": proc.returncode,
                "log": str(log)}
    metric_path = case_dir / (role + "-metrics.json")
    if not metric_path.is_file():
        metrics = {"__metrics_error__": "structured metrics file missing"}
    else:
        try:
            metrics = json.loads(metric_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            metrics = {"__metrics_error__": "structured metrics file invalid: " + str(exc)}
    return {"timed_out": False, "returncode": proc.returncode,
            "log": str(log), "metrics": metrics}


def run_pair(executable, base_env, scenario, seconds, output_dir, case_dir,
             leave_ms, offered_bps=None):
    namespace = uuid.uuid4().hex
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as host_socket, \
            socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client_socket:
        host_socket.bind(("127.0.0.1", 0))
        client_socket.bind(("127.0.0.1", 0))
        host_port = host_socket.getsockname()[1]
        client_port = client_socket.getsockname()[1]
    case_dir.mkdir(parents=True, exist_ok=True)
    fake_dir = case_dir / ("fake-" + namespace)
    fake_dir.mkdir(parents=True, exist_ok=True)
    host_proc = client_proc = None
    host_capture = client_capture = None
    host_log = case_dir / "host.log"
    client_log = case_dir / "client.log"

    def launch(role):
        is_host = role == "host"
        peer_id = HOST_ID if is_host else CLIENT_ID
        peer_port = host_port if is_host else client_port
        remote_port = client_port if is_host else host_port
        remote_id = CLIENT_ID if is_host else HOST_ID
        env = scenario_environment(
            Path(base_env["G3T_REALISM_FAKE_DLL"]), fake_dir, role, peer_id,
            peer_port, remote_port, remote_id, scenario, seconds)
        for name, value in base_env.items():
            if name not in env and not name.upper().startswith("G3T_FAKESTEAM_"):
                env[name] = value
        args = [str(executable), *peer_args(
                    role, scenario, seconds, leave_ms, offered_bps),
                "--out", str(case_dir / (role + "-metrics.json"))]
        log = case_dir / (role + ".log")
        capture = log.open("w", encoding="utf-8", errors="replace")
        proc = subprocess.Popen(args, cwd=str(case_dir), env=env,
                                stdout=capture, stderr=subprocess.STDOUT,
                                text=True)
        return proc, capture, log

    timed_out = set()
    try:
        host_proc, host_capture, host_log = launch("host")
        client_proc, client_capture, client_log = launch("client")
        try:
            host_proc.wait(timeout=seconds + 45)
        except subprocess.TimeoutExpired:
            timed_out.add("host")
        try:
            client_proc.wait(timeout=seconds + 45)
        except subprocess.TimeoutExpired:
            timed_out.add("client")
    finally:
        for role, proc, capture in (
                ("host", host_proc, host_capture),
                ("client", client_proc, client_capture)):
            if proc is not None and proc.poll() is None:
                if role in timed_out:
                    proc.terminate()
                else:
                    proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=10)
            if capture is not None:
                capture.close()

    def result(role, proc, log):
        metric_path = case_dir / (role + "-metrics.json")
        if not metric_path.is_file():
            metrics = {"__metrics_error__": "structured metrics file missing"}
        else:
            try:
                metrics = json.loads(metric_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError as exc:
                metrics = {"__metrics_error__":
                           "structured metrics file invalid: " + str(exc)}
        return {
            "timed_out": role in timed_out,
            "returncode": proc.returncode if proc is not None else -1,
            "log": str(log),
            "metrics": metrics,
        }

    return (result("host", host_proc, host_log),
            result("client", client_proc, client_log))


def merge_metrics(host, client):
    hm = host.get("metrics") or {}
    cm = client.get("metrics") or {}
    if hm and cm:
        plain_bytes = (hm.get("steam_plain_tx_bytes", 0) +
                       cm.get("steam_plain_tx_bytes", 0))
        wire_bytes = (hm.get("steam_wire_tx_bytes", 0) +
                      cm.get("steam_wire_tx_bytes", 0))
        return {
            "host": hm,
            "client": cm,
            "linked": bool(hm.get("linked_count", 0) or
                           cm.get("host_link_observed")),
            "host_enet_connected": bool(hm.get("enet_connected")),
            "client_enet_connected": bool(cm.get("enet_connected")),
            "enet_connected": bool(hm.get("enet_connected") and cm.get("enet_connected")),
            "host_link_observed": bool(hm.get("host_link_observed") and
                                       cm.get("host_link_observed")),
            "channel0_app_offered_bytes": hm.get("channel0_app_offered_bytes",
                                                 cm.get("channel0_app_offered_bytes", 0)),
            "channel0_app_received_bytes": cm.get("channel0_app_received_bytes",
                                                  hm.get("channel0_app_received_bytes", 0)),
            "channel0_outstanding_bytes": max(
                hm.get("channel0_outstanding_bytes", 0),
                cm.get("channel0_outstanding_bytes", 0)),
            "channel0_peak_outstanding_bytes": max(
                hm.get("channel0_peak_outstanding_bytes", 0),
                cm.get("channel0_peak_outstanding_bytes", 0)),
            "channel0_outstanding_slope_bytes_per_second": max(
                hm.get("channel0_outstanding_slope_bytes_per_second", 0),
                cm.get("channel0_outstanding_slope_bytes_per_second", 0)),
            "stress_offer_started": bool(
                hm.get("stress_offer_started") or cm.get("stress_offer_started")),
            "stress_offer_duration_ms": max(
                hm.get("stress_offer_duration_ms", 0),
                cm.get("stress_offer_duration_ms", 0)),
            "stress_offer_bps_observed": max(
                hm.get("stress_offer_bps_observed", 0),
                cm.get("stress_offer_bps_observed", 0)),
            "product_queue_bytes": max(
                hm.get("product_queue_bytes", 0),
                cm.get("product_queue_bytes", 0)),
            "product_queue_depth": max(
                hm.get("product_queue_depth", 0),
                cm.get("product_queue_depth", 0)),
            "product_queue_oldest_ms": max(
                hm.get("product_queue_oldest_ms", 0),
                cm.get("product_queue_oldest_ms", 0)),
            "product_queue_peak_bytes": max(
                hm.get("product_queue_peak_bytes", 0),
                cm.get("product_queue_peak_bytes", 0)),
            "product_queue_peak_depth": max(
                hm.get("product_queue_peak_depth", 0),
                cm.get("product_queue_peak_depth", 0)),
            "product_queue_peak_age_ms": max(
                hm.get("product_queue_peak_age_ms", 0),
                cm.get("product_queue_peak_age_ms", 0)),
            "product_queue_slope_bytes_per_second": max(
                hm.get("product_queue_slope_bytes_per_second", 0),
                cm.get("product_queue_slope_bytes_per_second", 0)),
            "channel0_echo": hm.get("channel0_echo"),
            "channel1_echo": hm.get("channel1_echo"),
            "player_opcode3_echo": hm.get("player_opcode3_echo"),
            "rtt_enet_p50_ms": max(
                hm.get("rtt_enet_p50_ms", 0), cm.get("rtt_enet_p50_ms", 0)),
            "rtt_enet_p95_ms": max(
                hm.get("rtt_enet_p95_ms", 0), cm.get("rtt_enet_p95_ms", 0)),
            "rtt_enet_max_ms": max(
                hm.get("rtt_enet_max_ms", 0), cm.get("rtt_enet_max_ms", 0)),
            "exact_reliable_payloads_sent": (
                hm.get("exact_reliable_payloads_sent", 0) +
                cm.get("exact_reliable_payloads_sent", 0)),
            "exact_reliable_payloads_received": (
                hm.get("exact_reliable_payloads_received", 0) +
                cm.get("exact_reliable_payloads_received", 0)),
            "exact_reliable_payload_mismatch": max(
                hm.get("exact_reliable_payload_mismatch", 0),
                cm.get("exact_reliable_payload_mismatch", 0)),
            "native_connect_header_size": max(
                hm.get("native_connect_header_size", 0),
                cm.get("native_connect_header_size", 0)),
            "native_verify_header_size": max(
                hm.get("native_verify_header_size", 0),
                cm.get("native_verify_header_size", 0)),
            "native_outbound_frames": (
                hm.get("native_outbound_frames", 0) +
                cm.get("native_outbound_frames", 0)),
            "native_outbound_compressed_frames": (
                hm.get("native_outbound_compressed_frames", 0) +
                cm.get("native_outbound_compressed_frames", 0)),
            "native_outbound_connect_frames": (
                hm.get("native_outbound_connect_frames", 0) +
                cm.get("native_outbound_connect_frames", 0)),
            "native_outbound_verify_frames": (
                hm.get("native_outbound_verify_frames", 0) +
                cm.get("native_outbound_verify_frames", 0)),
            "native_outbound_compressed_connect_frames": (
                hm.get("native_outbound_compressed_connect_frames", 0) +
                cm.get("native_outbound_compressed_connect_frames", 0)),
            "native_outbound_compressed_verify_frames": (
                hm.get("native_outbound_compressed_verify_frames", 0) +
                cm.get("native_outbound_compressed_verify_frames", 0)),
            "native_observed_connect_header": max(
                hm.get("native_observed_connect_header", 0),
                cm.get("native_observed_connect_header", 0)),
            "native_observed_verify_header": max(
                hm.get("native_observed_verify_header", 0),
                cm.get("native_observed_verify_header", 0)),
            "steam_plain_tx_bytes": plain_bytes,
            "steam_wire_tx_bytes": wire_bytes,
            "compression_ratio_plain_over_wire": (
                plain_bytes / wire_bytes if wire_bytes else 0),
            "compression_ratio_scope": "both-directions-byte-weighted",
            "fake_stats": {
                "host": hm.get("fake_stats"),
                "client": cm.get("fake_stats"),
            },
            "fake_config": {
                "host": hm.get("fake_config"),
                "client": cm.get("fake_config"),
            },
        }
    return {"host": hm, "client": cm}


def bounded_traffic_reasons(metrics, p95_ms=250, maximum_ms=1000):
    reasons = []
    for key in ("channel1_echo", "player_opcode3_echo"):
        echo = metrics.get(key) or {}
        if not echo.get("samples"):
            reasons.append(key + ": no measured replies")
        elif any(isinstance(echo.get(field), bool) or
                 not isinstance(echo.get(field), (int, float)) or
                 not math.isfinite(echo[field]) or echo[field] < 0
                 for field in ("p95_ms", "max_ms")):
            reasons.append(key + ": invalid measured latency")
        elif echo.get("p95_ms", float("inf")) > p95_ms or echo.get(
                "max_ms", float("inf")) > maximum_ms:
            reasons.append(
                key + ": latency exceeds " + str(p95_ms) +
                "ms p95/" + str(maximum_ms) + "ms maximum")
    if metrics.get("exact_reliable_payload_mismatch") != 0:
        reasons.append("exact reliable payload mismatch")
    if not metrics.get("exact_reliable_payloads_sent"):
        reasons.append("no exact reliable payloads sent")
    if not metrics.get("exact_reliable_payloads_received"):
        reasons.append("no exact reliable payloads received")
    return reasons


def measured_load_reasons(metrics, expected_bps):
    reasons = []
    duration = metrics.get("stress_offer_duration_ms", 0)
    rate = metrics.get("stress_offer_bps_observed", 0)
    if duration < 60000:
        reasons.append("offered load ran for less than 60 seconds after connect")
    if not expected_bps * 0.99 <= rate <= expected_bps * 1.01:
        reasons.append("measured offered rate is not " + str(expected_bps) + " B/s")
    if metrics.get("channel0_app_offered_bytes", 0) < expected_bps * 60 - 1200:
        reasons.append("required application bytes were not actually offered")
    return reasons


def evaluate_case(name, host, client, merge):
    if host.get("timed_out") or client.get("timed_out"):
        return "NOT_REACHED", ["process timeout"]
    hm = host.get("metrics") or {}
    cm = client.get("metrics") or {}
    if not hm or not cm:
        return "NOT_REACHED", ["peer metrics unavailable"]
    if "__metrics_error__" in hm or "__metrics_error__" in cm:
        return "NOT_REACHED", [
            "structured peer metrics missing or invalid",
            hm.get("__metrics_error__", cm.get("__metrics_error__", "")),
        ]
    if (hm.get("schema") != 1 or cm.get("schema") != 1 or
            hm.get("role") != "host" or cm.get("role") != "client"):
        return "FAIL", ["invalid structured peer schema or role"]
    if any(not isinstance(peer.get("seeded_bypass"), bool) for peer in (hm, cm)):
        return "FAIL", ["explicit invite-bypass metric unavailable"]
    if hm.get("seeded_bypass") or cm.get("seeded_bypass"):
        return "FAIL", ["a seeded link bypassed the real invite handshake"]
    if any(peer.get("sampled_before_stop") is not True or
           peer.get("fake_stats_available") is not True or
           peer.get("fake_config_available") is not True for peer in (hm, cm)):
        return "FAIL", ["required live, matching fake diagnostic ABI was not sampled before Stop"]
    if host.get("returncode") not in (0,) or client.get("returncode") not in (0,):
        reasons = ["peer process exit nonzero"]
        if hm.get("error"): reasons.append("host: " + hm["error"])
        if cm.get("error"): reasons.append("client: " + cm["error"])
        if not hm.get("ready") or not cm.get("ready"):
            reasons.append("fake Steam Start/hello/welcome did not become ready")
        return "FAIL", reasons

    if name in ("saturation", "compressible", "steady-load"):
        required = ("product_queue_bytes", "product_queue_oldest_ms",
                    "product_queue_slope_bytes_per_second", "rtt_enet_p95_ms")
        for role, peer in (("host", hm), ("client", cm)):
            for key in required:
                value = peer.get(key)
                if (isinstance(value, bool) or not isinstance(value, (int, float))
                        or not math.isfinite(value)):
                    return "FAIL", [role + ": required numeric metric unavailable: " + key]

    if name == "compressed-connect":
        hc = merge.get("native_connect_header_size", 0)
        hv = merge.get("native_verify_header_size", 0)
        observed_connect = merge.get("native_observed_connect_header", 0)
        observed_verify = merge.get("native_observed_verify_header", 0)
        outbound_connect = merge.get("native_outbound_connect_frames", 0)
        outbound_verify = merge.get("native_outbound_verify_frames", 0)
        compressed_connect = merge.get(
            "native_outbound_compressed_connect_frames", 0)
        compressed_verify = merge.get(
            "native_outbound_compressed_verify_frames", 0)
        if outbound_connect == 0 or compressed_connect == 0 or observed_connect != 48:
            return "NOT_REACHED", ["actual range-compressed native 48-byte CONNECT was not emitted"]
        if hc != 48:
            return "FAIL", [
                "native ENet CONNECT classifier is " + str(hc) +
                " bytes, expected real48"
            ]
        if hv != 44:
            return "FAIL", [
                "native ENet VERIFY_CONNECT classifier is " + str(hv) +
                " bytes, expected real44"
            ]
        hs = hm.get("fake_stats") or {}
        cs = cm.get("fake_stats") or {}
        if max(hs.get("callback504_count", 0),
               cs.get("callback504_count", 0)) == 0:
            return "FAIL", ["no LobbyEnter callback504"]
        if outbound_connect == 0 or outbound_verify == 0:
            return "FAIL", [
                "actual compressed native CONNECT was emitted but no VERIFY completed",
                "connect_frames=" + str(outbound_connect) +
                " verify_frames=" + str(outbound_verify),
            ]
        if observed_connect != 48 or observed_verify != 44:
            return "FAIL", [
                "captured native command header mismatch",
                "CONNECT=" + str(observed_connect) +
                " VERIFY_CONNECT=" + str(observed_verify),
            ]
        if compressed_connect == 0 or compressed_verify == 0:
            return "FAIL", [
                "captured native handshake frames were not both range-compressed",
                "compressed_connect=" + str(compressed_connect) +
                " compressed_verify=" + str(compressed_verify),
            ]
        linked = hm.get("host_link_observed") and cm.get("host_link_observed")
        connected = hm.get("enet_connected") and cm.get("enet_connected")
        if not linked or not connected:
            return "FAIL", [
                "actual native compressed CONNECT did not complete the real Steam/ENet handshake"
            ]
        return "PASS", [
            "compressed native 48-byte CONNECT and 44-byte VERIFY_CONNECT"
        ]

    if name == "metadata":
        hc = hm.get("fake_config") or {}
        cc = cm.get("fake_config") or {}
        delay = max(hc.get("metadata_delay_ms", 0), cc.get("metadata_delay_ms", 0))
        hs = hm.get("fake_stats") or {}
        cs = cm.get("fake_stats") or {}
        callbacks504 = max(hs.get("callback504_count", 0),
                           cs.get("callback504_count", 0))
        linked = hm.get("host_link_observed") and cm.get("host_link_observed")
        if delay != 300:
            return "FAIL", ["metadata delay was not 300ms"]
        if callbacks504 == 0:
            return "FAIL", ["LobbyEnter callback504 was not observed"]
        if not cm.get("lobby_enter_callback_observed"):
            return "NOT_REACHED", ["no actual delivered client LobbyEnter callback"]
        if not cm.get("metadata_empty_at_lobby_enter"):
            return "FAIL", ["fixture did not expose empty metadata after LobbyEnter"]
        if not cm.get("metadata_update_callback_observed"):
            return "FAIL", ["empty metadata at actual LobbyEnter was not recovered through LobbyDataUpdate"]
        if not linked:
            return "FAIL", [
                "metadata Steam H/W link/hello/welcome progress was not observed"
            ]
        lobby_to_link = cm.get("lobby_to_link_ms",
                               hm.get("lobby_to_link_ms", 0))
        if lobby_to_link + 1 < delay:
            return "FAIL", ["Steam H/W link occurred before the 300ms metadata delay"]
        return "PASS", [
            "metadata delay was observed after LobbyEnter and before Steam H/W link"
        ]

    linked = hm.get("host_link_observed") and cm.get("host_link_observed")
    connected = hm.get("enet_connected") and cm.get("enet_connected")
    if not linked or not connected:
        if name == "connect" and merge.get("native_outbound_connect_frames", 0):
            return "FAIL", ["actual native CONNECT was emitted but the handshake did not complete"]
        if name in ("bad", "rejoin") and (
                hm.get("enet_connect_count", 0) or cm.get("enet_connect_count", 0)):
            return "FAIL", ["initial native connection completed but recovery did not complete"]
        return "NOT_REACHED", [
            "real Steam hello/welcome/ENet handshake did not complete"
        ]

    if name == "connect":
        mismatch = merge.get("exact_reliable_payload_mismatch", 0)
        sent = merge.get("exact_reliable_payloads_sent", 0)
        received = merge.get("exact_reliable_payloads_received", 0)
        if mismatch != 0:
            return "FAIL", ["exact reliable payload mismatch=" + str(mismatch)]
        if sent == 0:
            return "NOT_REACHED", [
                "real ENet exact reliable payload echo coverage was incomplete",
                "sent=" + str(sent) + " received=" + str(received),
            ]
        if received == 0:
            return "FAIL", ["connected peers sent real reliable payloads but none were delivered"]
        return "PASS", [
            "accepted synthetic command-line invite; both Steam links and both ENet peers connected",
            "exact reliable payload sent=" + str(sent) +
            " received=" + str(received),
        ]

    if name in ("saturation", "steady-load"):
        expected_bps = 400000
        if name == "steady-load":
            expected_bps = hm.get("app_bps_requested")
            if (isinstance(expected_bps, bool) or
                    not isinstance(expected_bps, int) or expected_bps <= 0):
                return "FAIL", ["steady-load has no explicit positive offered rate"]
        reasons = measured_load_reasons(merge, expected_bps)
        if reasons:
            return "NOT_REACHED", reasons
        for peer in (hm, cm):
            config = peer.get("fake_config") or {}
            if (config.get("profile") != "steam-typical" or
                    config.get("physical_rate_bps") != 262144 or
                    config.get("nodelay_policy") != 0):
                return "FAIL", ["saturation did not use the stated typical physical profile"]
        reasons = bounded_traffic_reasons(merge)
        if merge.get("product_queue_slope_bytes_per_second", float("inf")) > 1024:
            reasons.append("measured retained queue grows by more than 1024 B/s")
        if merge.get("product_queue_bytes", float("inf")) > 262144:
            reasons.append("measured final retained queue exceeds 256 KiB")
        if merge.get("product_queue_oldest_ms", float("inf")) > 1000:
            reasons.append("measured oldest retained datagram exceeds one second")
        if merge.get("rtt_enet_p95_ms", float("inf")) > 1000:
            reasons.append("measured native ENet RTT p95 exceeds one second")
        refusals = sum((peer.get("fake_stats") or {}).get("result25_count", 0)
                       for peer in (hm, cm))
        if name == "saturation" and not refusals:
            reasons.append("finite-buffer result25 path was not exercised")
        return ("FAIL", reasons) if reasons else (
            "PASS", [
                "measured 60-second offered load, retained backlog and critical latency are bounded",
                "offered_application_bps=" + str(expected_bps) +
                " result25=" + str(refusals),
            ])

    if name == "bad":
        hs = hm.get("fake_stats") or {}
        cs = cm.get("fake_stats") or {}
        hfc = hm.get("fake_config") or {}
        cfc = cm.get("fake_config") or {}
        config = hfc if hfc else cfc
        if config.get("physical_rate_bps") != 102400:
            return "FAIL", ["bad profile physical rate is not 102400 B/s"]
        if config.get("loss_permille", 0) < 20:
            return "FAIL", ["bad profile loss is not 2%"]
        if config.get("jitter_ms", 0) < 150:
            return "FAIL", ["bad profile jitter spike is not 150ms"]
        if config.get("queue_delay_ms") != 2000:
            return "FAIL", ["bad profile queue-delay fixture is not two seconds"]
        reasons = bounded_traffic_reasons(merge, 2500, 5000)
        if not hs.get("dropped_data_packets", 0) + cs.get("dropped_data_packets", 0):
            reasons.append("configured loss never actually dropped a datagram")
        if not hm.get("session_failed_callbacks", 0) + cm.get("session_failed_callbacks", 0):
            reasons.append("no actual delivered session-failure callback")
        if not hs.get("queue_delay_events", 0) + cs.get("queue_delay_events", 0):
            reasons.append("no actual bufferbloat delay was exercised")
        if max(hs.get("max_queue_delay_ms", 0), cs.get("max_queue_delay_ms", 0)) < 2000:
            reasons.append("actual bufferbloat delay did not reach two seconds")
        if cm.get("enet_connect_count", 0) < 2:
            reasons.append("no second successful native ENet connection")
        if not cm.get("recovered_echo_samples", 0):
            reasons.append("no payload reply on the restored connection")
        return ("FAIL", reasons) if reasons else (
            "PASS", ["observed loss, session failure and bounded post-reconnect payload replies"])

    if name == "rejoin":
        rejoin = cm.get("rejoin_count", 0)
        if rejoin == 0:
            return "FAIL", ["renewed accepted invite/rejoin was not injected"]
        if cm.get("link_lost_count", 0) == 0:
            return "FAIL", ["client leave/disconnect was not observed"]
        if cm.get("enet_connect_count", 0) < 2 or cm.get("steam_link_count", 0) < 2:
            return "FAIL", ["renewed ENet link did not reconnect"]
        if not cm.get("recovered_echo_samples", 0):
            return "FAIL", ["no payload reply after rejoining"]
        return "PASS", ["client leave and renewed accepted invite/rejoin completed"]

    if name == "compressible":
        for peer in (hm, cm):
            config = peer.get("fake_config") or {}
            if (config.get("profile") != "steam-typical" or
                    config.get("physical_rate_bps") != 262144 or
                    config.get("nodelay_policy") != 0):
                return "FAIL", ["compressible workload did not use the stated typical physical profile"]
        plain = hm.get("steam_plain_tx_bytes")
        wire = hm.get("steam_wire_tx_bytes")
        if any(isinstance(value, bool) or
               not isinstance(value, (int, float)) or
               not math.isfinite(value) or value <= 0
               for value in (plain, wire)):
            return "FAIL", ["no valid host application/wire traffic counters"]
        ratio = plain / wire
        if ratio < 1.1:
            return "FAIL", ["host workload did not demonstrate real compression"]
        reasons = measured_load_reasons(merge, 400000)
        reasons += bounded_traffic_reasons(merge)
        if reasons:
            return "FAIL", reasons
        if merge.get("product_queue_bytes", float("inf")) > 262144:
            return "FAIL", ["compressible workload retained more than 256 KiB"]
        if merge.get("product_queue_slope_bytes_per_second", float("inf")) > 1024:
            return "FAIL", ["compressible workload retained queue keeps growing"]
        if merge.get("product_queue_oldest_ms", float("inf")) > 1000:
            return "FAIL", ["compressible workload retained a datagram for more than one second"]
        return "PASS", [
            "compressible 400000 B/s app case reports separate wire/app rates",
            "host_plain_wire_ratio=" + str(ratio),
        ]

    if name == "refusal41":
        hs = hm.get("fake_stats") or {}
        cs = cm.get("fake_stats") or {}
        hc = hm.get("fake_config") or {}
        cc = cm.get("fake_config") or {}
        config = hc if hc.get("nodelay_policy") is not None else cc
        count41 = max(hs.get("result41_count", 0), cs.get("result41_count", 0))
        if config.get("nodelay_policy") != 1:
            return "FAIL", ["strict-no-delay-reject fixture was not active"]
        if count41 == 0:
            return "FAIL", ["no real flat-API result41 refusal was observed"]
        exactMismatch = merge.get("exact_reliable_payload_mismatch", 0)
        if exactMismatch:
            return "FAIL", ["strict-refusal exact payload mismatch"]
        reasons = bounded_traffic_reasons(merge)
        if reasons:
            return "FAIL", reasons
        return "PASS", ["synthetic 41 fixture: queue/41 handling, not Valve NoDelay behavior"]

    return "NOT_REACHED", ["unknown scenario"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fake-dll", required=True)
    parser.add_argument("--source-root", default=str(ROOT))
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--scenario", default="all",
                        choices=("all",) + SCENARIOS + ("steady-load",))
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--offered-bps", type=int,
                        help="explicit application offer for separate steady-load case")
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    if args.scenario == "steady-load":
        if args.offered_bps is None or not 0 < args.offered_bps <= 0xFFFFFFFF:
            parser.error("steady-load requires --offered-bps in 1..4294967295")
    elif args.offered_bps is not None:
        parser.error("--offered-bps is only valid for the separate steady-load case")

    fake_dll = Path(args.fake_dll).resolve()
    source_root = Path(args.source_root).resolve()
    output_dir = Path(args.output_dir).resolve()
    if not fake_dll.is_file():
        print("ERROR: --fake-dll file not found: " + str(fake_dll),
              file=sys.stderr)
        return 2
    if not (source_root / "src" / "steam.cpp").is_file():
        print("ERROR: source root has no src/steam.cpp: " + str(source_root),
              file=sys.stderr)
        return 2
    try:
        header = find_fake_header(fake_dll)
        revision = source_revision(source_root)
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        return 2
    if not header:
        print("ERROR: fake_steam_diag.h not found beside --fake-dll or staged fakesteam directory",
              file=sys.stderr)
        return 2
    try:
        if output_dir.exists() and any(output_dir.iterdir()):
            raise RuntimeError("--output-dir must be a fresh empty directory")
        output_dir.mkdir(parents=True, exist_ok=True)
        executable, env, warnings = build_peer(
            source_root, output_dir, fake_dll, header)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        return 2

    selected = (
        list(SCENARIOS) if args.scenario == "all" else [args.scenario])
    cases = []
    for name in selected:
        seconds = args.seconds
        if args.scenario == "all" and args.seconds == 60:
            if name not in ("saturation", "compressible"):
                seconds = 10
        if name in ("saturation", "compressible", "steady-load"):
            seconds = max(seconds, 60)
        elif name == "bad":
            seconds = max(seconds, 20)
        elif name == "rejoin":
            seconds = max(seconds, 10)
        case_dir = output_dir / name
        case_dir.mkdir(parents=True, exist_ok=True)
        base_env = env.copy()
        base_env["G3T_REALISM_FAKE_DLL"] = str(fake_dll)
        base_env["G3T_STEAM_DLL"] = str(fake_dll)
        base_env["G3T_REALISM_FAKE_DIAG_HEADER"] = str(header)

        host, client = run_pair(executable, base_env, name, seconds,
                                output_dir, case_dir, 3000, args.offered_bps)
        merged = merge_metrics(host, client)
        status, reasons = evaluate_case(name, host, client, merged)
        cases.append({
            "name": name,
            "status": status,
            "reasons": reasons,
            "metrics": merged,
            "host": {
                "timed_out": host["timed_out"],
                "returncode": host["returncode"],
                "log": str(host["log"]),
            },
            "client": {
                "timed_out": client["timed_out"],
                "returncode": client["returncode"],
                "log": str(client["log"]),
            },
            "strict_fixture": name == "refusal41",
        })

    summary = {
        "schema": 1,
        "source_revision": revision,
        "source_root": str(source_root),
        "fake_dll": str(fake_dll),
        "fake_dll_marker": "G3T_REALISM_FAKE_DLL",
        "cases": cases,
        "warnings": warnings,
        "disclaimer": {
            "refusal41": "synthetic strict-no-delay-reject fixture only; not Valve NoDelay fidelity",
            "saturation": "400KB/s app over a fixed 262144B/s physical link; measured load, queue and latency bounds decide PASS/FAIL",
            "steady-load": "separate opaque transport-volume proxy at an explicitly measured rate, not replay of production NPC serializer output",
        },
        "acceptance": {
            "load_duration_ms": 60000,
            "offered_bps": args.offered_bps if args.scenario == "steady-load" else 400000,
            "retained_queue_end_bytes_max": 262144,
            "retained_queue_slope_bps_max": 1024,
            "retained_queue_age_ms_max": 1000,
            "typical_player_critical_p95_ms_max": 250,
            "typical_player_critical_max_ms": 1000,
            "bad_player_critical_p95_ms_max": 2500,
            "bad_player_critical_max_ms": 5000,
        },
    }
    summary_path = output_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n",
                            encoding="utf-8")
    for case in cases:
        print(case["name"] + " | " + case["status"] + " | " +
              "; ".join(case["reasons"]))
    print("STRUCTURED_REPORT: " + str(summary_path))
    failed = any(case["status"] in ("FAIL", "NOT_REACHED") for case in cases)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
