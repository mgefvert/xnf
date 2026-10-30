"""Verify object lifetimes: python tests/object_qemu.py --build cmake-build-tests.

Disposable guests exercise ownership, allocation pressure and initialization.
The heap harness also checks full PMM/VMM ownership, page tables and faults.
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
    assert environment.gdb.exists(), 'Object tests require GDB; pass --gdb PATH'
    for ram in (128, 512):
        kheap_qemu.run_case(f'object-{ram}m', ram=ram, entry='_object_test_entry',
                            stage_symbol='_object_test_stage', final_stage=2,
                            success_text='Object tests passed:')
    kheap_qemu.run_case('object-preinit', hook='_kheap_init', entry='_object_test_preinit_entry',
                        stage_symbol='_object_test_stage', final_stage=2,
                        success_text='Object tests passed:')
    kheap_qemu.run_case('object-bad-release', entry='_object_test_bad_release',
                        expected='Object panic: release without caller reference')
