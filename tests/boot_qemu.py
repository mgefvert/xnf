"""Validate ordinary boots and pre-.bss safety: python tests/boot_qemu.py --build cmake-build-debug.

GDB is required for early E820 fixtures and legacy-response normalization.
Run against both ordinary and XNF_BUILD_TESTS builds. Artifacts stay in build.
"""
import re
import socket
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from qemu_test import configure, HIDDEN


def run_case(name, ram=128, cpu=None, fixture=None, reject=False, legacy=False):
    port = None
    if fixture is not None or legacy:
        with socket.socket() as endpoint:
            endpoint.bind(('127.0.0.1', 0))
            port = endpoint.getsockname()[1]
    guest = environment.guest(name, ram, port, cpu)
    try:
        if port:
            lines = ['set pagination off', 'set confirm off', 'set architecture i386',
                     f'target remote 127.0.0.1:{port}']
            if legacy:
                # Stop after a valid BIOS reply but before its size branch.
                # Force a legacy response so the real assembly normalization
                # runs, rather than merely injecting already-normalized data.
                lines += [f'hbreak *{symbols["memory_detect.response_size"]:#x}',
                          'continue', 'delete breakpoints', 'set $ecx = 20',
                          f'set {{unsigned int}}{symbols["_e820_map"] + 20:#x} = 0']
            else:
                data = build / f'{name}-e820.bin'
                data.write_bytes(b''.join(struct.pack('<QQII', *entry) for entry in fixture))
                lines += [f'hbreak *{symbols["_boot_memory_check"]:#x}',
                          'continue', 'delete breakpoints',
                          f'restore {data.name} binary {symbols["_e820_map"]:#x}',
                          f'set {{unsigned short}}{symbols["_e820_entry_count"]:#x} = {len(fixture)}']
                if reject:
                    # These entire regions must remain untouched on rejection,
                    # including the stack which the old path used prematurely.
                    bss_marker = b'\xa5' * (symbols['__bss_end'] - symbols['__bss_start'])
                    stack_marker = b'\x5a' * (symbols['__kernel_stack_top'] - symbols['__kernel_stack_bottom'])
                    for label, address, data in [('bss', symbols['__bss_start'], bss_marker),
                                                  ('stack', symbols['__kernel_stack_bottom'], stack_marker)]:
                        path = build / f'{name}-{label}-marker.bin'
                        path.write_bytes(data)
                        lines.append(f'restore {path.name} binary {address:#x}')
            lines += ['detach', 'quit']
            commands = build / f'{name}-gdb.txt'
            commands.write_text('\n'.join(lines) + '\n')
            result = subprocess.run([str(environment.gdb), '-q', '-nx', '-batch', '-x', str(commands)],
                                    cwd=build, capture_output=True, text=True, timeout=20,
                                    creationflags=HIDDEN)
            (build / f'{name}-gdb.log').write_text(result.stdout + result.stderr)
            assert result.returncode == 0, result
        else:
            guest.qmp('cont')
        time.sleep(1)
        guest.qmp('stop')
        raw = guest.read(0xb8000, 4000, 'vga')[::2].decode('ascii')
        screen = '\n'.join(raw[i:i + 80].rstrip() for i in range(0, 2000, 80))
        (build / f'{name}-screen.txt').write_text(screen)
        registers = guest.monitor('info registers')
        (build / f'{name}-registers.txt').write_text(registers)
        if reject or cpu:
            expected = 'Boot memory panic:' if reject else 'Unsupported CPU: i686 required'
            assert expected in screen and 'Kernel started.' not in screen, screen
            assert 'HLT=1' in registers
            assert int(re.search(r'EFL=([0-9a-fA-F]+)', registers)[1], 16) & 0x200 == 0
            if reject:
                assert guest.read(symbols['__bss_start'], len(bss_marker), 'bss-after') == bss_marker
                assert guest.read(symbols['__kernel_stack_bottom'], len(stack_marker), 'stack-after') == stack_marker
            before = registers
            guest.qmp('cont')
            time.sleep(0.15)
            guest.qmp('stop')
            assert guest.monitor('info registers') == before, 'rejected boot did not remain halted'
        else:
            assert 'VMM ready:' in screen and 'Kernel started.' in screen, screen
            assert not any(word in screen for word in ('panic', 'failed', 'Exception')), screen
            assert 'CR0=80010011' in registers and 'CR4=00000000' in registers
            if legacy:
                assert guest.value('_e820_map') == 0  # First BIOS range starts at physical zero.
                attributes = struct.unpack('<I', guest.read(symbols['_e820_map'] + 20, 4, 'legacy-attributes'))[0]
                assert attributes == 1, 'legacy entry was not normalized to enabled'
            ticks = guest.value('_timer_ticks')
            guest.qmp('cont')
            time.sleep(0.15)
            guest.qmp('stop')
            assert guest.value('_timer_ticks') > ticks
        guest.qmp('quit')
        guest.process.wait(timeout=5)
        log = (build / f'{name}-qemu.log').read_text()
        assert not re.search(r'check_exception|Triple fault|v=(?:0d|0e|08)', log, re.I)
        assert log.count('CPU Reset') == 2
        print(f'{name}: PASS' + ('; .bss and final stack unchanged' if reject else ''))
    finally:
        guest.close()


if __name__ == '__main__':
    environment = configure(__doc__, require_tests=False)
    build, symbols = environment.build, environment.symbols
    assert environment.gdb.exists(), 'Pass --gdb PATH for bootstrap safety tests'
    run_case('boot-128m')
    run_case('boot-512m', ram=512)
    run_case('boot-486-rejected', cpu='486')
    run_case('boot-pentium-rejected', cpu='pentium')
    run_case('boot-legacy-e820', legacy=True)
    usable = [(0, 0x9fc00, 1, 1), (0x100000, 0x7f00000, 1, 1)]
    run_case('boot-disabled-reservation', fixture=usable + [(symbols['__bss_start'], 4096, 2, 0)])
    run_case('boot-bss-reserved', fixture=usable + [(symbols['__bss_start'], 4096, 2, 1)], reject=True)
    run_case('boot-stack-reserved', fixture=usable + [(symbols['__kernel_stack_bottom'], 4096, 2, 1)], reject=True)
    run_case('boot-gap', fixture=[(0, symbols['__bss_start'], 1, 1),
                                (symbols['__bss_start'] + 4096, 0x9fc00 - symbols['__bss_start'] - 4096, 1, 1)],
             reject=True)
    run_case('boot-disabled-ram', fixture=[(0, 0x9fc00, 1, 0)], reject=True)
    run_case('boot-overflow', fixture=usable + [(0xfffffffffffffff0, 4096, 1, 1)], reject=True)
