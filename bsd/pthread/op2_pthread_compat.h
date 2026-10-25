/*
 * Compile the matching libpthread provider with its kext view of BSD headers.
 * It owns its full ksyn_waitq_element layout; the kernel's opaque counterpart
 * reserves this layout. All communication with private BSD state uses callbacks.
 */
#ifndef OP2_PTHREAD_COMPAT_H
#define OP2_PTHREAD_COMPAT_H
#undef BSD_KERNEL_PRIVATE
#undef XNU_KERNEL_PRIVATE
#define __PTHREAD_EXPOSE_INTERNALS__ 1
#define NEEDS_SCHED_CALL_T 1

struct arm_saved_state;
typedef struct arm_saved_state arm_saved_state_t;

/* The seven-argument kext API forwards to the eight-argument kernel vm_fault. */
#define vm_fault vm_fault_external
#define mach_vm_map mach_vm_map_external
#define mach_vm_allocate mach_vm_allocate_external

/* Avoid collisions with the kernel side of the callback interface. */
#define pthread_kern op2_pthread_kern
#define current_uthread op2_pthread_current_uthread
#define pthread_start op2_pthread_start
#define pthread_stop op2_pthread_stop

/* os/log passes &__dso_handle as the containing Mach-O image address. */
extern void *op2_pthread_log_image __asm("__mh_execute_header");
#define __dso_handle op2_pthread_log_image

void op2_pthread_register(void);
#endif
