"""Verify software and timer context switching: python tests/scheduler_qemu.py --build cmake-build-tests.

Runs isolated guests because this scheduler intentionally has no stop or thread
reclamation yet. QEMU/GDB snapshots and logs remain in the build directory.
"""
import re
import socket
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from qemu_test import configure, HIDDEN


def run_case(environment, mode, ram):
    build, symbols = environment.build, environment.symbols
    with socket.socket() as endpoint:
        endpoint.bind(('127.0.0.1', 0))
        port = endpoint.getsockname()[1]
    guest = environment.guest(f'scheduler-{mode}-{ram}m', ram, port)
    try:
        guest.qmp('cont')
        time.sleep(1)
        guest.qmp('stop')
        raw = guest.read(0xb8000, 4000, 'boot-vga')[::2].decode('ascii')
        assert 'Kernel started.' in raw, raw
        commands = build / f'{guest.name}-gdb.txt'
        commands.write_text('\n'.join([
            'set pagination off', 'set confirm off', 'set architecture i386',
            f'target remote 127.0.0.1:{port}',
            # Do not abandon IRQ0 before its EOI: the PIC would suppress
            # subsequent timer delivery and make the preemption test hang.
            f'hbreak *{symbols["_interrupts_enable_and_halt"]:#x}',
            'continue', 'delete breakpoints',
            f'set $esp = {symbols["__kernel_stack_top"] - 4:#x}',
            'set {unsigned int}$esp = 0',
            f'set $eip = {symbols["_scheduler_test_" + mode]:#x}',
            'detach', 'quit',
        ]) + '\n')
        result = subprocess.run([str(environment.gdb), '-q', '-nx', '-batch', '-x', str(commands)],
                                capture_output=True, text=True, timeout=20, creationflags=HIDDEN)
        (build / f'{guest.name}-gdb.log').write_text(result.stdout + result.stderr)
        assert result.returncode == 0 and not re.search(r'^.*Error', result.stderr, re.M), result
        deadline = time.monotonic() + 20
        while True:
            time.sleep(0.2)
            guest.qmp('stop')
            stage = guest.value('_scheduler_test_stage')
            assert guest.value('_scheduler_test_error') == 0, 'guest scheduler test failed'
            if stage:
                break
            assert time.monotonic() < deadline, 'scheduler did not make progress'
            guest.qmp('cont')
        assert stage == {'cooperative': 1, 'preemptive': 2, 'failure': 3}[mode]
        if mode != 'failure':
            stack_data = guest.read(symbols['_scheduler_test_stacks'], 8, 'stacks')
            first_stack, second_stack = struct.unpack('<II', stack_data)
            assert first_stack and second_stack and abs(first_stack - second_stack) >= 4096
        if mode == 'preemptive':
            counts_before = struct.unpack('<II', guest.read(symbols['_scheduler_test_counts'], 8, 'counts-before'))
            ticks = guest.value('_timer_ticks')
            guest.qmp('cont')
            time.sleep(0.5)
            guest.qmp('stop')
            counts_after = struct.unpack('<II', guest.read(symbols['_scheduler_test_counts'], 8, 'counts-after'))
            assert all(after > before for before, after in zip(counts_before, counts_after)), (counts_before, counts_after)
            assert guest.value('_timer_ticks') > ticks
            assert guest.value('_scheduler_test_error') == 0
        registers = guest.monitor('info registers')
        assert 'CR0=80010011' in registers and 'CR4=00000000' in registers
        assert int(re.search(r'CR3=([0-9a-fA-F]+)', registers)[1], 16) == guest.value('_paging_directory_physical')
        guest.qmp('quit')
        guest.process.wait(timeout=5)
        log = (build / f'{guest.name}-qemu.log').read_text()
        assert 'Triple fault' not in log and log.count('CPU Reset') == 2
        faults = [int(v, 16) for v in re.findall(r'v=([0-9a-f]{2}) e=[0-9a-f]+', log, re.I) if int(v, 16) < 32]
        assert not faults, faults
        print(f'{guest.name}: PASS')
    finally:
        guest.close()


if __name__ == '__main__':
    env = configure(__doc__)
    assert env.gdb.exists(), 'Scheduler tests require GDB; pass --gdb PATH'
    for memory in (128, 512):
        run_case(env, 'cooperative', memory)
        run_case(env, 'preemptive', memory)
    run_case(env, 'failure', 128)
