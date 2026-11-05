"""Where the private workspace lives: test copies of the game, isolation receipts, builds, run logs.

It is never committed (it is gitignored). Default: the `workspace` folder inside this repository;
override with the environment variable G3_WORKSPACE.
"""
import os
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
WORKSPACE = Path(os.environ.get('G3_WORKSPACE') or REPO/'workspace').resolve()
if not (WORKSPACE/'project.json').is_file():
    raise SystemExit(f'No workspace at {WORKSPACE} (project.json missing). Set G3_WORKSPACE.')
