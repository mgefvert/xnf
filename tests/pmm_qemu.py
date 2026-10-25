"""Build first, then run: python tests/pmm_qemu.py --build cmake-build-tests.

Checks the boot self-test, all bitmap bits against an independent E820 model,
control registers, timer progress, and exception/reset logs. Optional GDB tests
inject synthetic E820 maps before the early memory check; unmapped high frames are never
dereferenced. A separate guest verifies that a late bootstrap allocation panics
and halts without changing PMM state. Logs and snapshots stay in the build directory.
"""
import re
import socket
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from qemu_test import configure, HIDDEN as hidden

def expected_bitmap(entries, occupied):
    # Independent two-pass model: usable interiors first, then every page that
    # intersects a reserved entry or live kernel/bootstrap allocation.
    entries = [entry for entry in entries if entry[3] & 1]
    free = set()
    limit = 1 << 32
    # Round available ranges inward: only complete pages can be allocated.
    for base, length, kind, attributes in entries:
        if kind == 1:
            first = (min(base, limit) + 4095) // 4096
            last = min(base + length, limit) // 4096
            free.update(range(first, last))
    exclusions = [(base, base + length) for base, length, kind, _ in entries if kind != 1]
    exclusions += occupied + [(0, 4096)]
    # Round exclusions outward: even a one-byte reservation blocks its frame.
    for base, end in exclusions:
        free.difference_update(range(min(base, limit) // 4096,
                                     (min(end, limit) + 4095) // 4096))
    bitmap = bytearray(b'\xff' * (128 * 1024))
    # Host bytes have eight frame bits each, whereas the guest uses 32-bit
    # words. Comparing serialized bits checks the same ownership independently.
    for frame in free:
        bitmap[frame // 8] &= ~(1 << (frame % 8))
    return bitmap, len(free)


def run_case(name, ram, fixture=None, retirement_probe=False):
    port = None
    if fixture is not None or retirement_probe:
        with socket.socket() as endpoint:
            endpoint.bind(('127.0.0.1', 0))
            port = endpoint.getsockname()[1]
    guest = Guest(name, ram, port)
    try:
        if fixture is None:
            guest.qmp('cont')
        else:
            data = build / f'{name}-e820.bin'
            data.write_bytes(b''.join(struct.pack('<QQII', *entry) for entry in fixture))
            commands = build / f'{name}-gdb.txt'
            commands.write_text('\n'.join([
                'set pagination off', 'set confirm off', 'set architecture i386',
                f'target remote 127.0.0.1:{port}',
                f'hbreak *{symbols["_boot_memory_check"]:#x}', 'continue', 'delete breakpoints',
                f'restore {data.name} binary {symbols["_e820_map"]:#x}',
                f'set {{unsigned short}}{symbols["_e820_entry_count"]:#x} = {len(fixture)}',
                f'set {{unsigned char}}{symbols["_e820_detection_failed"]:#x} = 0',
                'detach', 'quit',
            ]) + '\n')
            result = subprocess.run([str(gdb), '-q', '-nx', '-batch', '-x', str(commands)],
                                    capture_output=True, text=True, timeout=20, creationflags=hidden,
                                    cwd=build)
            (build / f'{name}-gdb.log').write_text(result.stdout + result.stderr)
            assert result.returncode == 0, result.stdout + result.stderr
        time.sleep(2)
        guest.qmp('stop')
        registers = guest.monitor('info registers')
        (build / f'{name}-registers.txt').write_text(registers)
        assert 'CR0=80010011' in registers and 'CR4=00000000' in registers
        cr3 = int(re.search(r'CR3=([0-9a-fA-F]+)', registers)[1], 16)
        assert cr3 == guest.value('_paging_directory_physical')
        raw = guest.read(0xb8000, 4000, 'vga')[::2].decode('ascii')
        screen = '\n'.join(raw[i:i + 80].rstrip() for i in range(0, 2000, 80))
        (build / f'{name}-screen.txt').write_text(screen)
        assert 'PMM tests passed:' in screen and 'Kernel started.' in screen, screen
        assert 'Physical page ownership transferred to PMM; bootstrap allocator retired.' in screen, screen
        assert not any(word in screen for word in ('failed', 'panic', 'Exception')), screen

        count = struct.unpack('<H', guest.read(symbols['_e820_entry_count'], 2, 'e820-count'))[0]
        entries = list(struct.iter_unpack('<QQII', guest.read(symbols['_e820_map'], count * 24, 'e820')))
        occupied = [(symbols['__kernel_start'], symbols['__kernel_end'])]
        occupied.append((cr3, cr3 + 4096))
        directory = struct.unpack('<1024I', guest.read(cr3, 4096, 'pd'))
        for entry in directory:
            if entry & 1:
                table = entry & ~4095
                occupied.append((table, table + 4096))
        # Current startup has no other memblock_alloc call sites.
        expected, managed = expected_bitmap(entries, occupied)
        # Startup's ownership tests must return every loan. Matching all bits,
        # not only counters, detects freeing a wrong but equally sized set.
        actual = guest.read(symbols['_bitmap'], 128 * 1024, 'bitmap')
        assert actual == expected, f'{name}: bitmap differs from independent E820 model'
        assert guest.value('_managed_pages') == guest.value('_free_pages') == managed
        if name == 'pmm-top-frame':
            match = re.search(r'PMM allocation test: (.*?) release OK', screen)
            allocated = {int(value, 16) for value in re.findall(r'0x[0-9A-F]+', match[1])}
            assert allocated == {0xffffd000, 0xffffe000, 0xfffff000}, allocated
        ticks = guest.value('_timer_ticks')
        guest.qmp('cont')
        time.sleep(1.1)
        guest.qmp('stop')
        assert guest.value('_timer_ticks') > ticks, 'timer did not progress'
        if retirement_probe:
            # Disposable guest only: redirect execution to a real, late cdecl
            # memblock_alloc(4096, 4096) call. It must panic before mutating state.
            commands = build / f'{name}-late-alloc-gdb.txt'
            commands.write_text('\n'.join([
                'set pagination off', 'set confirm off', 'set architecture i386',
                f'target remote 127.0.0.1:{port}',
                'set {unsigned long long}($esp + 4) = 4096',
                'set {unsigned long long}($esp + 12) = 4096',
                f'set $eip = {symbols["_memblock_alloc"]:#x}',
                'detach', 'quit',
            ]) + '\n')
            result = subprocess.run([str(gdb), '-q', '-nx', '-batch', '-x', str(commands)],
                                    capture_output=True, text=True, timeout=20, creationflags=hidden)
            (build / f'{name}-late-alloc-gdb.log').write_text(result.stdout + result.stderr)
            assert result.returncode == 0, result.stdout + result.stderr
            time.sleep(0.3)
            guest.qmp('stop')
            raw = guest.read(0xb8000, 4000, 'panic-vga')[::2].decode('ascii')
            panic_screen = '\n'.join(raw[i:i + 80].rstrip() for i in range(0, 2000, 80))
            (build / f'{name}-panic-screen.txt').write_text(panic_screen)
            assert 'Memblock panic: bootstrap allocator retired; use PMM for physical pages' in panic_screen, panic_screen
            assert guest.read(symbols['_bitmap'], 128 * 1024, 'panic-bitmap') == actual
            assert guest.value('_free_pages') == managed
            registers = guest.monitor('info registers')
            assert 'HLT=1' in registers
            assert int(re.search(r'EFL=([0-9a-fA-F]+)', registers)[1], 16) & 0x200 == 0
            ticks = guest.value('_timer_ticks')
            guest.qmp('cont')
            time.sleep(0.2)
            guest.qmp('stop')
            assert guest.value('_timer_ticks') == ticks, 'panic did not disable interrupts'
            print(f'{name}: late bootstrap allocation panicked and halted; PMM state unchanged')
        guest.qmp('quit')
        guest.process.wait(timeout=5)
        log = (build / f'{name}-qemu.log').read_text()
        assert not re.search(r'check_exception|Triple fault|v=(?:0d|0e|08)', log, re.I)
        assert log.count('CPU Reset') == 2, 'unexpected reset'
        print(f'{name}: PASS, managed/free={managed}, bitmap matches, timer continues')
        print('\n'.join(line for line in screen.splitlines() if line.startswith('PMM')))
    finally:
        guest.close()


if __name__ == '__main__':
    environment = configure(__doc__)
    build, gdb, symbols = environment.build, environment.gdb, environment.symbols
    Guest = environment.guest
    run_case('pmm-128m', 128)
    run_case('pmm-512m', 512)
    if gdb.exists():
        entries = [(0, 0x9fc00, 1, 1), (0x100000, 0x3f00000, 1, 1),
                   (0x6001, 0x2ffe, 2, 1), (0x3000001, 0x1ffe, 2, 1),
                   (0x10000001, 0x4000, 1, 1), (0x10002001, 1, 2, 1),
                   (0x70000000, 8192, 1, 1), (0xffffe001, 0x5000, 1, 1),
                   (0x100010000, 0x10000, 1, 1)]
        run_case('pmm-overlaps', 128, entries)
        run_case('pmm-reversed', 128, list(reversed(entries)))
        run_case('pmm-top-frame', 128, [
            (symbols['__kernel_start'], symbols['__kernel_end'] - symbols['__kernel_start'], 1, 1),
            (0xffffd000, 3 * 4096, 1, 1)])
        run_case('pmm-disabled', 128, [
            (0, 0x9fc00, 1, 1), (0x100000, 0x3f00000, 1, 1),
            (0x2000000, 4096, 2, 0), (0x10000000, 4096, 1, 0),
            (0xfffffffffffffff0, 4096, 1, 0)])
        run_case('pmm-retirement', 128, retirement_probe=True)
    else:
        print('Synthetic E820 cases skipped: pass --gdb to enable them.')
