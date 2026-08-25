// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <linux/if_link.h> /* Need XDP flags */

#include "bpf/eth_type_filter.h"
#include "e2e_bpf.h"

#define MAX_ERRNO	4095
#define IS_ERR_VALUE(x) ((x) >= (unsigned long)-MAX_ERRNO)

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static struct bpf_object *bpf_obj = NULL;

static inline bool IS_ERR_OR_NULL(const void *ptr)
{
	return (!ptr) || IS_ERR_VALUE((unsigned long)ptr);
}

static inline long PTR_ERR(const void *ptr)
{
	return (long)ptr;
}

static int e2e_bpf_open_object(struct bpf_object **obj, const char *file,
			       int ifindex)
{
	struct bpf_program *prog, *first_prog = NULL;
	struct bpf_object *o;
	struct bpf_map *map;
	long err;

	o = bpf_object__open(file);
	if (IS_ERR_OR_NULL(o)) {
		err = -PTR_ERR(o);
		fprintf(stderr, "ERR: opening BPF-OBJ file(%s) (%ld): %s\n",
			file, err, strerror((int)-err));
		return (int)err;
	}

	bpf_object__for_each_program (prog, o) {
		bpf_program__set_type(prog, BPF_PROG_TYPE_XDP);
		bpf_program__set_ifindex(prog, ifindex);
		if (!first_prog)
			first_prog = prog;
	}

	bpf_object__for_each_map (map, o) {
		if (!bpf_map__type(map))
			bpf_map__set_ifindex(map, ifindex);
	}

	if (!first_prog) {
		fprintf(stderr, "ERR: file %s contains no programs\n", file);
		return -EINVAL;
	}

	*obj = o;
	return 0;
}

static int e2e_bpf_reuse_maps(struct bpf_object *obj, const char *path)
{
	struct bpf_map *map;

	if (!obj)
		return -ENOENT;

	if (!path)
		return -EINVAL;

	bpf_object__for_each_map (map, obj) {
		int len, err;
		int pinned_map_fd;
		char buf[PATH_MAX];

		len = snprintf(buf, PATH_MAX, "%s/%s", path,
			       bpf_map__name(map));
		if (len < 0)
			return -EINVAL;
		else if (len >= PATH_MAX)
			return -ENAMETOOLONG;

		pinned_map_fd = bpf_obj_get(buf);
		if (pinned_map_fd < 0)
			return pinned_map_fd;

		err = bpf_map__reuse_fd(map, pinned_map_fd);
		if (err)
			return err;
	}

	return 0;
}

static int e2e_bpf_load_object_file_reuse_maps(struct bpf_object **obj,
					       const char *file, int ifindex,
					       const char *pin_dir)
{
	struct bpf_object *o = NULL;
	int err;

	err = e2e_bpf_open_object(&o, file, ifindex);
	if (err) {
		fprintf(stderr, "ERR: failed to open object %s\n", file);
		return err;
	}

	err = e2e_bpf_reuse_maps(o, pin_dir);
	if (err) {
		fprintf(stderr,
			"ERR: failed to reuse maps for object %s, pin_dir=%s\n",
			file, pin_dir);
		return err;
	}

	err = bpf_object__load(o);
	if (err) {
		fprintf(stderr, "ERR: loading BPF-OBJ file(%s) (%d): %s\n",
			file, err, strerror(-err));
		return err;
	}

	*obj = o;
	return err;
}

int e2e_bpf_load_object_file(struct bpf_object **obj, const char *filename,
			     int ifindex)
{
	struct bpf_program *prog, *first_prog = NULL;
	struct bpf_object *o;
	struct bpf_map *map;
	int err;

	/*
	 * Set ifindex for hardware offloading XDP programs (note this sets
	 * libbpf bpf_program->prog_ifindex and foreach bpf_map->map_ifindex).
	 */
	o = bpf_object__open(filename);
	if (IS_ERR_OR_NULL(o)) {
		fprintf(stderr, "ERR: Failed to open %s\n", filename);
		return (int)-PTR_ERR(o);
	}

	bpf_object__for_each_program (prog, o) {
		bpf_program__set_type(prog, BPF_PROG_TYPE_XDP);
		bpf_program__set_ifindex(prog, ifindex);
		if (!first_prog)
			first_prog = prog;
	}

	bpf_object__for_each_map (map, o) {
		if (!bpf_map__type(map))
			bpf_map__set_ifindex(map, ifindex);
	}

	if (!first_prog) {
		fprintf(stderr, "ERR: file %s contains no programs\n", filename);
		return -EINVAL;
	}

	/*
	 * Use libbpf for extracting BPF byte-code from BPF-ELF object, and
	 * loading this into the kernel via bpf-syscall
	 */
	err = bpf_object__load(o);
	if (err) {
		fprintf(stderr, "ERR: loading BPF-OBJ file(%s) (%d): %s\n",
			filename, err, strerror(-err));
		return err;
	}

	/* Notice how a pointer to a libbpf bpf_object is returned */
	*obj = o;
	return 0;
}

