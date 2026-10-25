"""Build first, then: python tests/vmm_qemu.py --build cmake-build-tests.

Requires GDB (same discovery/--gdb as pmm_qemu.py). Uses real 128/512 MiB
guests for write tests, and separate guests for four expected page faults.
Artifacts stay in the build directory. Never injects fictitious writable RAM.
"""
import re
import socket
import struct
import subprocess
import sys
import time

# Keep import artifacts out of the source tree, like all guest snapshots/logs.
sys.dont_write_bytecode = True
from qemu_test import configure, HIDDEN as hidden


def screen(guest, label):
    raw = guest.read(0xb8000, 4000, label)[::2].decode('ascii')
    result = '\n'.join(raw[i:i + 80].rstrip() for i in range(0, 2000, 80))
    (build / f'{guest.name}-{label}.txt').write_text(result)
    return result


def redirect(guest, port, function):
    commands = build / f'{guest.name}-gdb.txt'
    commands.write_text('\n'.join([
        'set pagination off', 'set confirm off', 'set architecture i386',
        f'target remote 127.0.0.1:{port}',
        f'set $esp = {symbols["__kernel_stack_top"] - 4:#x}',
        'set {unsigned int}$esp = 0',
        f'set $eip = {symbols[function]:#x}',
        'detach', 'quit',
    ]) + '\n')
    result = subprocess.run([str(gdb), '-q', '-nx', '-batch', '-x', str(commands)],
                            capture_output=True, text=True, timeout=20, creationflags=hidden)
    (build / f'{guest.name}-gdb.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0 and not re.search(r'^.*Error', result.stderr, re.M), result


def check_tables(guest):
    directory_address = guest.value('_paging_directory_physical')
    identity_end = guest.value('_paging_identity_end')
    directory = struct.unpack('<1024I', guest.read(directory_address, 4096, 'pd'))
    assert directory[64:] == (0,) * 960, 'future user range has present directories'
    for index, pde in enumerate(directory[:64]):
        assert pde & 0xfff in (3, 0x23), f'bad PDE flags {pde:#x}'
        table = pde & ~4095
        assert symbols['__kernel_start'] <= table < symbols['__kernel_end']
        entries = struct.unpack('<1024I', guest.read(table, 4096, f'pt-{index}'))
        for offset, entry in enumerate(entries):
            virtual = (index * 1024 + offset) * 4096
            if virtual == 0 or virtual >= identity_end:
                assert entry == 0, f'leaked mapping {virtual:#x}: {entry:#x}'
            else:
                assert entry & ~0x60 == virtual | 3, f'changed identity mapping {virtual:#x}'


def run_case(ram, function='_vmm_test_entry', fault=None, high_fixture=False):
    name = f'vmm-{ram}m' if fault is None else f'vmm-fault-{fault}'
    if high_fixture:
        name = 'vmm-high-fragmented'
    with socket.socket() as endpoint:
        endpoint.bind(('127.0.0.1', 0))
        port = endpoint.getsockname()[1]
    guest = Guest(name, ram, port)
    try:
        if high_fixture:
            entries = [(symbols['__kernel_start'], symbols['__kernel_end'] - symbols['__kernel_start'], 1, 1),
                       (0x10000000, 0x1000000, 1, 1), (0x10001000, 4096, 2, 1)]
            data = build / f'{name}-e820.bin'
            data.write_bytes(b''.join(struct.pack('<QQII', *entry) for entry in entries))
            commands = build / f'{name}-fixture-gdb.txt'
            commands.write_text('\n'.join([
                'set pagination off', 'set confirm off', 'set architecture i386',
                f'target remote 127.0.0.1:{port}',
                f'hbreak *{symbols["_boot_memory_check"]:#x}', 'continue', 'delete breakpoints',
                f'restore {data.name} binary {symbols["_e820_map"]:#x}',
                f'set {{unsigned short}}{symbols["_e820_entry_count"]:#x} = {len(entries)}',
                'detach', 'quit',
            ]) + '\n')
            result = subprocess.run([str(gdb), '-q', '-nx', '-batch', '-x', str(commands)],
                                    cwd=build, capture_output=True, text=True, timeout=20,
                                    creationflags=hidden)
            (build / f'{name}-fixture-gdb.log').write_text(result.stdout + result.stderr)
            assert result.returncode == 0, result
        else:
            guest.qmp('cont')
        time.sleep(1)
        guest.qmp('stop')
        initial_screen = screen(guest, 'boot-screen')
        assert 'VMM ready:' in initial_screen and 'Kernel started.' in initial_screen, initial_screen
        baseline_bitmap = guest.read(symbols['_bitmap'], 128 * 1024, 'initial-bitmap')
        baseline_metadata = [guest.read(symbols[s], 8192, f'initial-{s}')
                             for s in ('_occupied', '_heads', '_tails')]
        physical_before = guest.value('_free_pages')
        virtual_before = guest.value('_free_virtual_pages')
        check_tables(guest)
        if high_fixture:
            # The dedicated C entry enables fixture assertions after .bss
            # initialization and before running the general allocator tests.
            function = '_vmm_test_high_entry'
        redirect(guest, port, function)
        deadline = time.monotonic() + 120
        while True:
            time.sleep(1)
            guest.qmp('stop')
            current_screen = screen(guest, 'screen')
            if fault is not None and 'Exception 14: Page Fault' in current_screen:
                break
            if fault is None and guest.value('_vmm_test_stage') == 5:
                break
            assert not any(x in current_screen for x in ('panic', 'failed', 'Exception')), current_screen
            assert time.monotonic() < deadline, current_screen
            guest.qmp('cont')

        registers = guest.monitor('info registers')
        (build / f'{name}-registers.txt').write_text(registers)
        assert 'CR0=80010011' in registers and 'CR4=00000000' in registers
        ticks = guest.value('_timer_ticks')
        if fault is None:
            assert 'VMM tests passed:' in current_screen, current_screen
            assert guest.read(symbols['_bitmap'], 128 * 1024, 'final-bitmap') == baseline_bitmap
            assert guest.value('_free_pages') == physical_before
            assert guest.value('_free_virtual_pages') == virtual_before
            for s, expected in zip(('_occupied', '_heads', '_tails'), baseline_metadata):
                assert guest.read(symbols[s], 8192, f'final-{s}') == expected, s
            check_tables(guest)
            if high_fixture:
                assert guest.value('_vmm_test_high_physical') >= 0x10000000
            elif ram == 512:
                assert guest.value('_vmm_test_ticks_during_allocation') > 0, 'IRQs blocked during zeroing'
            guest.qmp('cont')
            time.sleep(1.1)
            guest.qmp('stop')
            assert guest.value('_timer_ticks') > ticks, 'timer did not progress'
        else:
            expected_address = guest.value('_vmm_test_fault_address')
            actual_address = int(re.search(r'CR2=([0-9a-fA-F]+)', registers)[1], 16)
            assert actual_address == expected_address
            assert f'Faulting linear address (CR2): 0x{expected_address:08X}' in current_screen
            assert 'write, supervisor' in current_screen
            assert ('protection violation' if fault == 'readonly' else 'non-present page') in current_screen
            assert 'HLT=1' in registers
            assert int(re.search(r'EFL=([0-9a-fA-F]+)', registers)[1], 16) & 0x200 == 0
            guest.qmp('cont')
            time.sleep(0.2)
            guest.qmp('stop')
            assert guest.value('_timer_ticks') == ticks

        guest.qmp('quit')
        guest.process.wait(timeout=5)
        log = (build / f'{name}-qemu.log').read_text()
        assert 'Triple fault' not in log and log.count('CPU Reset') == 2
        exceptions = re.findall(r'v=([0-9a-f]{2}) e=([0-9a-f]+)', log, re.I)
        faults = [(int(vector, 16), int(error, 16)) for vector, error in exceptions
                  if int(vector, 16) < 32]
        assert faults == ([(14, 3 if fault == 'readonly' else 2)] if fault else []), faults
        print(f'{name}: PASS' + ('; expected page fault and halt' if fault else
              '; bitmap/metadata restored, tables verified, timer continues'))
    finally:
        guest.close()


if __name__ == '__main__':
    environment = configure(__doc__)
    build, gdb, symbols = environment.build, environment.gdb, environment.symbols
    Guest = environment.guest
    assert gdb.exists(), 'VMM tests require GDB; pass --gdb PATH'
    run_case(128)
    run_case(512)
    run_case(512, high_fixture=True)
    for probe in ('null', 'freed', 'readonly', 'boundary'):
        run_case(128, f'_vmm_fault_{probe}', probe)
