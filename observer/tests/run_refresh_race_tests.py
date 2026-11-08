"""Run isolated refresh scenarios within one shared CTest time budget."""
from concurrent.futures import ThreadPoolExecutor
import subprocess
import sys
import time


def run_groups(executable, profiles):
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
            output = error.output or ''
            if isinstance(output, bytes):
                output = output.decode('utf-8', errors='replace')
            return group, False, output + '\nShared refresh test deadline exceeded.\n'

    # Each executable owns its Core stdout capture and disposable fixtures.
    # Split the two independent race families so their real coalescing waits do
    # not serialize on one worker after the other workers have become idle.
    # Keep three workers, one shared 40s deadline and the outer 45s CTest limit.
    with ThreadPoolExecutor(max_workers=3) as pool:
        return list(pool.map(run, ('source-races', 'observations', 'documentation', 'safety')))


def main():
    executable, profiles = sys.argv[1:]
    results = run_groups(executable, profiles)
    for group, passed, output in results:
        print(f'{group}: {"passed" if passed else "FAILED"}')
        if not passed:
            print(output)
    return 0 if all(passed for _, passed, _ in results) else 1


if __name__ == '__main__':
    sys.exit(main())
