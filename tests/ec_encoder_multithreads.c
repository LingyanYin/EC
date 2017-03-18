/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
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
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "ec_common.h"
#include <eco_encoder.h>
#include <pthread.h>
#include "../util/atomics.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

struct encoder_context {
	struct ibv_context	*context;
	struct ibv_pd		*pd;
	struct ec_context	*ec_ctx;
	int			infd;
	int			outfd_sw[NUM_THR];
	int			outfd_verbs[NUM_THR];
	int			outfd_eco[NUM_THR];
};


struct encoder_context *ctx;
struct ibv_device *device;
struct inargs in;
struct eco_encoder *lib_encoder;

static void close_io_files(struct encoder_context *ctx)
{
	int i;
	for (i=0; i<NUM_THR; i++) {
		close(ctx->outfd_sw[i]);
		close(ctx->outfd_verbs[i]);
		close(ctx->outfd_eco[i]);
	}
	close(ctx->infd);
}

static int open_io_files(struct inargs *in, struct encoder_context *ctx, int mytid)
{
	char *outfile;
	int err = 0;

	ctx->infd = open(in->datafile, O_RDONLY);
	if (ctx->infd < 0) {
		err_log("Failed to open file\n");
		return -EIO;
	}

	outfile = calloc(1, strlen(in->datafile) + strlen(".encode.code.verbs") + 5 + 1);
	if (!outfile) {
		err_log("Failed to alloc outfile\n");
		err = -ENOMEM;
		goto close_infd;
	}

	char tid[4];
	memset(tid, 0, sizeof(char)*4);
	//itoa(mytid, tid, 10);
	snprintf(tid, 4,"%d",mytid);
	outfile = strcat(outfile, in->datafile);
	outfile = strcat(outfile, ".");
	outfile = strcat(outfile, tid);
	outfile = strcat(outfile, ".encode.code.verbs");
	unlink(outfile);
	ctx->outfd_verbs[mytid] = open(outfile, O_RDWR | O_CREAT, 0666);
	if (ctx->outfd_verbs[mytid] < 0) {
		err_log("Failed to open verbs code file");
		free(outfile);
		err = -EIO;
		goto close_infd;
	}
	free(outfile);

	outfile = calloc(1, strlen(in->datafile) + strlen(".encode.code.sw") + 5 + 1);
	if (!outfile) {
		err_log("Failed to alloc outfile\n");
		err = -ENOMEM;
		goto close_infd;
	}

	outfile = strcat(outfile, in->datafile);
	outfile = strcat(outfile, ".");
	outfile = strcat(outfile, tid);
	outfile = strcat(outfile, ".encode.code.sw");
	unlink(outfile);
	ctx->outfd_sw[mytid] = open(outfile, O_RDWR | O_CREAT, 0666);
	if (ctx->outfd_sw[mytid] < 0) {
		err_log("Failed to open sw code file");
		free(outfile);
		err = -EIO;
		goto close_infd;
	}
	free(outfile);

	outfile = calloc(1, strlen(in->datafile) + strlen(".encode.code.eco") + 5 + 1);
	if (!outfile) {
		err_log("Failed to alloc outfile\n");
		err = -ENOMEM;
		goto close_infd;
	}

	outfile = strcat(outfile, in->datafile);
	outfile = strcat(outfile, ".");
	outfile = strcat(outfile, tid);
	outfile = strcat(outfile, ".encode.code.eco");
	unlink(outfile);
	ctx->outfd_eco[mytid] = open(outfile, O_RDWR | O_CREAT, 0666);
	if (ctx->outfd_eco[mytid] < 0) {
		err_log("Failed to open eco code file");
		free(outfile);
		err = -EIO;
		goto close_infd;
	}
	free(outfile);

	return 0;

close_infd:
	close(ctx->infd);

	return err;
}

static struct encoder_context *
init_ctx(struct ibv_device *ib_dev, struct inargs *in)
{
	struct encoder_context *ctx;
	int err;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		err_log("Failed to allocate encoder context\n");
		return NULL;
	}

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		err_log("Couldn't get context for %s\n",
				ibv_get_device_name(ib_dev));
		goto free_ctx;
	}

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		err_log("Failed to allocate PD\n");
		goto close_device;
	}

	ctx->ec_ctx = alloc_ec_ctx(ctx->pd, in->frame_size,
			in->k, in->m, in->w, 1, NULL);
	if (!ctx->ec_ctx) {
		err_log("Failed to allocate EC context\n");
		goto dealloc_pd;
	}

	int i;
	for (i=0; i<NUM_THR; i++) {
		err = open_io_files(in, ctx, i);
		if (err)
			goto free_ec;
	}

	return ctx;