static int e2e_bpf_link_attach(const struct e2e_bpf_cfg *cfg, int prog_fd)
{
	uint32_t xdp_flags = cfg->xdp_flags;
	int if_idx = cfg->if_idx;
	int err;

	/* libbpf provide the XDP net_device link-level hook attach helper */
	err = bpf_xdp_attach(if_idx, prog_fd, xdp_flags, NULL);
	if (err == -EEXIST && !(xdp_flags & XDP_FLAGS_UPDATE_IF_NOEXIST)) {
		/* Force mode didn't work, probably because a program of the
		 * opposite type is loaded. Let's unload that and try loading
		 * again.
		 */

		__u32 old_flags = xdp_flags;

		xdp_flags &= ~XDP_FLAGS_MODES;
		xdp_flags |= (old_flags & XDP_FLAGS_SKB_MODE) ?
				     XDP_FLAGS_DRV_MODE :
				     XDP_FLAGS_SKB_MODE;

		err = bpf_xdp_detach(if_idx, xdp_flags, NULL);
		if (!err)
			err = bpf_xdp_attach(if_idx, prog_fd, old_flags, NULL);
	}
	if (err < 0) {
		fprintf(stderr,
			"ERR: "
			"ifindex(%d) link set xdp fd failed (%d): %s\n",
			if_idx, -err, strerror(-err));

		switch (-err) {
		case EBUSY:
		case EEXIST:
			fprintf(stderr, "Hint: XDP already loaded on device"
					" use --force to swap/replace\n");
			break;
		case EOPNOTSUPP:
			fprintf(stderr, "Hint: Native-XDP not supported"
					" use --skb-mode or --auto-mode\n");
			break;
		default:
			break;
		}
		return err;
	}

	return 0;
}

static int e2e_bpf_link_detach(const struct e2e_bpf_cfg *cfg,
			       int expected_prog_id)
{
	int if_idx = cfg->if_idx;
	uint32_t curr_prog_id;
	int err;

	bpf_object__close(bpf_obj);

	err = bpf_xdp_query_id(if_idx, 0, &curr_prog_id);
	if (err) {
		if (cfg->verbose)
			fprintf(stderr,
				"ERR: get link xdp id failed (err=%d): %s\n",
				-err, strerror(-err));
		return err;
	}

	if (!curr_prog_id) {
		printf("INFO: %s() no curr XDP prog on ifindex:%d\n", __func__,
		       if_idx);
		return 0;
	}

	if (expected_prog_id && curr_prog_id != expected_prog_id) {
		fprintf(stderr,
			"ERR: %s() "
			"expected prog ID(%d) no match(%d), not removing\n",
			__func__, expected_prog_id, curr_prog_id);
		return 1;
	}

	err = bpf_xdp_detach(if_idx, cfg->xdp_flags, NULL);
	if (err < 0) {
		fprintf(stderr, "ERR: %s() link set xdp failed (err=%d): %s\n",
			__func__, err, strerror(-err));
		return err;
	}

	if (cfg->verbose)
		printf("INFO: %s() removed XDP prog ID:%d on ifindex:%d\n",
		       __func__, curr_prog_id, if_idx);

	return 0;
}

