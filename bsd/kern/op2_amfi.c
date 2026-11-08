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

static uint64_t op2_sandbox_seen;

static int
op2_mac_policy_syscall(struct proc *p, int call, user_addr_t arg)
{
	int pid = p ? p->p_pid : -1;

	if (call == AMFI_CALL_DYLD_POLICY_SELF) {
		struct amfi_dyld_policy_self_args kargs;
		int err = copyin(arg, &kargs, sizeof(kargs));
		if (err) {
			return err;
		}

		uint64_t out_flags = AMFI_DYLD_ALL_ALLOWED;
		if (kargs.out_flags_ptr != 0) {
			err = copyout(&out_flags, kargs.out_flags_ptr, sizeof(out_flags));
			if (err) {
				return err;
			}
		}
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
		return 0;
	}

	if (call >= 0 && call < 64 && !(op2_sandbox_seen & (1ULL << call))) {
		op2_sandbox_seen |= 1ULL << call;
		char name[32] = {0};
		proc_name(pid, name, sizeof(name));
		printf("OP2 Sandbox: %s op %d\n", name, call);
	}

	switch (call) {
	case 2: {
		/*
		 * sandbox_check() / _sandbox_check_by_audit_token():
		 * arg[0] is user_addr_t out_ptr pointing to result on user stack.
		 * Userland expects *out_ptr == 0 for allowed (non-zero = denied).
		 */
		user_addr_t out_ptr = 0;
		if (copyin(arg, &out_ptr, sizeof(out_ptr)) == 0 && out_ptr != 0) {
			uint64_t zero = 0;
			copyout(&zero, out_ptr, sizeof(zero));
		}
		return 0;
	}
	case 28: /* 0x1c */ {
		/*
		 * sandbox_check_by_audit_token() variant:
		 * arg[16] is user_addr_t out_ptr pointing to result on user stack.
		 * Userland initializes result to -1 and returns it if syscall returns 0.
		 */
		user_addr_t out_ptr = 0;
		if (copyin(arg + 16, &out_ptr, sizeof(out_ptr)) == 0 && out_ptr != 0) {
			uint64_t zero = 0;
			copyout(&zero, out_ptr, sizeof(zero));
		}
		if (copyin(arg, &out_ptr, sizeof(out_ptr)) == 0 && out_ptr != 0) {
			uint64_t zero = 0;
			copyout(&zero, out_ptr, sizeof(zero));
		}
		return 0;
	}
	case 259: /* 0x103 */ {
		/*
		 * sandbox_check_by_reference():
		 * arg[8] is user_addr_t out_ptr.
		 */
		user_addr_t out_ptr = 0;
		if (copyin(arg + 8, &out_ptr, sizeof(out_ptr)) == 0 && out_ptr != 0) {
			uint64_t zero = 0;
			copyout(&zero, out_ptr, sizeof(zero));
		}
		return 0;
	}
	case 4:         /* container path (audit token / pid) */
	case 5:         /* extension issue */
	case 6:         /* extension consume */
	case 0x43:      /* container expected */
		return ENOENT;
	default:
		return 0;
	}
}

static int
op2_mac_vnode_check_signature(struct vnode *vp, __unused struct label *label,
    __unused cpu_type_t cpu_type, __unused struct cs_blob *cs_blob,
    unsigned int *cs_flags, __unused unsigned int *signer_type,
    __unused int flags,
    __unused char **fatal_failure_desc, __unused size_t *fatal_failure_desc_len)
{
	if (cs_flags != NULL) {
		*cs_flags |= CS_VALID | CS_SIGNED | CS_PLATFORM_BINARY | CS_DEBUGGED;
	}
	extern int cs_debug;
	if (cs_debug > 0 && vp != NULL && vp->v_name != NULL) {
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
