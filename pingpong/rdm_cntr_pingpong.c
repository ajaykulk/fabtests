/*
 * Copyright (c) 2013-2014 Intel Corporation.  All rights reserved.
 *
 * This software is available to you under the BSD license
 * below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AWV
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <netdb.h>
#include <unistd.h>

#include <rdma/fabric.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <shared.h>

static struct cs_opts opts;
static int max_credits = 128;
static int credits = 128;
static int send_count = 0;
static int recv_outs = 0;	/* Outstanding recvs */
static char test_name[10] = "custom";
static struct timespec start, end;
static void *buf;
static size_t buffer_size;

static struct fi_info *fi, *hints;

static struct fid_fabric *fab;
static struct fid_domain *dom;
static struct fid_ep *ep;
static struct fid_cntr *rcntr, *scntr;
static struct fid_av *av;
static struct fid_mr *mr;
static void *local_addr, *remote_addr;
static size_t addrlen = 0;
static fi_addr_t remote_fi_addr;
struct fi_context fi_ctx_send;
struct fi_context fi_ctx_recv;
struct fi_context fi_ctx_av;

static int get_send_completions()
{
	int ret;

	ret = fi_cntr_wait(scntr, send_count, -1);
	if (ret < 0) {
		FT_PRINTERR("fi_cntr_wait", ret);
		return ret;
	}

	credits = max_credits;

	return ret;
}

static int send_xfer(int size)
{
	int ret;

	if (!credits) {
		ret = fi_cntr_wait(scntr, send_count, -1);
		if (ret < 0) {
			FT_PRINTERR("fi_cntr_wait", ret);
			return ret;
		}
	}

	credits--;
	ret = fi_send(ep, buf, (size_t) size, fi_mr_desc(mr), remote_fi_addr, 
			&fi_ctx_send);
	if (ret) {
		FT_PRINTERR("fi_send", ret);
		return ret;
	}
	send_count++;

	return ret;
}

static int recv_xfer(int size)
{
	int ret;

	ret = fi_cntr_wait(rcntr, recv_outs, -1);
	if (ret < 0) {
		FT_PRINTERR("fi_cntr_wait", ret);
		return ret;
	}

	ret = fi_recv(ep, buf, buffer_size, fi_mr_desc(mr), remote_fi_addr, 
			&fi_ctx_recv);
	if (ret)
		FT_PRINTERR("fi_recv", ret);
	recv_outs++;

	return ret;
}

static int send_msg(int size)
{
	int ret;

	ret = fi_send(ep, buf, (size_t) size, fi_mr_desc(mr), remote_fi_addr,
			&fi_ctx_send);
	if (ret) {
		FT_PRINTERR("fi_send", ret);
		return ret;
	}
	send_count++;

	ret = fi_cntr_wait(scntr, send_count, -1);
	if (ret < 0) {
		FT_PRINTERR("fi_cntr_wait", ret);
	}

	return ret;
}

static int recv_msg(void) 
{
	int ret;

	ret = fi_recv(ep, buf, buffer_size, fi_mr_desc(mr), 0, &fi_ctx_recv);
	if (ret) {
		FT_PRINTERR("fi_recv", ret);
		return ret;
	}
	recv_outs++;

	ret = fi_cntr_wait(rcntr, recv_outs, -1);
	if (ret < 0) {
		FT_PRINTERR("fi_cntr_wait", ret);
		return ret;
	}

	return ret;
}

static int sync_test(void)
{
	int ret;

	ret = get_send_completions();
	if (ret)
		return ret;

	ret = opts.dst_addr ? send_xfer(16) : recv_xfer(16);
	if (ret)
		return ret;

	return opts.dst_addr ? recv_xfer(16) : send_xfer(16);
}

