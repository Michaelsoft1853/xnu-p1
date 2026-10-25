/*
 * Built-in replacement for HFS's IOKit glue on OnePlus 2 (MSM8994).
 */
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/malloc.h>
#include <sys/ubc.h>

#include "hfs.h"
#include "hfs_cnode.h"
#include "hfs_dbg.h"
#include "hfs_iokit.h"
#include "hfs_encodings/hfs_encodings_internal.h"

#ifndef panic_on_assert
bool panic_on_assert;
#endif

#if DEBUG
bool hfs_corruption_panics = true;
#endif

extern struct vnodeopv_desc hfs_vnodeop_opv_desc;
extern struct vnodeopv_desc hfs_specop_opv_desc;
extern struct vnodeopv_desc hfs_fifoop_opv_desc;
extern struct vfsops hfs_vfsops;

void op2_hfs_register(void);

static vfstable_t op2_hfs_vfs_handle;

void
op2_hfs_register(void)
{
#ifndef panic_on_assert
	panic_on_assert = PE_i_can_has_kernel_configuration() & kPEICanHasAssertions;
#endif

#if DEBUG
	PE_parse_boot_argn("hfs_corruption_panics", &hfs_corruption_panics, sizeof(hfs_corruption_panics));
#endif

	/* The HFSEncodings kext's start(): its converters must exist before any mount. */
	hfs_converterinit();

	struct vnodeopv_desc *op_descs[] = {
		&hfs_vnodeop_opv_desc,
		&hfs_specop_opv_desc,
#if FIFO
		&hfs_fifoop_opv_desc,
#endif
	};

#ifndef VFS_TBLVNOP_SECLUDE_RENAME
#define VFS_TBLVNOP_SECLUDE_RENAME 0
#endif

	struct vfs_fsentry vfe = {
		.vfe_vfsops = &hfs_vfsops,
		.vfe_vopcnt = sizeof(op_descs) / sizeof(op_descs[0]),
		.vfe_opvdescs = op_descs,
		.vfe_fsname = "hfs",
		.vfe_flags = (VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBLREADDIR_EXTENDED
		    | VFS_TBL64BITREADY | VFS_TBLVNOP_PAGEOUTV2 | VFS_TBLVNOP_PAGEINV2
		    | VFS_TBLTHREADSAFE | VFS_TBLCANMOUNTROOT | VFS_TBLVNOP_SECLUDE_RENAME
		    | VFS_TBLNATIVEXATTR)
	};

	int ret = vfs_fsadd(&vfe, &op2_hfs_vfs_handle);
	if (ret) {
		panic("OP2 HFS: vfs_fsadd failed: %d", ret);
	}

	hfs_init_zones();
	hfs_sysctl_register();

	uint32_t num_cpus;
	size_t sz = sizeof(num_cpus);

	if (sysctlbyname("hw.physicalcpu", &num_cpus, &sz, NULL, 0) == 0) {
		if ((2 * num_cpus) > MAX_CACHED_ORIGINS_DEFAULT) {
			_hfs_max_origins = 2 * num_cpus;
			_hfs_max_file_origins = 2 * num_cpus;
		} else if ((2 * num_cpus) > MAX_CACHED_FILE_ORIGINS_DEFAULT) {
			_hfs_max_file_origins = 2 * num_cpus;
		}
	}

	printf("OP2 HFS: hfs-627.100.6 registered\n");
}

/* Built in, not a loadable kext: there is no kext to retain per mount. */
OSKextLoadTag
op2_hfs_current_load_tag(void)
{
	return kOSKextInvalidLoadTag;
}

int
hfs_is_ejectable(__unused const char *cdev_name)
{
	return 0;
}

void
hfs_iterate_media_with_content(__unused const char *content_uuid_cstring,
    __unused int (*func)(const char *bsd_name, const char *uuid_str, void *arg),
    __unused void *arg)
{
}

kern_return_t
hfs_get_platform_serial_number(char *serial_number_str, uint32_t len)
{
	if (len < 1) {
		return 0;
	}
	serial_number_str[0] = '\0';
	return KERN_NOT_SUPPORTED;
}

void *
IOMallocZero(vm_size_t size)
{
	void *address = IOMalloc(size);
	if (address) {
		bzero(address, size);
	}
	return address;
}

void
hashdestroy(void *hashtbl, int type, u_long hashmask __unused)
{
	if (hashtbl) {
		FREE(hashtbl, type);
	}
}

int
ubc_create_upl(vnode_t vp, off_t f_offset, int bufsize, upl_t *uplp, upl_page_info_t **plp, int uplflags)
{
	return ubc_create_upl_external(vp, f_offset, bufsize, uplp, plp, uplflags);
}

