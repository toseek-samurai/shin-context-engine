"""Run isolated refresh scenarios within one shared CTest time budget."""
from concurrent.futures import ThreadPoolExecutor
import subprocess
import sys
import time


def main():
    executable, profiles = sys.argv[1:]
    deadline = time.monotonic() + 40

    def run(group):
        try:
            result = subprocess.run(
                [executable, profiles, '--group', group],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                encoding='utf-8', errors='replace',
                timeout=max(0.001, deadline - time.monotonic()),
            )
            return group, result.returncode == 0, result.stdout
        except subprocess.TimeoutExpired as error:
            output = (error.output or b'').decode('utf-8', errors='replace')
            return group, False, output + '\nShared refresh test deadline exceeded.\n'

    # Each executable owns its Core stdout capture and disposable fixtures.
    # The three groups together retain the original suite's 45-second timeout.
    with ThreadPoolExecutor(max_workers=3) as pool:
        results = list(pool.map(run, ('documentation', 'races', 'safety')))
    for group, passed, output in results:
        print(f'{group}: {"passed" if passed else "FAILED"}')
        if not passed:
            print(output)
    return 0 if all(passed for _, passed, _ in results) else 1


if __name__ == '__main__':
    sys.exit(main())