free_ec:
        free_ec_ctx(ctx->ec_ctx);
dealloc_pd:
	ibv_dealloc_pd(ctx->pd);
close_device:
	ibv_close_device(ctx->context);
free_ctx:
	free(ctx);

	return NULL;
}

static void close_ctx(struct encoder_context *ctx)
{
	free_ec_ctx(ctx->ec_ctx);
	ibv_dealloc_pd(ctx->pd);

	if (ibv_close_device(ctx->context))
		err_log("Couldn't release context\n");


	close_io_files(ctx);
	free(ctx);
}

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s            start EC encoder\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -i, --ib-dev=<dev>         use IB device <dev> (default first device found)\n");
	printf("  -k, --data_blocks=<blocks> Number of data blocks\n");
	printf("  -m, --code_blocks=<blocks> Number of code blocks\n");
	printf("  -w, --gf=<gf>              Galois field GF(2^w)\n");
	printf("  -D, --datafile=<name>      Name of input file to encode\n");
	printf("  -s, --frame_size=<size>    size of EC frame\n");
	printf("  -d, --debug                print debug messages\n");
	printf("  -v, --verbose              add verbosity\n");
	printf("  -h, --help                 display this output\n");
}

static int process_inargs(int argc, char *argv[], struct inargs *in)
{
	int err;
	struct option long_options[11] = {
			{ .name = "ib-dev",        .has_arg = 1, .val = 'i' },
			{ .name = "datafile",      .has_arg = 1, .val = 'D' },
			{ .name = "frame_size",    .has_arg = 1, .val = 's' },
			{ .name = "data_blocks",   .has_arg = 1, .val = 'k' },
			{ .name = "code_blocks",   .has_arg = 1, .val = 'm' },
			{ .name = "gf",            .has_arg = 1, .val = 'w' },
			{ .name = "debug",         .has_arg = 0, .val = 'd' },
			{ .name = "verbose",       .has_arg = 0, .val = 'v' },
			{ .name = "help",          .has_arg = 0, .val = 'h' },
			{ .name = 0, .has_arg = 0, .val = 0 }
	};

	err = common_process_inargs(argc, argv, "i:D:E:s:k:m:w:hdv",
			long_options, in, usage);
	if (err)
		return err;

	if (in->datafile == NULL) {
		err_log("No input datafile was given\n");
		return -EINVAL;
	}

	if (in->frame_size <= 0) {
		err_log("No frame_size given %d\n", in->frame_size);
		return -EINVAL;
	}

	return 0;
}

static int encode_file(struct encoder_context *ctx, struct eco_encoder *lib_encoder, int mytid)
{
	struct ec_context *ec_ctx = ctx->ec_ctx;
	int bytes;
	int err;

	while (1) {
		bytes = read(ctx->infd, ec_ctx->data[mytid].buf,
				ec_ctx->block_size[mytid] * ec_ctx->attr.k);
		if (bytes <= 0)
			break;

		err = ibv_exp_ec_encode_sync(ec_ctx->calc, &ec_ctx->mem[mytid]);
		if (err) {
			err_log("Failed ibv_exp_ec_encode (%d)\n", err);
			return err;
		}

		bytes = write(ctx->outfd_verbs[mytid], ec_ctx->code[mytid].buf,
				ec_ctx->block_size[mytid] * ec_ctx->attr.m);
		if (bytes < (int)ec_ctx->block_size[mytid] * ec_ctx->attr.m) {
			err_log("Failed write to fd1 (%d)\n", err);
			return err;
		}

		memset(ec_ctx->code[mytid].buf, 0, ec_ctx->block_size[mytid] * ec_ctx->attr.m);
		err = sw_ec_encode(ec_ctx, mytid);
		if (err) {
			err_log("Failed sw_ec_encode (%d)\n", err);
			return err;
		}

		bytes = write(ctx->outfd_sw[mytid], ec_ctx->code[mytid].buf,
				ec_ctx->block_size[mytid] * ec_ctx->attr.m);
		if (bytes < (int)ec_ctx->block_size[mytid] * ec_ctx->attr.m) {
			err_log("Failed write to fd2 (%d)\n", err);
			return err;
		}

		// library encode
		memset(ec_ctx->code[mytid].buf, 0, ec_ctx->block_size[mytid] * ec_ctx->attr.m);
		err = mlx_eco_encoder_encode(lib_encoder, ec_ctx->data_arr[mytid], ec_ctx->code_arr[mytid], ec_ctx->attr.k, ec_ctx->attr.m, ec_ctx->block_size[mytid], mytid);
		bytes = write(ctx->outfd_eco[mytid], ec_ctx->code[mytid].buf,
				ec_ctx->block_size[mytid] * ec_ctx->attr.m);
		if (bytes < (int)ec_ctx->block_size[mytid] * ec_ctx->attr.m) {
			err_log("Failed write to library fd (%d)\n", err);
			return err;
		}
		// done
		memset(ec_ctx->data[mytid].buf, 0, ec_ctx->block_size[mytid] * ec_ctx->attr.k);
		memset(ec_ctx->code[mytid].buf, 0, ec_ctx->block_size[mytid] * ec_ctx->attr.m);
	}

	return 0;
}