static int e2e_bpf_load_and_xdp_attach(struct e2e_bpf_cfg *cfg)
{
	struct bpf_program *bpf_prog;
	int offload_ifindex = 0;
	int prog_fd;
	int err;

	/* If flags indicate hardware offload, supply ifindex */
	if (cfg->xdp_flags & XDP_FLAGS_HW_MODE)
		offload_ifindex = cfg->if_idx;

	/* Load the BPF-ELF object file and get back libbpf bpf_object */
	if (cfg->reuse_maps)
		err = e2e_bpf_load_object_file_reuse_maps(
			&bpf_obj, cfg->filename, offload_ifindex, cfg->pin_dir);
	else
		err = e2e_bpf_load_object_file(&bpf_obj, cfg->filename,
					       offload_ifindex);

	if (err) {
		fprintf(stderr, "ERR: loading file: %s\n", cfg->filename);
		return err;
	}

	/*
	 * At this point: All XDP/BPF programs from the cfg->filename have been
	 * loaded into the kernel, and evaluated by the verifier. Only one of
	 * these gets attached to XDP hook, the others will get freed once this
	 * process exit.
	 */

	if (cfg->progname[0])
		/* Find a matching BPF prog section name */
		bpf_prog = bpf_object__find_program_by_name(bpf_obj,
							     cfg->progname);
	else
		/* Find the first program */
		bpf_prog = bpf_object__next_program(bpf_obj, NULL);

	if (!bpf_prog) {
		fprintf(stderr,
			"ERR: couldn't find a program with function name '%s'\n",
			cfg->progname);
		return -EINVAL;
	}

	snprintf(cfg->progname, sizeof(cfg->progname), "%s",
		 bpf_program__name(bpf_prog));

	prog_fd = bpf_program__fd(bpf_prog);
	if (prog_fd <= 0) {
		fprintf(stderr, "ERR: bpf_program__fd failed\n");
		return prog_fd;
	}

	/*
	 * At this point: BPF-progs are (only) loaded by the kernel, and prog_fd
	 * is our select file-descriptor handle. Next step is attaching this FD
	 * to a kernel hook point, in this case XDP net_device link-level hook.
	 */
	return e2e_bpf_link_attach(cfg, prog_fd);
}

int e2e_bpf_update_xsk_map(int idx, int sock_fd)
{
	struct bpf_map *map;
	int map_fd;
	int ret;

	/*
	 * Even in XDP mode the BPF program might not be loaded (not necessary
	 * for TX only). We do not need to update the map in this case. Simulate
	 * success
	 */
	if (!bpf_obj)
		return 0;

	map = bpf_object__find_map_by_name(bpf_obj, E2E_BPF_XSK_MAP_NAME);
	map_fd = bpf_map__fd(map);
	if (map_fd < 0) {
		fprintf(stderr, "ERROR: no xsks map found: %s\n",
			strerror(map_fd));
		return map_fd;
	}

	ret = bpf_map_update_elem(map_fd, &idx, &sock_fd, 0);
	if (ret) {
		fprintf(stderr, "ERROR: unable to update bpf map: %s\n",
			strerror(ret));
		return ret;
	}

	return 0;
}

static int checkMasterOffset()
{
	FILE *checksync;

	int32_t masterOffset = INT32_MAX;
	int unSyncCount = 0;
	int syncCount = 0;
	char response[65];
	char *offset;
	int foundptp = 0;
	int gotresponse = 0;
	do {
		sleep(1);
		checksync = popen("sudo pmc -u -b 0 'GET TIME_STATUS_NP'", "r");
		loop:while (fgets(response, sizeof(response), checksync) != NULL)
		{
		  gotresponse += 1;
		  //printf("response: %s", response);
		  // check for sync offset
		  if(strstr(response, "master_offset")) {
			foundptp = 1;
			offset = strtok(response, " ");
			offset = strtok(NULL, " ");
			masterOffset = atoi(offset);
		  }
		  // check for sync master
		  if(strstr(response, "gmPresent")) {
			if(strstr(response, "false")) {
			  printf("\033[0;31mWARNING! No sync master found! Either this node is sync master, or it is unsyncronized!\033[0m\n");
			  unSyncCount += 1;
			} else {
				syncCount += 1;
				printf("Master offset is: %d, waiting ...\033[0m\n", masterOffset);
			}
		  }
		}
		pclose(checksync);
		
		if(gotresponse == 0) {
			checksync = popen("pmc -u -b 0 'GET TIME_STATUS_NP'", "r"); //try to get pmc info again, without sudo this time
			gotresponse = 3; // avoid that we try again and again
			goto loop; //restart loop if pmc command needs to be executed without sudo
		}
		if(foundptp == 0) {
			printf("\033[0;31mWARNING! No running PTP instance found!\033[0m\n");
			unSyncCount += 1;
		}
	} while (((masterOffset > 10 || masterOffset < -10) || syncCount < 3) && unSyncCount < 15);
	return masterOffset;
}

int e2e_bpf_load(struct e2e_bpf_cfg *cfg)
{
	int ret;
	printf("loading bpf program...\n");

	ret = e2e_bpf_load_and_xdp_attach(cfg);
	if (ret)
		return ret;

	printf("bpf program loaded.\n");

	/* Wait until time synchronization is back */
	checkMasterOffset();

	return ret;
}

int e2e_bpf_unload(const struct e2e_bpf_cfg *cfg)
{
	return e2e_bpf_link_detach(cfg, 0);
}
