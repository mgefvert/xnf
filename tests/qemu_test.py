"""Shared QEMU/GDB infrastructure. Importing this module has no side effects."""
import argparse
import json
import os
import pathlib
import queue
import re
import struct
import subprocess
import threading


HIDDEN = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0


class TestEnvironment:
    def __init__(self, build, cache, gdb, symbols):
        self.build, self.cache, self.gdb, self.symbols = build, cache, gdb, symbols

    def guest(self, name, ram, port=None, cpu=None):
        return Guest(self, name, ram, port, cpu)


def configure(description, require_tests=True):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('--build', type=pathlib.Path, default=pathlib.Path('cmake-build-tests'))
    parser.add_argument('--gdb', type=pathlib.Path)
    args = parser.parse_args()
    build = args.build.resolve()
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        match = re.match(r'([^:#]+):[^=]+=(.*)', line)
        if match:
            cache[match[1]] = match[2]
    suffix = '.exe' if os.name == 'nt' else ''
    nm = pathlib.Path(cache['KERNEL_C_COMPILER']).parent / ('nm' + suffix)
    gdb = args.gdb or pathlib.Path(cache['KERNEL_C_COMPILER']).parents[2] / 'gdb/win/x64/bin/gdb.exe'
    symbols = {}
    for line in subprocess.check_output([str(nm), '-n', str(build / 'kernel_linked.exe')],
                                        text=True, creationflags=HIDDEN).splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)

    if require_tests and cache.get('XNF_BUILD_TESTS') != 'ON':
        parser.error('Configure a separate build with -DXNF_BUILD_TESTS=ON first')
    return TestEnvironment(build, cache, gdb, symbols)


class Guest:
    def __init__(self, environment, name, ram, port=None, cpu=None):
        self.environment = environment
        build, cache = environment.build, environment.cache
        self.name = name
        command = [cache['QEMU_EXECUTABLE'], '-m', str(ram), '-drive',
                   f'format=raw,file={build / "xnf.img"}', '-display', 'none',
                   '-serial', 'none', '-monitor', 'none', '-qmp', 'stdio', '-S',
                   '-no-reboot', '-no-shutdown', '-d', 'int,cpu_reset',
                   '-D', str(build / f'{name}-qemu.log')]
        if cpu:
            command += ['-cpu', cpu]
        if port:
            command += ['-gdb', f'tcp:127.0.0.1:{port}']
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, creationflags=HIDDEN)
        self.messages = queue.Queue()
        def receive():
            for line in self.process.stdout:
                self.messages.put(json.loads(line))
        threading.Thread(target=receive, daemon=True).start()
        assert 'QMP' in self.messages.get(timeout=10)
        self.qmp('qmp_capabilities')

    def qmp(self, command, **arguments):
        self.process.stdin.write(json.dumps({'execute': command, 'arguments': arguments}) + '\n')
        self.process.stdin.flush()
        while True:
            message = self.messages.get(timeout=10)
            assert 'error' not in message, message
            if 'return' in message:
                return message['return']

    def monitor(self, command):
        return self.qmp('human-monitor-command', **{'command-line': command})

    def read(self, address, size, label):
        path = self.environment.build / f'{self.name}-{label}.bin'
        assert not self.monitor(f'pmemsave {address:#x} {size:#x} "{path.as_posix()}"')
        return path.read_bytes()

    def value(self, symbol):
        return struct.unpack('<I', self.read(self.environment.symbols[symbol], 4, symbol))[0]

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()

