"""Send QA commands to the games of a running `together.py --suite warm` and print the new log lines.

  python live.py host "teleportto <spot>"      run one command on the host, print what the game logged
  python live.py client npchp                  the same on the client
  python live.py both state                    both machines
  python live.py stop                          ask the warm runner to shut both games down (normal close + report)
"""
import json
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bot import Bot  # noqa: E402
from workspace import WORKSPACE  # noqa: E402

PAIR = os.environ.get('G3_PAIR', '1')
SUFFIX = '' if PAIR == '1' else PAIR


class Running:
    """Stands in for the runner's Popen: the games belong to the warm runner, this process only talks to them."""
    returncode = None

    def poll(self):
        return None


def connect(tag, info):
    root = Path(info['roots'][tag])
    bot = Bot(Running(), Path(info['run']), time.monotonic()+3600, root=root, name=tag)
    # Continue the runner's sequence numbers: the hook ignores a command that does not count up.
    seen = [int(n) for n in re.findall(r'COMMAND sequence=(\d+) ', bot.log())]
    bot.sequence = max(seen, default=1) + 1000
    return bot


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    session = WORKSPACE/f'runtime/warm-session{SUFFIX}.json'
    if sys.argv[1] == 'stop':
        (WORKSPACE/f'runtime/warm-stop{SUFFIX}.txt').write_text('stop\n')
        return
    if not session.is_file() or len(sys.argv) < 3:
        raise SystemExit('No warm session (start: python together.py --suite warm --seconds 3600)\n'+__doc__)
    info = json.loads(session.read_text(encoding='utf-8'))
    targets = ('host', 'client') if sys.argv[1] == 'both' else (sys.argv[1],)
    command = ' '.join(sys.argv[2:])
    for tag in targets:
        bot = connect(tag, info)
        start = bot.send(command, 30)
        time.sleep(1.0)  # replies follow the acknowledgement
        text = bot.log()[start:]
        print(f'--- {tag}: {command}')
        print('\n'.join(l for l in text.splitlines() if not l.startswith('COMMAND sequence=')))


if __name__ == '__main__':
    main()
