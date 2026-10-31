"""Check the A/B demonstration: python tests/ab_qemu.py --build cmake-build-debug.

Test builds enter the same demonstration through GDB after their ordinary
fixture boot. Snapshots and logs stay in the selected build directory.
"""
import re
import socket
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from qemu_test import configure, HIDDEN


def run_case(environment, ram):
    build, symbols = environment.build, environment.symbols
    test_build = environment.cache.get('XNF_BUILD_TESTS') == 'ON'
    port = None
    if test_build:
        with socket.socket() as endpoint:
            endpoint.bind(('127.0.0.1', 0))
            port = endpoint.getsockname()[1]
    guest = environment.guest(f'ab-{ram}m', ram, port)
    try:
        guest.qmp('cont')
        time.sleep(1)
        guest.qmp('stop')
        screen = guest.read(0xb8000, 4000, 'boot-screen')[::2].decode('ascii')
        assert 'Kernel started.' in screen, screen
        if test_build:
            # Wait for the normal idle entry so no PIC IRQ is left in service
            # when the disposable guest abandons its original call stack.
            commands = build / f'{guest.name}-gdb.txt'
            commands.write_text('\n'.join([
                'set pagination off', 'set confirm off', 'set architecture i386',
                f'target remote 127.0.0.1:{port}',
                f'hbreak *{symbols["_interrupts_enable_and_halt"]:#x}',
                'continue', 'delete breakpoints',
                f'set $esp = {symbols["__kernel_stack_top"] - 4:#x}',
                'set {unsigned int}$esp = 0',
                f'set $eip = {symbols["_kernel_demo_start"]:#x}', 'detach', 'quit',
            ]) + '\n')
            result = subprocess.run([str(environment.gdb), '-q', '-nx', '-batch', '-x', str(commands)],
                                    capture_output=True, text=True, timeout=20, creationflags=HIDDEN)
            (build / f'{guest.name}-gdb.log').write_text(result.stdout + result.stderr)
            assert result.returncode == 0 and not re.search(r'^.*Error', result.stderr, re.M), result
            time.sleep(0.2)
            guest.qmp('stop')

        def snapshot(label):
            raw = guest.read(0xb8000, 4000, label)[::2].decode('ascii')
            assert 'Kernel started.' in raw, raw
            text = raw.split('Kernel started.', 1)[1]
            assert not any(word in raw for word in ('panic', 'Exception', 'failed')), raw
            assert guest.read(symbols['_bootstrap_finished'], 1, f'{label}-bootstrap') == b'\x01'
            return guest.value('_timer_ticks'), text.count('A'), text.count('B')

        ticks, before_a, before_b = snapshot('initial')
        saw_idle = guest.value('_current') == 3
        deadline = time.monotonic() + 15
        while True:
            guest.qmp('cont')
            time.sleep(0.2)
            guest.qmp('stop')
            now = guest.value('_timer_ticks')
            current = guest.value('_current')
            assert current != 0, 'retired bootstrap context was scheduled'
            saw_idle = saw_idle or current == 3
            if ((now - ticks) & 0xffffffff) >= 220:
                break
            assert time.monotonic() < deadline, 'timer did not progress'
        after_ticks, after_a, after_b = snapshot('final')
        assert 2 <= after_a - before_a <= 4 and 2 <= after_b - before_b <= 4, (
            ticks, after_ticks, before_a, after_a, before_b, after_b)
        assert abs((after_a - before_a) - (after_b - before_b)) <= 1
        assert saw_idle, 'idle context did not run while both threads slept'
        registers = guest.monitor('info registers')
        assert 'CR0=80010011' in registers and 'CR4=00000000' in registers
        assert int(re.search(r'CR3=([0-9a-fA-F]+)', registers)[1], 16) == guest.value('_paging_directory_physical')
        guest.qmp('quit')
        guest.process.wait(timeout=5)
        log = (build / f'{guest.name}-qemu.log').read_text()
        assert 'Triple fault' not in log and log.count('CPU Reset') == 2
        faults = [int(v, 16) for v in re.findall(r'v=([0-9a-f]{2}) e=[0-9a-f]+', log, re.I) if int(v, 16) < 32]
        assert not faults, faults
        print(f'ab-{ram}m: PASS; A and B continue, bootstrap retired, idle runs')
    finally:
        guest.close()


if __name__ == '__main__':
    env = configure(__doc__, require_tests=False)
    if env.cache.get('XNF_BUILD_TESTS') == 'ON':
        assert env.gdb.exists(), 'Test-build A/B checks require GDB; pass --gdb PATH'
    for memory in (128, 512):
        run_case(env, memory)