static int run_test(void)
{
	int ret, i;

	ret = sync_test();
	if (ret)
		goto out;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (i = 0; i < opts.iterations; i++) {
		ret = opts.dst_addr ? send_xfer(opts.transfer_size) :
				 recv_xfer(opts.transfer_size);
		if (ret)
			goto out;

		ret = opts.dst_addr ? recv_xfer(opts.transfer_size) :
				 send_xfer(opts.transfer_size);
		if (ret)
			goto out;
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	if (opts.machr)
		show_perf_mr(opts.transfer_size, opts.iterations, &start, &end, 2, opts.argc, opts.argv);
	else
		show_perf(test_name, opts.transfer_size, opts.iterations, &start, &end, 2);

	ret = 0;

out:
	return ret;
}

static void free_ep_res(void)
{
	fi_close(&ep->fid);
	fi_close(&av->fid);
	fi_close(&mr->fid);
	fi_close(&rcntr->fid);
	fi_close(&scntr->fid);
	free(buf);
}

static int alloc_ep_res(struct fi_info *fi)
{
	struct fi_cntr_attr cntr_attr;
	struct fi_av_attr av_attr;
	int ret;

	buffer_size = opts.user_options & FT_OPT_SIZE ?
			opts.transfer_size : test_size[TEST_CNT - 1].size;
	buf = malloc(buffer_size);
	if (!buf) {
		perror("malloc");
		return -1;
	}

	memset(&cntr_attr, 0, sizeof cntr_attr);
	cntr_attr.events = FI_CNTR_EVENTS_COMP;

	ret = fi_cntr_open(dom, &cntr_attr, &scntr, NULL);
	if (ret) {
		FT_PRINTERR("fi_cntr_open", ret);
		goto err1;
	}

	ret = fi_cntr_open(dom, &cntr_attr, &rcntr, NULL);
	if (ret) {
		FT_PRINTERR("fi_cntr_open", ret);
		goto err2;
	}

	ret = fi_mr_reg(dom, buf, buffer_size, 0, 0, 0, 0, &mr, NULL);
	if (ret) {
		FT_PRINTERR("fi_mr_reg", ret);
		goto err3;
	}

	memset(&av_attr, 0, sizeof av_attr);
	av_attr.type = fi->domain_attr->av_type ?
			fi->domain_attr->av_type : FI_AV_MAP;
	av_attr.count = 1;
	av_attr.name = NULL;

	ret = fi_av_open(dom, &av_attr, &av, NULL);
	if (ret) {
		FT_PRINTERR("fi_av_open", ret);
		goto err4;
	}

	ret = fi_endpoint(dom, fi, &ep, NULL);
	if (ret) {
		FT_PRINTERR("fi_endpoint", ret);
		goto err5;
	}

	return 0;

err5:
	fi_close(&av->fid);
err4:
	fi_close(&mr->fid);
err3:
	fi_close(&rcntr->fid);
err2:
	fi_close(&scntr->fid);
err1:
	free(buf);
	return ret;
}

static int bind_ep_res(void)
{
	int ret;

	ret = fi_ep_bind(ep, &scntr->fid, FI_SEND);
	if (ret) {
		FT_PRINTERR("fi_ep_bind", ret);
		return ret;
	}

	ret = fi_ep_bind(ep, &rcntr->fid, FI_RECV);
	if (ret) {
		FT_PRINTERR("fi_ep_bind", ret);
		return ret;
	}

	ret = fi_ep_bind(ep, &av->fid, 0);
	if (ret) {
		FT_PRINTERR("fi_ep_bind", ret);
		return ret;
	}

	ret = fi_enable(ep);
	if (ret) {
		FT_PRINTERR("fi_enable", ret);
		return ret;
	}

	return ret;
}

static int init_fabric(void)
{
	uint64_t flags = 0;
	char *node, *service;
	int ret;

	if (opts.dst_addr) {
		ret = ft_getsrcaddr(opts.src_addr, opts.src_port, hints);
		if (ret)
			return ret;
		node = opts.dst_addr;
		service = opts.dst_port;
	} else {
		node = opts.src_addr;
		service = opts.src_port;
		flags = FI_SOURCE;
	}

	ret = fi_getinfo(FT_FIVERSION, node, service, flags, hints, &fi);
	if (ret) {
		FT_PRINTERR("fi_getinfo", ret);
		return ret;
	}

	/* Get remote address */
	if (opts.dst_addr) {
		addrlen = fi->dest_addrlen;
		remote_addr = malloc(addrlen);
		memcpy(remote_addr, fi->dest_addr, addrlen);
	}

	ret = fi_fabric(fi->fabric_attr, &fab, NULL);
	if (ret) {
		FT_PRINTERR("fi_fabric", ret);
		goto err0;
	}

	ret = fi_domain(fab, fi, &dom, NULL);
	if (ret) {
		FT_PRINTERR("fi_domain", ret);
		goto err1;
	}

	ret = alloc_ep_res(fi);
	if (ret)
		goto err3;

	ret = bind_ep_res();
	if (ret)
		goto err4;

	return 0;

err4:
	free_ep_res();
err3:
	fi_close(&dom->fid);
err1:
	fi_close(&fab->fid);
err0:
	return ret;
}

static int init_av(void)
{
	int ret;

	if (opts.dst_addr) {
		/* Get local address blob. Find the addrlen first. We set addrlen 
		 * as 0 and fi_getname will return the actual addrlen. */
		addrlen = 0;
		ret = fi_getname(&ep->fid, local_addr, &addrlen);
		if (ret != -FI_ETOOSMALL) {
			FT_PRINTERR("fi_getname", ret);
			return ret;
		}

		local_addr = malloc(addrlen);
		ret = fi_getname(&ep->fid, local_addr, &addrlen);
		if (ret) {
			FT_PRINTERR("fi_getname", ret);
			return ret;
		}

		ret = fi_av_insert(av, remote_addr, 1, &remote_fi_addr, 0, 
				&fi_ctx_av);
		if (ret != 1) {
			FT_PRINTERR("fi_av_insert", ret);
			return ret;
		}

		/* Send local addr size and local addr */
		memcpy(buf, &addrlen, sizeof(size_t));
		memcpy(buf + sizeof(size_t), local_addr, addrlen);
		ret = send_msg(sizeof(size_t) + addrlen);
		if (ret)
			return ret;

		/* Receive ACK from server */
		ret = recv_msg();
		if (ret)
			return ret;

	} else {
		/* Post a recv to get the remote address */
		ret = recv_msg();
		if (ret)
			return ret;

		memcpy(&addrlen, buf, sizeof(size_t));
		remote_addr = malloc(addrlen);
		memcpy(remote_addr, buf + sizeof(size_t), addrlen);

		ret = fi_av_insert(av, remote_addr, 1, &remote_fi_addr, 0, 
				&fi_ctx_av);
		if (ret != 1) {
			FT_PRINTERR("fi_av_insert", ret);
			return ret;
		}

		/* Send ACK */
		ret = send_msg(16);
		if (ret)
			return ret;
	}

	/* Post first recv */
	ret = fi_recv(ep, buf, buffer_size, fi_mr_desc(mr), remote_fi_addr,
			&fi_ctx_recv);
	if (ret)
		FT_PRINTERR("fi_recv", ret);
	recv_outs++;

	return ret;
}

static int run(void)
{
	int i, ret = 0;

	ret = init_fabric();
	if (ret)
		return ret;

	ret = init_av();
	if (ret)
		goto out;

	if (!(opts.user_options & FT_OPT_SIZE)) {
		for (i = 0; i < TEST_CNT; i++) {
			if (test_size[i].option > opts.size_option)
				continue;
			opts.transfer_size = test_size[i].size;
			init_test(&opts, test_name, sizeof(test_name));
			ret = run_test();
			if (ret)
				goto out;
		}
	} else {
		init_test(&opts, test_name, sizeof(test_name));
		ret = run_test();
		if (ret)
			goto out;
	}

	get_send_completions();
	/* TODO: need support for finalize operation to sync test */
out:
	free_ep_res();
	fi_close(&dom->fid);
	fi_close(&fab->fid);
	return ret;
}

int main(int argc, char **argv)
{
	int op, ret;
	opts = INIT_OPTS;

	hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	while ((op = getopt(argc, argv, "h" CS_OPTS INFO_OPTS)) != -1) {
		switch (op) {
		default:
			ft_parseinfo(op, optarg, hints);
			ft_parsecsopts(op, optarg, &opts);
			break;
		case '?':
		case 'h':
			ft_csusage(argv[0], "Ping pong client and server using counters.");
			return EXIT_FAILURE;
		}
	}

	if (optind < argc)
		opts.dst_addr = argv[optind];

	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_MSG;
	hints->mode = FI_CONTEXT | FI_LOCAL_MR;

	ret = run();

	fi_freeinfo(hints);
	fi_freeinfo(fi);
	return -ret;
}
