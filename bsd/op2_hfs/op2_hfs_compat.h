/*
 * Compile Apple's HFS kext sources inside OP2 the way the kext is built:
 * with the kext (KPI) view of the kernel headers, and with only HFS's own
 * kext-config.h deciding features. The kernel's -DCONFIG_* options are not
 * visible to a kext build, so the ones HFS tests are cleared first.
 */
#ifndef OP2_HFS_COMPAT_H
#define OP2_HFS_COMPAT_H
#undef BSD_KERNEL_PRIVATE
#undef XNU_KERNEL_PRIVATE
#undef CONFIG_PROTECT
#undef CONFIG_SEARCHFS
#undef CONFIG_HFS_DIRLINK
#undef CONFIG_SECLUDED_RENAME
#undef CONFIG_MACF
#undef CONFIG_HFS_RBTREE
#undef QUOTA
#undef NAMEDSTREAMS
#undef FIFO
#undef HFS_COMPRESSION

struct arm_saved_state;
typedef struct arm_saved_state arm_saved_state_t;

#include "core/kext-config.h"
#include <IOKit/IOLib.h>

void *IOMallocZero(vm_size_t size);

#ifndef IOMallocType
#define IOMallocType(type) IOMallocZero(sizeof(type))
#endif

#ifndef IOFreeType
#define IOFreeType(ptr, type) IOFree((ptr), sizeof(type))
#endif

#ifndef IOMallocZeroType
#define IOMallocZeroType(type) IOMallocZero(sizeof(type))
#endif

#ifndef IONewZero
#define IONewZero(type, number) \
( ((number) != 0 && ((vm_size_t) ((sizeof(type) * (number) / (number))) != sizeof(type)) \
  ? 0 \
  : ((type*)IOMallocZero(sizeof(type) * (number)))) )
#endif

#ifndef IOMallocData
#define IOMallocData(size) IOMalloc(size)
#endif

#ifndef IOMallocZeroData
#define IOMallocZeroData(size) IOMallocZero(size)
#endif

#ifndef IONewData
#define IONewData(type, count) IONew(type, count)
#endif

#ifndef IONewZeroData
#define IONewZeroData(type, count) IONewZero(type, count)
#endif

#ifndef IOFreeData
#define IOFreeData(ptr, size) IOFree((ptr), (size))
#endif

#ifndef IODeleteData
#define IODeleteData(ptr, type, count) IODelete((ptr), type, (count))
#endif

#ifndef TASK_PHYSICAL_WRITE_METADATA
#define TASK_PHYSICAL_WRITE_METADATA 0
#endif

#ifndef TASK_BALANCE_CREDIT
#define TASK_BALANCE_CREDIT 0
#endif

#define task_update_physical_writes(task, type, size, balance) do {} while (0)

struct fsioc_cas_bsdflags {
	uint32_t expected_flags;
	uint32_t new_flags;
	uint32_t actual_flags;
};
#ifndef FSIOC_CAS_BSDFLAGS
#define FSIOC_CAS_BSDFLAGS _IOWR('A', 20, struct fsioc_cas_bsdflags)
#endif

struct vnode;
int vn_getpath(struct vnode *vp, char *pathbuf, int *len);
static inline int vn_getpath_ext(struct vnode *vp, struct vnode *dvp __attribute__((unused)), char *pathbuf, int *len, int flags __attribute__((unused))) {
	return vn_getpath(vp, pathbuf, len);
}

#ifndef ZC_NOENCRYPT
#define ZC_NOENCRYPT 0
#endif

#include <kern/zalloc.h>

static inline zone_t zone_create(const char *name, vm_size_t size, int flags __attribute__((unused))) {
	return zinit(size, 8192 * size, 4096, name);
}

/* Built in, not a loadable kext (see op2_hfs_iokit.c). */
#define OSKextGetCurrentLoadTag op2_hfs_current_load_tag
#endif
