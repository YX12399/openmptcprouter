// SPDX-License-Identifier: GPL-2.0
/*
 * ratan_sched.bpf.c — RATAN MPTCP packet scheduler (BPF struct_ops).
 *
 * Per-packet decision rule: pick the active subflow with the highest
 * weight in the pinned ratan_path_weights map. Fall back to first-active
 * if all weights are zero. Userspace (ratan-predictor) writes the weights.
 *
 * Kernel ABI: requires the MPTCP BPF struct_ops scheduler hooks present
 * in mainline kernel >=6.5 and in OMR's 6.6 tree via 999-mptcp-bpf.patch.
 * Uses only kfuncs that exist on both 6.6 and 6.12.
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char _license[] SEC("license") = "GPL";

#ifndef MPTCP_SUBFLOWS_MAX
#define MPTCP_SUBFLOWS_MAX 8
#endif

extern void mptcp_subflow_set_scheduled(struct mptcp_subflow_context *subflow,
					bool scheduled) __ksym;
extern struct mptcp_subflow_context *
bpf_mptcp_subflow_ctx_by_pos(const struct mptcp_sched_data *data,
			     unsigned int pos) __ksym;
extern bool mptcp_subflow_active(struct mptcp_subflow_context *subflow) __ksym;

/* Pinned by name; userspace daemon reads/writes via /sys/fs/bpf/ratan/path_weights.
 * key   = subflow position (0..MPTCP_SUBFLOWS_MAX-1)
 * value = u32 weight (0..100); 0 == drain this path.
 */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, MPTCP_SUBFLOWS_MAX);
	__type(key, __u32);
	__type(value, __u32);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} ratan_path_weights SEC(".maps");

SEC("struct_ops/mptcp_sched_ratan_init")
void BPF_PROG(mptcp_sched_ratan_init, struct mptcp_sock *msk)
{
}

SEC("struct_ops/mptcp_sched_ratan_release")
void BPF_PROG(mptcp_sched_ratan_release, struct mptcp_sock *msk)
{
}

SEC("struct_ops/mptcp_sched_ratan_get_subflow")
int BPF_PROG(mptcp_sched_ratan_get_subflow, struct mptcp_sock *msk,
	     struct mptcp_sched_data *data)
{
	struct mptcp_subflow_context *subflow;
	__u32 best_idx = 0, best_weight = 0;
	__u32 any_idx = 0;
	bool any_active = false;
	__u32 *weight_p;
	int i;

	for (i = 0; i < MPTCP_SUBFLOWS_MAX; i++) {
		__u32 key = i;

		subflow = bpf_mptcp_subflow_ctx_by_pos(data, i);
		if (!subflow)
			continue;
		if (!mptcp_subflow_active(subflow))
			continue;

		if (!any_active) {
			any_active = true;
			any_idx = i;
		}

		weight_p = bpf_map_lookup_elem(&ratan_path_weights, &key);
		if (!weight_p)
			continue;

		if (*weight_p > best_weight) {
			best_weight = *weight_p;
			best_idx = i;
		}
	}

	if (best_weight > 0) {
		subflow = bpf_mptcp_subflow_ctx_by_pos(data, best_idx);
		if (subflow)
			mptcp_subflow_set_scheduled(subflow, true);
		return 0;
	}

	if (any_active) {
		subflow = bpf_mptcp_subflow_ctx_by_pos(data, any_idx);
		if (subflow)
			mptcp_subflow_set_scheduled(subflow, true);
		return 0;
	}

	return -1;
}

SEC(".struct_ops.link")
struct mptcp_sched_ops ratan = {
	.init		= (void *)mptcp_sched_ratan_init,
	.release	= (void *)mptcp_sched_ratan_release,
	.get_subflow	= (void *)mptcp_sched_ratan_get_subflow,
	.name		= "ratan",
};
