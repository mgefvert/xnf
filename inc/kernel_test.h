#ifndef XNF_KERNEL_TEST_H
#define XNF_KERNEL_TEST_H

/* Present only in builds configured with -DXNF_BUILD_TESTS=ON. */
void pmm_self_test(void);
void vmm_self_test(void);

#endif
