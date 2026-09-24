#!/usr/bin/python3

"""The first finished card must appear while another provider still runs."""

import os
import pty
import select
import subprocess
import sys
import tempfile
import time
from pathlib import Path


FAKE_CODEXBAR = """#!/usr/bin/python3
import json
import os
import sys
import time
from pathlib import Path

if sys.argv[1:3] == ['config', 'providers']:
    print(json.dumps([
        {'provider': 'codex', 'displayName': 'Codex', 'enabled': True},
        {'provider': 'claude', 'displayName': 'Claude', 'enabled': True},
    ]))
elif '--provider' in sys.argv:
    provider = sys.argv[sys.argv.index('--provider') + 1]
    if provider == 'claude':
        while not Path(os.environ['RELEASE_FILE']).exists():
            time.sleep(0.01)
        print('╭──────────╮\\n│ SLOW CARD │\\n╰──────────╯')
    else:
        print('╭──────────╮\\n│ FAST CARD │\\n╰──────────╯')
else:
    sys.exit(2)
"""


def main():
    with tempfile.TemporaryDirectory(prefix="slstatus-stream-") as directory:
        root = Path(directory)
        binary = root / "codexbar"
        binary.write_text(FAKE_CODEXBAR)
        binary.chmod(0o755)
        release = root / "release"
        env = os.environ.copy()
        env["PATH"] = directory + os.pathsep + env.get("PATH", "")
        env["RELEASE_FILE"] = str(release)
        master, slave = pty.openpty()
        process = subprocess.Popen(
            [sys.executable, "scripts/ai-usage-report"],
            stdout=slave,
            stderr=subprocess.PIPE,
            env=env,
        )
        os.close(slave)
        output = b""
        try:
            deadline = time.monotonic() + 5
            while b"FAST CARD" not in output and time.monotonic() < deadline:
                ready, _, _ = select.select([master], [], [], 0.1)
                if ready:
                    output += os.read(master, 65536)
            assert b"FAST CARD" in output, "first card did not stream"
            assert b"SLOW CARD" not in output, "report waited for every provider"
            release.touch()
            assert process.wait(timeout=5) == 0, "report failed"
            while True:
                ready, _, _ = select.select([master], [], [], 0.1)
                if not ready:
                    break
                try:
                    output += os.read(master, 65536)
                except OSError:
                    break
            assert b"SLOW CARD" in output, "second card did not appear"
        finally:
            release.touch()
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("ai usage streaming tests passed")


if __name__ == "__main__":
    main()
