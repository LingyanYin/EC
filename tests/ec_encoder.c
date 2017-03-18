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

struct encoder_context {
	struct ibv_context	*context;
	struct ibv_pd		*pd;
	struct ec_context	*ec_ctx;
	int			infd;
	int			outfd_sw;
	int			outfd_verbs;
	int			outfd_eco;
};

static void close_io_files(struct encoder_context *ctx)
{
	close(ctx->outfd_sw);
	close(ctx->outfd_verbs);
	close(ctx->outfd_eco);
	close(ctx->infd);
}

static int open_io_files(struct inargs *in, struct encoder_context *ctx)
{
	char *outfile;
	int err = 0;

	ctx->infd = open(in->datafile, O_RDONLY);
	if (ctx->infd < 0) {
		err_log("Failed to open file\n");
		return -EIO;
	}

	outfile = calloc(1, strlen(in->datafile) + strlen(".encode.code.verbs") + 1);
	if (!outfile) {
		err_log("Failed to alloc outfile\n");
		err = -ENOMEM;
		goto close_infd;
	}

	outfile = strcat(outfile, in->datafile);
	outfile = strcat(outfile, ".encode.code.verbs");
	unlink(outfile);
	ctx->outfd_verbs = open(outfile, O_RDWR | O_CREAT, 0666);
	if (ctx->outfd_verbs < 0) {
		err_log("Failed to open verbs code file");
		free(outfile);
		err = -EIO;
		goto close_infd;
	}
	free(outfile);

	outfile = calloc(1, strlen(in->datafile) + strlen(".encode.code.sw") + 1);
	if (!outfile) {
		err_log("Failed to alloc outfile\n");
		err = -ENOMEM;
		goto close_infd;
	}

	outfile = strcat(outfile, in->datafile);
	outfile = strcat(outfile, ".encode.code.sw");
	unlink(outfile);
	ctx->outfd_sw = open(outfile, O_RDWR | O_CREAT, 0666);
	if (ctx->outfd_sw < 0) {
		err_log("Failed to open sw code file");
		free(outfile);
		err = -EIO;
		goto close_infd;
	}
	free(outfile);

	outfile = calloc(1, strlen(in->datafile) + strlen(".encode.code.eco") + 1);
	if (!outfile) {
		err_log("Failed to alloc outfile\n");
		err = -ENOMEM;
		goto close_infd;
	}

	outfile = strcat(outfile, in->datafile);
	outfile = strcat(outfile, ".encode.code.eco");
	unlink(outfile);
	ctx->outfd_eco = open(outfile, O_RDWR | O_CREAT, 0666);
	if (ctx->outfd_eco < 0) {
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

	err = open_io_files(in, ctx);
	if (err)
		goto free_ec;

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

static int encode_file(struct encoder_context *ctx, struct eco_encoder *lib_encoder)
{
	struct ec_context *ec_ctx = ctx->ec_ctx;
	int bytes;
	int err;

	while (1) {
		bytes = read(ctx->infd, ec_ctx->data.buf,
				ec_ctx->block_size * ec_ctx->attr.k);
		if (bytes <= 0)
			break;

		err = ibv_exp_ec_encode_sync(ec_ctx->calc, &ec_ctx->mem);
		if (err) {
			err_log("Failed ibv_exp_ec_encode (%d)\n", err);
			return err;
		}

		bytes = write(ctx->outfd_verbs, ec_ctx->code.buf,
				ec_ctx->block_size * ec_ctx->attr.m);
		if (bytes < (int)ec_ctx->block_size * ec_ctx->attr.m) {
			err_log("Failed write to fd1 (%d)\n", err);
			return err;
		}

		memset(ec_ctx->code.buf, 0, ec_ctx->block_size * ec_ctx->attr.m);
		err = sw_ec_encode(ec_ctx);
		if (err) {
			err_log("Failed sw_ec_encode (%d)\n", err);
			return err;
		}

		bytes = write(ctx->outfd_sw, ec_ctx->code.buf,
				ec_ctx->block_size * ec_ctx->attr.m);
		if (bytes < (int)ec_ctx->block_size * ec_ctx->attr.m) {
			err_log("Failed write to fd2 (%d)\n", err);
			return err;
		}

		// library encode
		memset(ec_ctx->code.buf, 0, ec_ctx->block_size * ec_ctx->attr.m);
		err = mlx_eco_encoder_encode(lib_encoder, ec_ctx->data_arr, ec_ctx->code_arr, ec_ctx->attr.k, ec_ctx->attr.m, ec_ctx->block_size, 0);
		bytes = write(ctx->outfd_eco, ec_ctx->code.buf,
				ec_ctx->block_size * ec_ctx->attr.m);
		if (bytes < (int)ec_ctx->block_size * ec_ctx->attr.m) {
			err_log("Failed write to library fd (%d)\n", err);
			return err;
		}
		// done
		memset(ec_ctx->data.buf, 0, ec_ctx->block_size * ec_ctx->attr.k);
		memset(ec_ctx->code.buf, 0, ec_ctx->block_size * ec_ctx->attr.m);
	}

	return 0;
}

static void set_buffers(struct encoder_context *ctx)
{
	struct ec_context *ec_ctx = ctx->ec_ctx;
	int i;

	ec_ctx->data_arr = calloc(ec_ctx->attr.k, sizeof(*ec_ctx->data_arr));
	ec_ctx->code_arr = calloc(ec_ctx->attr.m, sizeof(*ec_ctx->code_arr));

	for (i = 0; i < ec_ctx->attr.k ; i++) {
		ec_ctx->data_arr[i] = ec_ctx->data.buf + i * ec_ctx->block_size;
	}
	for (i = 0; i < ec_ctx->attr.m ; i++) {
		ec_ctx->code_arr[i] = ec_ctx->code.buf + i * ec_ctx->block_size;
	}
}


int main(int argc, char *argv[])
{
	struct encoder_context *ctx;
	struct ibv_device *device;
	struct inargs in;
	int err;

	err = process_inargs(argc, argv, &in);
	if (err)
		return err;

	device = find_device(in.devname);
	if (!device)
		return -EINVAL;

	ctx = init_ctx(device, &in);
	if (!ctx)
		return -ENOMEM;

	set_buffers(ctx);

	// library init
	struct eco_encoder *lib_encoder = mlx_eco_encoder_init(in.k, in.m, 1);	
	if (!lib_encoder) {
		err_log("mlx_eco_encoder_init failed\n");
		return -ENOMEM;
	}

	// register buffers
	err = mlx_eco_encoder_register(lib_encoder, ctx->ec_ctx->data_arr, ctx->ec_ctx->code_arr, in.k, in.m, ctx->ec_ctx->block_size, 0);
	if (err) {
		err_log("mlx_ec_register_mrs failed to register\n");
		return -ENOMEM;
	}

	// encode data
	err = encode_file(ctx, lib_encoder);
	if (err)
		err_log("failed to encode file %s\n", in.datafile);


	mlx_eco_encoder_release(lib_encoder);

	free(ctx->ec_ctx->data_arr);
	free(ctx->ec_ctx->code_arr);

	close_ctx(ctx);

	return 0;
}
