/*
 * Entry points supplied only by the optional kernel test build.
 * Keeping test declarations separate prevents tests becoming normal allocator APIs.
 */

#ifndef XNF_KERNEL_TEST_H
#define XNF_KERNEL_TEST_H

#include "types.h"

/* Present only in builds configured with -DXNF_BUILD_TESTS=ON. */
void pmm_self_test(void);
void vmm_self_test(void);
void kheap_self_test(void);
void helpers_self_test(void);
void object_self_test(void);
/* Test-only saturation fixture; never use to manage real references. */
struct Object;
void object_test_force_references(struct Object *object, uint32_t references);

#endif
