# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the same engine-free Steam scenarios against detached historical sources."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
REVISIONS = (("build64", "f7d81647"), ("build65", "5f6512ef"),
             ("build66", "b6096c08"), ("current", "HEAD"))


def git(*arguments):
    try:
        result = subprocess.run(
            ["git", "-c", "maintenance.auto=false", "-c", "gc.auto=0", *arguments],
            cwd=ROOT, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"git operation exceeded 120 seconds: {arguments}") from error
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout.strip()


def run_history(fake_dll, output, scenario="all", seconds=60):
    fake_dll = fake_dll.resolve(strict=True)
    output = output.resolve()
    if output.exists() and any(output.iterdir()):
        raise RuntimeError("--output-dir must be a fresh empty directory")
    output.mkdir(parents=True, exist_ok=True)
    matrix = {"schema": 1, "scenario": scenario, "seconds": seconds, "runs": []}
    for label, revision in REVISIONS:
        run_dir = output / label
        run_dir.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=f"g3t-realism-{label}-") as scratch:
            source = Path(scratch) / "source"
            if source.is_relative_to(ROOT):
                raise RuntimeError("Historical worktrees must be outside the repository")
            added = False
            try:
                git("worktree", "add", "--detach", str(source), revision)
                added = True
                sha = git("-C", str(source), "rev-parse", "HEAD")
                # The runner and scenarios are always current; only production
                # source/headers/ENet come from the chosen detached revision.
                command = [
                    sys.executable, str(ROOT / "tools" / "run_steam_realism.py"),
                    "--fake-dll", str(fake_dll), "--source-root", str(source),
                    "--output-dir", str(run_dir), "--scenario", scenario,
                    "--seconds", str(seconds),
                ]
                environment = os.environ.copy()
                with (output / f"{label}-runner-output.txt").open(
                        "w", encoding="utf-8") as stream:
                    result = subprocess.run(command, cwd=ROOT, env=environment,
                                            stdout=stream, stderr=subprocess.STDOUT)
                summary = run_dir / "summary.json"
                if not summary.is_file():
                    raise RuntimeError(f"{label}: runner produced no summary: {run_dir}")
                observed = json.loads(summary.read_text(encoding="utf-8"))
                matrix["runs"].append({
                    "label": label, "revision": sha, "exit_code": result.returncode,
                    "summary": observed,
                })
                statuses = ", ".join(
                    f"{case['name']}={case['status']}" for case in observed["cases"])
                print(f"{label} {sha[:8]}: {statuses}", flush=True)
            finally:
                if added or (source / ".git").is_file():
                    # Never force-remove a changed historical source tree.
                    git("worktree", "remove", str(source))
        (output / "matrix.json").write_text(
            json.dumps(matrix, indent=2) + "\n", encoding="utf-8")
    return matrix


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fake-dll", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--scenario", default="all")
    parser.add_argument("--seconds", type=int, default=60)
    arguments = parser.parse_args()
    if arguments.seconds <= 0:
        parser.error("--seconds must be positive")
    try:
        matrix = run_history(arguments.fake_dll, arguments.output_dir,
                             arguments.scenario, arguments.seconds)
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    current = next(run for run in matrix["runs"] if run["label"] == "current")
    # Old failures are observations, not expected-result substitutions.
    return 0 if current["exit_code"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
