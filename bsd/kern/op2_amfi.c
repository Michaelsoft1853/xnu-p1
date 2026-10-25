/*
 * OnePlus 2 Built-in MAC Policy Shim (AMFI, Sandbox, Quarantine)
 *
 * Implements minimal MAC policy modules to allow real Apple iOS 12.5.7
 * userland (dyld, launchd, and system services) to evaluate policies,
 * validate signatures as platform binaries, and run daemons cleanly.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/codesign.h>
#include <kern/cs_blobs.h>
#include <security/mac_policy.h>

#define AMFI_CALL_DYLD_POLICY_SELF    0x5a
#define AMFI_CALL_DYLD_POLICY_PROC    0x5b

/* All dyld permission flags */
#define AMFI_DYLD_ALL_ALLOWED         0xffffffffffffffffULL

struct amfi_dyld_policy_self_args {
	uint64_t in_flags;
	user_addr_t out_flags_ptr;
};

struct amfi_dyld_policy_proc_args {
	pid_t pid;
	uint32_t pad;
	uint64_t in_flags;
	user_addr_t out_flags_ptr;
};

static int
op2_mac_policy_syscall(struct proc *p, int call, user_addr_t arg)
{
	int pid = p ? p->p_pid : -1;

	if (call == AMFI_CALL_DYLD_POLICY_SELF) {
		struct amfi_dyld_policy_self_args kargs;
		int err = copyin(arg, &kargs, sizeof(kargs));
		if (err) {
			printf("OP2 MAC: call 0x5a copyin failed (%d) for pid %d\n", err, pid);
			return err;
		}

		uint64_t out_flags = AMFI_DYLD_ALL_ALLOWED;
		if (kargs.out_flags_ptr != 0) {
			err = copyout(&out_flags, kargs.out_flags_ptr, sizeof(out_flags));
			if (err) {
				printf("OP2 MAC: call 0x5a copyout failed (%d) for pid %d\n", err, pid);
				return err;
			}
		}
		printf("OP2 MAC: dyld_policy_self handled for pid %d (in=0x%llx, out=0x%llx)\n",
		       pid, (unsigned long long)kargs.in_flags, (unsigned long long)out_flags);
		return 0;
	}

	if (call == AMFI_CALL_DYLD_POLICY_PROC) {
		struct amfi_dyld_policy_proc_args kargs;
		int err = copyin(arg, &kargs, sizeof(kargs));
		if (err) {
			struct amfi_dyld_policy_self_args fallback;
			if (copyin(arg, &fallback, sizeof(fallback)) == 0 && fallback.out_flags_ptr != 0) {
				uint64_t out_flags = AMFI_DYLD_ALL_ALLOWED;
				copyout(&out_flags, fallback.out_flags_ptr, sizeof(out_flags));
			}
			return 0;
		}

		uint64_t out_flags = AMFI_DYLD_ALL_ALLOWED;
		if (kargs.out_flags_ptr != 0) {
			copyout(&out_flags, kargs.out_flags_ptr, sizeof(out_flags));
		}
		printf("OP2 MAC: dyld_policy_proc handled for target pid %d from pid %d\n",
		       kargs.pid, pid);
		return 0;
	}

	printf("OP2 MAC: syscall call=0x%x (%d) from pid %d (arg=0x%llx) -> success\n",
	       call, call, pid, (unsigned long long)arg);
	return 0;
}

static int
op2_mac_vnode_check_signature(struct vnode *vp, __unused struct label *label,
    __unused cpu_type_t cpu_type, __unused struct cs_blob *cs_blob,
    unsigned int *cs_flags, __unused unsigned int *signer_type,
    __unused int flags,
    __unused char **fatal_failure_desc, __unused size_t *fatal_failure_desc_len)
{
	if (cs_flags != NULL) {
		*cs_flags |= CS_VALID | CS_SIGNED | CS_PLATFORM_BINARY;
	}
	if (vp != NULL && vp->v_name != NULL) {
		printf("OP2 MAC: vnode_check_signature for '%s' -> platform binary\n", vp->v_name);
	}
	return 0;
}

