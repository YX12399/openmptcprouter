// SPDX-License-Identifier: GPL-2.0
/*
 * ratan_loader.c — one-shot libbpf loader for the RATAN MPTCP scheduler.
 *
 * Loads ratan_sched.bpf.o, pins ratan_path_weights at
 * /sys/fs/bpf/ratan/path_weights, attaches the struct_ops, pins the link
 * at /sys/fs/bpf/ratan/sched_link, and exits. The pins hold the kernel
 * references so the scheduler stays registered after this process exits.
 *
 *   ratan_loader [/path/to/ratan_sched.bpf.o]   # load + pin
 *   ratan_loader --unload                        # remove pins
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#define PIN_DIR			"/sys/fs/bpf/ratan"
#define PIN_LINK		PIN_DIR "/sched_link"
#define PIN_WEIGHTS		PIN_DIR "/path_weights"
#define BPF_OBJ_DEFAULT		"/usr/local/lib/bpf/ratan_sched.bpf.o"

static int libbpf_print(enum libbpf_print_level lvl, const char *fmt, va_list ap)
{
	(void)lvl;
	return vfprintf(stderr, fmt, ap);
}

static void ensure_bpffs(void)
{
	struct stat st;

	if (stat("/sys/fs/bpf", &st) != 0)
		mkdir("/sys/fs/bpf", 0755);

	/* If /sys/fs/bpf isn't a bpffs mount, mount it. systemd-based distros
	 * already do this; OpenWrt may not. */
	FILE *f = fopen("/proc/mounts", "r");
	if (f) {
		char line[512];
		int mounted = 0;
		while (fgets(line, sizeof(line), f)) {
			if (strstr(line, " /sys/fs/bpf ") && strstr(line, "bpf")) {
				mounted = 1;
				break;
			}
		}
		fclose(f);
		if (!mounted)
			mount("bpf", "/sys/fs/bpf", "bpf", 0, NULL);
	}

	mkdir(PIN_DIR, 0755);
}

static int do_unload(void)
{
	int rc = 0;
	if (unlink(PIN_LINK) && errno != ENOENT) {
		fprintf(stderr, "unlink %s: %s\n", PIN_LINK, strerror(errno));
		rc = 1;
	}
	if (unlink(PIN_WEIGHTS) && errno != ENOENT) {
		fprintf(stderr, "unlink %s: %s\n", PIN_WEIGHTS, strerror(errno));
		rc = 1;
	}
	rmdir(PIN_DIR);
	return rc;
}

int main(int argc, char **argv)
{
	const char *obj_path = BPF_OBJ_DEFAULT;
	struct bpf_object *obj;
	struct bpf_map *map, *ops_map;
	struct bpf_link *link;
	int err;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--unload"))
			return do_unload();
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			fprintf(stderr, "usage: %s [/path/to/ratan_sched.bpf.o]\n"
					"       %s --unload\n",
				argv[0], argv[0]);
			return 0;
		}
		obj_path = argv[i];
	}

	libbpf_set_print(libbpf_print);

	/* Older kernels gate BPF map creation on RLIMIT_MEMLOCK; harmless on
	 * cgroupv2 + bpf-token systems. */
	struct rlimit r = { RLIM_INFINITY, RLIM_INFINITY };
	setrlimit(RLIMIT_MEMLOCK, &r);

	ensure_bpffs();

	obj = bpf_object__open_file(obj_path, NULL);
	if (!obj) {
		fprintf(stderr, "open %s: %s\n", obj_path, strerror(errno));
		return 1;
	}

	map = bpf_object__find_map_by_name(obj, "ratan_path_weights");
	if (!map) {
		fprintf(stderr, "map ratan_path_weights not found in object\n");
		bpf_object__close(obj);
		return 1;
	}
	err = bpf_map__set_pin_path(map, PIN_WEIGHTS);
	if (err) {
		fprintf(stderr, "set_pin_path %s: %s\n", PIN_WEIGHTS, strerror(-err));
		bpf_object__close(obj);
		return 1;
	}

	err = bpf_object__load(obj);
	if (err) {
		fprintf(stderr, "bpf_object__load: %s (need kernel >=6.5 with MPTCP BPF struct_ops)\n",
			strerror(-err));
		bpf_object__close(obj);
		return 1;
	}

	ops_map = bpf_object__find_map_by_name(obj, "ratan");
	if (!ops_map) {
		fprintf(stderr, "struct_ops 'ratan' not found in object\n");
		bpf_object__close(obj);
		return 1;
	}

	link = bpf_map__attach_struct_ops(ops_map);
	if (!link) {
		fprintf(stderr, "attach_struct_ops: %s\n", strerror(errno));
		bpf_object__close(obj);
		return 1;
	}

	err = bpf_link__pin(link, PIN_LINK);
	if (err) {
		fprintf(stderr, "pin link %s: %s\n", PIN_LINK, strerror(-err));
		bpf_link__destroy(link);
		bpf_object__close(obj);
		return 1;
	}

	fprintf(stderr, "ratan_sched: loaded, weights at %s, link at %s\n",
		PIN_WEIGHTS, PIN_LINK);
	fprintf(stderr, "ratan_sched: activate per-namespace with: echo ratan > /proc/sys/net/mptcp/scheduler\n");

	/* Pins hold kernel refs; we can exit cleanly. */
	bpf_object__close(obj);
	return 0;
}
