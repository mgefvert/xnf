"""Verify the VMM-backed heap: python tests/kheap_qemu.py --build cmake-build-tests.

Requires GDB. Positive guests exercise public heap contracts under real memory
pressure; isolated guests check invalid frees, initialization, and revoked pages.
Full ownership snapshots, logs, and GDB commands stay in the build directory.
"""
import re
import socket
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from qemu_test import configure, HIDDEN


def screen(guest, label):
    raw = guest.read(0xb8000, 4000, label)[::2].decode('ascii')
    value = '\n'.join(raw[i:i + 80].rstrip() for i in range(0, 2000, 80))
    (build / f'{guest.name}-{label}.txt').write_text(value)
    return value


def gdb_run(guest, port, commands, label):
    path = build / f'{guest.name}-{label}-gdb.txt'
    path.write_text('\n'.join([
        'set pagination off', 'set confirm off', 'set architecture i386',
        f'target remote 127.0.0.1:{port}', *commands, 'detach', 'quit',
    ]) + '\n')
    result = subprocess.run([str(environment.gdb), '-q', '-nx', '-batch', '-x', str(path)],
                            cwd=build, capture_output=True, text=True, timeout=30,
                            creationflags=HIDDEN)
    (build / f'{guest.name}-{label}-gdb.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0 and not re.search(r'^.*Error', result.stderr, re.M), result


def ownership(guest, label):
    return {name: guest.read(symbols[name], size, f'{label}-{name}') for name, size in
            [('_bitmap', 128 * 1024), ('_occupied', 8192), ('_heads', 8192), ('_tails', 8192)]}


def paging_snapshot(guest, label):
    # Compare real page-table bytes, masking only CPU-managed accessed/dirty
    # bits. This catches leaked mappings and damaged bootstrap permissions.
    directory = struct.unpack('<1024I', guest.read(guest.value('_paging_directory_physical'), 4096,
                                                  f'{label}-pd'))
    assert directory[64:] == (0,) * 960
    identity_end = guest.value('_paging_identity_end')
    result = [tuple(entry & ~0x60 for entry in directory)]
    for index, entry in enumerate(directory[:64]):
        assert entry & 0xfff in (3, 0x23)
        table = struct.unpack('<1024I', guest.read(entry & ~4095, 4096, f'{label}-pt-{index}'))
        table = tuple(pte & ~0x60 for pte in table)
        for offset, pte in enumerate(table):
            virtual = (index * 1024 + offset) * 4096
            assert pte == (virtual | 3 if 0 < virtual < identity_end else 0), (virtual, pte)
        result.append(table)
    return result


def run_case(name, ram=128, entry='_kheap_test_entry', high=False, hook=None, expected=None):
    with socket.socket() as endpoint:
        endpoint.bind(('127.0.0.1', 0))
        port = endpoint.getsockname()[1]
    guest = environment.guest(name, ram, port)
    try:
        if high:
            # All loans come from real high guest RAM. Many reserved odd pages
            # guarantee physical gaps even inside the large payload's extent.
            entries = [(symbols['__kernel_start'], symbols['__kernel_end'] - symbols['__kernel_start'], 1, 1),
                       (0x10000000, 0x1000000, 1, 1)]
            entries += [(0x10000000 + (2 * i + 1) * 4096, 4096, 2, 1) for i in range(15)]
            data = build / f'{name}-e820.bin'
            data.write_bytes(b''.join(struct.pack('<QQII', *item) for item in entries))
            gdb_run(guest, port, [f'hbreak *{symbols["_boot_memory_check"]:#x}',
                                 'continue', 'delete breakpoints',
                                 f'restore {data.name} binary {symbols["_e820_map"]:#x}',
                                 f'set {{unsigned short}}{symbols["_e820_entry_count"]:#x} = {len(entries)}'],
                    'fixture')
        elif hook is None:
            guest.qmp('cont')

        baseline = None
        tables = None
        physical = None
        virtual = None
        if hook is None:
            time.sleep(1)
            guest.qmp('stop')
            initial = screen(guest, 'boot-screen')
            assert 'Heap ready.' in initial and 'Kernel started.' in initial, initial
            assert guest.value('_heap_pages') == guest.value('_heap_allocations') == 0
            if expected is None:
                baseline = ownership(guest, 'before')
                tables = paging_snapshot(guest, 'before')
                physical = guest.value('_free_pages')
                virtual = guest.value('_free_virtual_pages')

        commands = []
        if hook is not None:
            commands += [f'hbreak *{symbols[hook]:#x}', 'continue', 'delete breakpoints']
        if entry == '_kheap_test_preinit_entry':
            # Capture the actual state at the initialization boundary while
            # GDB has the CPU stopped, before the preinit probe begins.
            baseline = {}
            for symbol, size in [('_bitmap', 128 * 1024), ('_occupied', 8192),
                                 ('_heads', 8192), ('_tails', 8192)]:
                path = build / f'{name}-preinit-{symbol}.bin'
                commands.append(f'dump binary memory {path.name} {symbols[symbol]:#x} {symbols[symbol] + size:#x}')
                baseline[symbol] = path
        commands += [f'set $esp = {symbols["__kernel_stack_top"] - 4:#x}',
                     'set {unsigned int}$esp = 0', f'set $eip = {symbols[entry]:#x}']
        gdb_run(guest, port, commands, 'entry')
        if entry == '_kheap_test_preinit_entry':
            baseline = {symbol: path.read_bytes() for symbol, path in baseline.items()}

        deadline = time.monotonic() + 120
        while True:
            time.sleep(0.5)
            guest.qmp('stop')
            current = screen(guest, 'screen')
            if expected is None and guest.value('_kheap_test_stage') == 5:
                break
            if expected is not None and expected in current:
                break
            assert not any(word in current for word in ('panic', 'failed', 'Exception')), current
            assert time.monotonic() < deadline, current
            guest.qmp('cont')

        registers = guest.monitor('info registers')
        (build / f'{name}-registers.txt').write_text(registers)
        assert 'CR0=80010011' in registers and 'CR4=00000000' in registers
        assert int(re.search(r'CR3=([0-9a-fA-F]+)', registers)[1], 16) == guest.value('_paging_directory_physical')
        ticks = guest.value('_timer_ticks')
        if expected is None:
            assert 'Heap tests passed:' in current, current
            assert ownership(guest, 'after') == baseline
            assert guest.value('_heap_pages') == guest.value('_heap_allocations') == 0
            assert guest.value('_first_extent') == 0
            if tables is not None:
                assert paging_snapshot(guest, 'after') == tables
                assert guest.value('_free_pages') == physical
                assert guest.value('_free_virtual_pages') == virtual
            else:
                paging_snapshot(guest, 'after')
                assert guest.value('_free_pages') == guest.value('_managed_pages')
                assert guest.value('_free_virtual_pages') == (0x10000000 - guest.value('_paging_identity_end')) // 4096
            if high:
                assert guest.value('_kheap_test_high_physical') >= 0x10000000
            guest.qmp('cont')
            time.sleep(0.2)
            guest.qmp('stop')
            assert guest.value('_timer_ticks') > ticks
        else:
            assert 'HLT=1' in registers
            assert int(re.search(r'EFL=([0-9a-fA-F]+)', registers)[1], 16) & 0x200 == 0
            if expected.startswith('Exception'):
                fault = guest.value('_kheap_test_fault_address')
                assert int(re.search(r'CR2=([0-9a-fA-F]+)', registers)[1], 16) == fault
                assert 'non-present page, write, supervisor' in current
            frozen = ownership(guest, 'halted')
            guest.qmp('cont')
            time.sleep(0.15)
            guest.qmp('stop')
            assert guest.value('_timer_ticks') == ticks
            assert ownership(guest, 'still-halted') == frozen

        guest.qmp('quit')
        guest.process.wait(timeout=5)
        log = (build / f'{name}-qemu.log').read_text()
        assert log.count('CPU Reset') == 2 and 'Triple fault' not in log
        faults = [(int(vector, 16), int(error, 16)) for vector, error in
                  re.findall(r'v=([0-9a-f]{2}) e=([0-9a-f]+)', log, re.I) if int(vector, 16) < 32]
        assert faults == ([(14, 2)] if expected and expected.startswith('Exception') else []), faults
        print(f'{name}: PASS' + ('; expected diagnostic and stable halt' if expected else
                                '; full ownership restored, mappings clean, timer continues'))
    finally:
        guest.close()


if __name__ == '__main__':
    environment = configure(__doc__)
    build, symbols = environment.build, environment.symbols
    assert environment.gdb.exists(), 'Heap tests require GDB; pass --gdb PATH'
    run_case('heap-128m')
    run_case('heap-512m', ram=512)
    run_case('heap-high-fragmented', ram=512, high=True, entry='_kheap_test_high_entry')
    run_case('heap-preinit', hook='_kheap_init', entry='_kheap_test_preinit_entry')
    for violation in ('interior', 'unaligned', 'double', 'stale', 'foreign', 'unmapped'):
        run_case(f'heap-bad-{violation}', entry=f'_kheap_bad_{violation}',
                 expected='Heap panic: invalid or already freed pointer')
    run_case('heap-reinit', entry='_kheap_bad_reinit', expected='Heap panic: initialize once after VMM')
    run_case('heap-init-order', hook='_vmm_init', entry='_kheap_bad_reinit',
             expected='Heap panic: initialize once after VMM')
    run_case('heap-released-fault', entry='_kheap_fault_released', expected='Exception 14: Page Fault')
