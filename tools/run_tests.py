#!/usr/bin/env python3
"""Compiles, assembles and runs the g2 test programs.

A test is a C file that includes test.h and calls ASSERT() from start().
It passes if every assertion holds. A test whose first line is
`// expect-error: <text>` passes if compilation fails with an error
message containing <text>.
"""

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import g2run


def run_test(path, compiler, assembler, include, workdir, cflags):
    base = os.path.join(workdir, os.path.splitext(os.path.basename(path))[0])
    asm, binary = base + '.g2', base + '.g2b'

    with open(path) as f:
        first_line = f.readline().strip()
    expect_error = None
    if first_line.startswith('// expect-error:'):
        expect_error = first_line.removeprefix('// expect-error:').strip()

    r = subprocess.run([compiler, '-I', include, *cflags, '-o', asm, path],
                       capture_output=True, text=True)
    if expect_error is not None:
        if r.returncode == 0:
            return 'expected a compile error, but compilation succeeded'
        if expect_error not in r.stderr:
            return f'expected error containing "{expect_error}", got:\n{r.stderr}'
        return None
    if r.returncode != 0:
        return f'compile failed:\n{r.stderr}'

    r = subprocess.run([assembler, asm, binary], capture_output=True, text=True)
    if r.returncode != 0 or 'ERROR' in r.stdout:
        return f'assembly failed:\n{r.stdout}{r.stderr}'

    try:
        vm, _ = g2run.run(binary, frames=1)
    except g2run.VMError as e:
        return f'runtime error: {e}'

    sym = g2run.read_symbols(asm)
    mem = lambda name: vm.memory[sym['g_' + name]]
    if mem('__test_failed_line'):
        return (f'{path}:{mem("__test_failed_line")}: expected {mem("__test_expected")},'
                f' got {mem("__test_actual")}')
    if mem('__test_count') == 0:
        return 'no assertions ran'
    return None


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(description='Run the g2 test suite.')
    parser.add_argument('tests', nargs='*', help='test files (default: test-g2/*.c)')
    parser.add_argument('--compiler', default=os.path.join(root, 'chibicc-g2'))
    parser.add_argument('--assembler', default='g2a')
    parser.add_argument('--cflags', default='',
                        help='extra compiler flags, e.g. "-fno-overlay -fno-peephole"')
    args = parser.parse_args()

    tests = args.tests
    if not tests:
        test_dir = os.path.join(root, 'test-g2')
        tests = sorted(os.path.join(dp, f) for dp, _, fs in os.walk(test_dir)
                       for f in fs if f.endswith('.c'))

    include = os.path.join(root, 'test-g2')
    failures = 0
    with tempfile.TemporaryDirectory() as workdir:
        for path in tests:
            error = run_test(path, args.compiler, args.assembler, include, workdir,
                             args.cflags.split())
            name = os.path.relpath(path, root)
            if error:
                failures += 1
                print(f'FAIL {name}\n  ' + error.replace('\n', '\n  '))
            else:
                print(f'ok   {name}')

    print(f'\n{len(tests) - failures} passed, {failures} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