static int
op2_mac_proc_check_run_cs_invalid(__unused struct proc *p)
{
	return 0;
}

static int
op2_mac_proc_check_get_cs_info(__unused kauth_cred_t cred, __unused struct proc *target, __unused unsigned int op)
{
	return 0;
}

static int
op2_mac_proc_check_set_cs_info(__unused kauth_cred_t cred, __unused struct proc *target, __unused unsigned int op)
{
	return 0;
}

static int
op2_mac_proc_check_fork(__unused kauth_cred_t cred, __unused struct proc *proc)
{
	return 0;
}

static const struct mac_policy_ops op2_mac_ops = {
	.mpo_policy_syscall = op2_mac_policy_syscall,
	.mpo_vnode_check_signature = op2_mac_vnode_check_signature,
	.mpo_proc_check_run_cs_invalid = op2_mac_proc_check_run_cs_invalid,
	.mpo_proc_check_get_cs_info = op2_mac_proc_check_get_cs_info,
	.mpo_proc_check_set_cs_info = op2_mac_proc_check_set_cs_info,
	.mpo_proc_check_fork = op2_mac_proc_check_fork,
};

static struct mac_policy_conf op2_amfi_mac_conf = {
	.mpc_name = "AMFI",
	.mpc_fullname = "OnePlus 2 Built-in AMFI Shim",
	.mpc_field_off = NULL,
	.mpc_labelnames = NULL,
	.mpc_labelname_count = 0,
	.mpc_ops = &op2_mac_ops,
	.mpc_loadtime_flags = 0,
	.mpc_runtime_flags = 0,
};

static struct mac_policy_conf op2_sandbox_mac_conf = {
	.mpc_name = "Sandbox",
	.mpc_fullname = "OnePlus 2 Built-in Sandbox Shim",
	.mpc_field_off = NULL,
	.mpc_labelnames = NULL,
	.mpc_labelname_count = 0,
	.mpc_ops = &op2_mac_ops,
	.mpc_loadtime_flags = 0,
	.mpc_runtime_flags = 0,
};

static struct mac_policy_conf op2_quarantine_mac_conf = {
	.mpc_name = "Quarantine",
	.mpc_fullname = "OnePlus 2 Built-in Quarantine Shim",
	.mpc_field_off = NULL,
	.mpc_labelnames = NULL,
	.mpc_labelname_count = 0,
	.mpc_ops = &op2_mac_ops,
	.mpc_loadtime_flags = 0,
	.mpc_runtime_flags = 0,
};

static mac_policy_handle_t op2_amfi_mac_handle;
static mac_policy_handle_t op2_sandbox_mac_handle;
static mac_policy_handle_t op2_quarantine_mac_handle;

void op2_amfi_policy_register(void);
void
op2_amfi_policy_register(void)
{
	int err;

	err = mac_policy_register(&op2_amfi_mac_conf, &op2_amfi_mac_handle, NULL);
	printf("OP2 MAC: policy register '%s' -> %s (err=%d, handle=%u)\n",
	       op2_amfi_mac_conf.mpc_name, err ? "FAILED" : "OK", err, (unsigned int)op2_amfi_mac_handle);

	err = mac_policy_register(&op2_sandbox_mac_conf, &op2_sandbox_mac_handle, NULL);
	printf("OP2 MAC: policy register '%s' -> %s (err=%d, handle=%u)\n",
	       op2_sandbox_mac_conf.mpc_name, err ? "FAILED" : "OK", err, (unsigned int)op2_sandbox_mac_handle);

	err = mac_policy_register(&op2_quarantine_mac_conf, &op2_quarantine_mac_handle, NULL);
	printf("OP2 MAC: policy register '%s' -> %s (err=%d, handle=%u)\n",
	       op2_quarantine_mac_conf.mpc_name, err ? "FAILED" : "OK", err, (unsigned int)op2_quarantine_mac_handle);
}
