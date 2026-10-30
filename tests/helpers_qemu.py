"""Run test-only string/container checks: python tests/helpers_qemu.py --build cmake-build-tests.

The heap test harness checks ownership, mappings, timer progress, and fault logs.
"""
import sys

sys.dont_write_bytecode = True
import kheap_qemu
from qemu_test import configure

if __name__ == '__main__':
    environment = configure(__doc__)
    kheap_qemu.environment = environment
    kheap_qemu.build = environment.build
    kheap_qemu.symbols = environment.symbols
    assert environment.gdb.exists(), 'Helper tests require GDB; pass --gdb PATH'
    for ram in (128, 512):
        kheap_qemu.run_case(f'helpers-{ram}m', ram=ram, entry='_helpers_test_entry',
                            stage_symbol='_helpers_test_stage', final_stage=2,
                            success_text='Helper tests passed:')
