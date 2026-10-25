/* Built-in provider: Apple libpthread-330.250.2, paired with xnu-4903.270.47. */
#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include "op2_pthread_compat.h"
#include "op2_libpthread/kern/kern_init.c"

/* Keep the provider's full layout within the kernel's reserved storage. */
_Static_assert(sizeof(struct ksyn_waitq_element) == 56,
    "update OP2 ksyn storage in sys/pthread_internal.h");

#define XNU_KERNEL_PRIVATE 1
#include <pexpert/pexpert.h>

void
op2_pthread_register(void)
{
    kern_return_t kr = op2_pthread_start(NULL, NULL);
    if (kr != KERN_SUCCESS || op2_pthread_kern == NULL) {
        panic("OP2: pthread provider registration failed: %d", kr);
    }
    kprintf("OP2 pthread: libpthread-330.250.2 registered\n");
}
#endif /* ARM64_BOARD_CONFIG_MSM8994 */
