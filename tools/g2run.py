#!/usr/bin/env python3
"""Headless g2 VM for testing compiled programs.

Runs a .g2b program (as produced by the g2 assembler) for a number of
frames without opening a window, then prints memory values and the pixels
drawn in the last frame. Follows the semantics of g2-spec.md and the
reference VM (cg2).

Example:
    g2run.py prog.g2b --frames 10 --press RIGHT:0-4 \\
        --symbols prog.g2asm --print g_x --print g_arr:4 --pixels
"""

import argparse
import re
import struct
import sys

CALL_STACK_ADDRESS = 16
CALL_STACK_SIZE = 16
KEYS = {
    'CONTROL1': 3, 'CONTROL2': 4, 'A': 5, 'B': 6,
    'UP': 7, 'DOWN': 8, 'LEFT': 9, 'RIGHT': 10,
}
OPCODES = ['ldi', 'rdi', 'sti', 'add', 'mul', 'div', 'mod', 'cmp', 'jne', 'col', 'pix']


class VMError(Exception):
    pass


def wrap32(v):
    return (v + 0x80000000) % 0x100000000 - 0x80000000


class Program:
    def __init__(self, path):
        with open(path, 'rb') as f:
            data = f.read()
        if data[:2] != b'g2':
            raise VMError('invalid signature')
        self.memory_size, self.width, self.height, count = struct.unpack_from('<IIII', data, 2)
        off = 18
        self.instructions = []
        for _ in range(count):
            self.instructions.append(struct.unpack_from('<Biii', data, off))
            off += 13
        self.data_address, data_len = struct.unpack_from('<II', data, off)
        off += 8
        self.data = list(struct.unpack_from(f'<{data_len}i', data, off))

        if self.width == 0 or self.height == 0:
            raise VMError('invalid window size')
        if self.memory_size < 32:
            raise VMError('not enough memory')


class VM:
    def __init__(self, program, max_steps):
        self.program = program
        self.max_steps = max_steps
        size = program.memory_size + len(program.data)
        self.memory = [0] * size
        self.memory[15] = CALL_STACK_ADDRESS
        if program.data_address > 0:
            self.memory[program.data_address:program.data_address + len(program.data)] = program.data
        self.color = (0, 0, 0)
        self.frame_count = 0
        self.steps = 0

    def get(self, addr):
        if addr < 0 or addr >= len(self.memory):
            raise VMError(f'out of bounds read at address {addr}')
        return self.memory[addr]

    def set(self, addr, value):
        if addr < 0 or addr >= len(self.memory):
            raise VMError(f'out of bounds write at address {addr}')
        self.memory[addr] = wrap32(value)

    def run_frame(self, keys, delta_ms):
        reserved = [0, 1, -1] + [1 if k in keys else 0 for k in KEYS] + [delta_ms]
        self.memory[0:12] = reserved

        pixels = []
        ins = self.program.instructions
        count = len(ins)
        pc = 0
        steps = 0
        while pc < count:
            steps += 1
            if steps > self.max_steps:
                raise VMError(f'frame {self.frame_count} exceeded {self.max_steps} instructions')
            op, a0, a1, a2 = ins[pc]
            pc += 1
            if op == 0:    # ldi
                self.set(a0, a1)
            elif op == 1:  # rdi
                self.set(a0, self.get(self.get(a1)))
            elif op == 2:  # sti
                self.set(self.get(a0), self.get(a1))
            elif op == 3:  # add
                self.set(a0, self.get(a1) + self.get(a2))
            elif op == 4:  # mul
                self.set(a0, self.get(a1) * self.get(a2))
            elif op in (5, 6):  # div, mod (Python's // and % floor like g2)
                b = self.get(a2)
                if b == 0:
                    raise VMError(f'division by zero at instruction {pc - 1}')
                a = self.get(a1)
                self.set(a0, a // b if op == 5 else a % b)
            elif op == 7:  # cmp
                a, b = self.get(a1), self.get(a2)
                self.set(a0, (a < b) - (a > b))
            elif op == 8:  # jne
                if self.get(a1) != self.get(a2):
                    target = self.get(a0)
                    if target < 0 or target > count:
                        raise VMError(f'jump out of bounds to {target} at instruction {pc - 1}')
                    pc = target
            elif op == 9:  # col
                self.color = tuple(max(0, min(255, self.get(a))) for a in (a0, a1, a2))
            elif op == 10:  # pix
                x, y = self.get(a0), self.get(a1)
                if 0 <= x < self.program.width and 0 <= y < self.program.height:
                    pixels.append((x, y, self.color))
            else:
                raise VMError(f'unrecognized opcode {op} at instruction {pc - 1}')

            if self.memory[15] > CALL_STACK_ADDRESS + CALL_STACK_SIZE:
                raise VMError(f'call stack overflow at instruction {pc - 1}')

        self.frame_count += 1
        self.steps += steps
        return pixels


def read_symbols(path):
    symbols = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'\s*define\s+(\w+)\s+(-?\d+)', line)
            if m:
                symbols[m.group(1)] = int(m.group(2))
    return symbols


def parse_presses(specs):
    """KEY:FROM-TO or KEY:N (frame numbers start at 0)."""
    presses = []
    for spec in specs:
        key, _, frames = spec.partition(':')
        key = key.upper()
        if key not in KEYS:
            raise SystemExit(f'unknown key: {key}')
        lo, _, hi = frames.partition('-')
        presses.append((key, int(lo), int(hi or lo)))
    return presses


def run(path, frames=1, presses=(), delta_ms=16, max_steps=10_000_000):
    vm = VM(Program(path), max_steps)
    pixels = []
    for i in range(frames):
        keys = {k for k, lo, hi in presses if lo <= i <= hi}
        pixels = vm.run_frame(keys, delta_ms)
    return vm, pixels


def main():
    parser = argparse.ArgumentParser(description='Run a g2 program headlessly.')
    parser.add_argument('program', help='.g2b file')
    parser.add_argument('--frames', type=int, default=1)
    parser.add_argument('--press', action='append', default=[], metavar='KEY:FROM-TO',
                        help='hold a key during the given frames, e.g. RIGHT:0-9')
    parser.add_argument('--delta', type=int, default=16, help='delta time in ms')
    parser.add_argument('--max-steps', type=int, default=10_000_000,
                        help='maximum instructions per frame')
    parser.add_argument('--symbols', help='.g2asm file to read define names from')
    parser.add_argument('--print', action='append', default=[], metavar='NAME[:COUNT]',
                        help='print a memory value by define name or address')
    parser.add_argument('--pixels', action='store_true', help='print pixels drawn in the last frame')
    args = parser.parse_args()

    symbols = read_symbols(args.symbols) if args.symbols else {}
    try:
        vm, pixels = run(args.program, args.frames, parse_presses(args.press),
                         args.delta, args.max_steps)
    except VMError as e:
        print(f'RUNTIME ERROR: {e}', file=sys.stderr)
        return 1

    for spec in args.print:
        name, _, count = spec.partition(':')
        addr = symbols[name] if name in symbols else int(name)
        values = vm.memory[addr:addr + int(count or 1)]
        print(f'{name} = {values[0] if not count else values}')
    if args.pixels:
        for x, y, c in pixels:
            print(f'pixel {x} {y} {c[0]} {c[1]} {c[2]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