static void set_buffers(struct encoder_context *ctx, int mytid)
{
	struct ec_context *ec_ctx = ctx->ec_ctx;
	int i;

	ec_ctx->data_arr[mytid] = calloc(ec_ctx->attr.k, sizeof(*ec_ctx->data_arr[mytid]));
	ec_ctx->code_arr[mytid] = calloc(ec_ctx->attr.m, sizeof(*ec_ctx->code_arr[mytid]));

	for (i = 0; i < ec_ctx->attr.k ; i++) {
		ec_ctx->data_arr[mytid][i] = ec_ctx->data[mytid].buf + i * ec_ctx->block_size[mytid];
	}
	for (i = 0; i < ec_ctx->attr.m ; i++) {
		ec_ctx->code_arr[mytid][i] = ec_ctx->code[mytid].buf + i * ec_ctx->block_size[mytid];
	}
}

void* encode_call(void* threadid){
	int* tid = (int*)threadid;
	int mytid = *tid;
	printf("mytid: %d\n", mytid);
	// register buffers
	int err = mlx_eco_encoder_register(lib_encoder, ctx->ec_ctx->data_arr[mytid], ctx->ec_ctx->code_arr[mytid], in.k, in.m, ctx->ec_ctx->block_size[mytid], mytid);
	if (err) {
		err_log("mlx_ec_register_mrs failed to register\n");
		exit(-1);
	}

	// encode data
	err = encode_file(ctx, lib_encoder, mytid);
	if (err)
		err_log("failed to encode file %s\n", in.datafile);
	return NULL;
}

int main(int argc, char *argv[])
{
	int err, i;

	err = process_inargs(argc, argv, &in);
	if (err)
		return err;

	device = find_device(in.devname);
	if (!device)
		return -EINVAL;

	ctx = init_ctx(device, &in);
	if (!ctx)
		return -ENOMEM;

	for (i=0; i<NUM_THR; i++) {
		set_buffers(ctx, i);
	}

	// library init
	lib_encoder = mlx_eco_encoder_init(in.k, in.m, 1);	
	if (!lib_encoder) {
		err_log("mlx_eco_encoder_init failed\n");
		return -ENOMEM;
	}

	printf("before pthread create\n");
    /**
     * pthread begining here
     */
    pthread_t threads[NUM_THR];
    pthread_attr_t attr;
    void *status;
    int rt;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	int threadid[NUM_THR];
	for (i=0; i<NUM_THR; i++) {
		threadid[i] = i;
		printf("threadid: %d\n", threadid[i]);
		rt = pthread_create(&threads[i], &attr, encode_call, (void*) &threadid[i]);
		if (rt) {
			perror("pthread_create error");
			exit(-1);
		}
	}
	//int mytid = atomic_increment(&threadid, 1);
	//printf("threadid: %d\n", mytid);
	
	for (i=0; i<NUM_THR; i++) {
		rt = pthread_join(threads[i], &status);
		if (rt) {
			perror("pthread_join error");
			exit(-1);
		}
	}
	pthread_attr_destroy(&attr);

	mlx_eco_encoder_release(lib_encoder);

	for (i=0; i<NUM_THR; i++) {
		free(ctx->ec_ctx->data_arr[i]);
		free(ctx->ec_ctx->code_arr[i]);
	}

	close_ctx(ctx);

	return 0;
}
