/*
 * Copyright (c) 2018, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the Intel Corporation nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include <string.h>
#include "fuzzer.h"
#include <uapi/ipc/topology.h>
#include <uapi/ipc/stream.h>
#include <uapi/ipc/control.h>
#include "qemu-bridge.h"
#include <uapi/ipc/trace.h>

int enable_fuzzer;

pthread_cond_t ipc_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t ipc_mutex = PTHREAD_MUTEX_INITIALIZER;

/* tplg message types */
uint32_t tplg_cmd_types[] = {SOF_IPC_TPLG_COMP_NEW,
		       SOF_IPC_TPLG_COMP_FREE,
		       SOF_IPC_TPLG_COMP_CONNECT,
		       SOF_IPC_TPLG_PIPE_NEW,
		       SOF_IPC_TPLG_PIPE_FREE,
		       SOF_IPC_TPLG_PIPE_CONNECT,
		       SOF_IPC_TPLG_PIPE_COMPLETE,
		       SOF_IPC_TPLG_BUFFER_NEW,
		       SOF_IPC_TPLG_BUFFER_FREE};

/* PM message types */
uint32_t pm_cmd_types[] = {SOF_IPC_PM_CTX_SAVE,
			  SOF_IPC_PM_CTX_RESTORE,
			  SOF_IPC_PM_CTX_SIZE,
			  SOF_IPC_PM_CLK_SET,
			  SOF_IPC_PM_CLK_GET,
			  SOF_IPC_PM_CLK_REQ,
			  SOF_IPC_PM_CORE_ENABLE};

uint32_t comp_cmd_types[] = {SOF_IPC_COMP_SET_VALUE,
			    SOF_IPC_COMP_GET_VALUE,
			    SOF_IPC_COMP_SET_DATA,
			    SOF_IPC_COMP_GET_DATA};


uint32_t dai_cmd_types[] = {SOF_IPC_DAI_CONFIG,
			   SOF_IPC_DAI_LOOPBACK};

uint32_t stream_cmd_types[] = {SOF_IPC_STREAM_PCM_PARAMS,
			    SOF_IPC_STREAM_PCM_PARAMS_REPLY,
			    SOF_IPC_STREAM_PCM_FREE,
			    SOF_IPC_STREAM_TRIG_START,
			    SOF_IPC_STREAM_TRIG_STOP,
			    SOF_IPC_STREAM_TRIG_PAUSE,
			    SOF_IPC_STREAM_TRIG_RELEASE,
			    SOF_IPC_STREAM_TRIG_DRAIN,
			    SOF_IPC_STREAM_TRIG_XRUN,
			    SOF_IPC_STREAM_POSITION,
			    SOF_IPC_STREAM_VORBIS_PARAMS,
			    SOF_IPC_STREAM_VORBIS_FREE};

uint32_t trace_cmd_types[] = {SOF_IPC_TRACE_DMA_PARAMS,
			     SOF_IPC_TRACE_DMA_POSITION};

/* list of supported target platforms */
static struct fuzz_platform *platform[] =
{
		&byt_platform,
		&cht_platform,
};

static void usage(char *name)
{
	int i;

	fprintf(stdout, "Usage 	%s -p platform <option(s)>\n", name);
	fprintf(stdout, "		-t topology file\n");
	fprintf(stdout, "		-p platform name\n");
	fprintf(stdout, "		-f (enable ipc fuzzing (optional))\n");
	fprintf(stdout, "		supported platforms: ");
	for (i = 0; i < ARRAY_SIZE(platform); i++) {
		fprintf(stdout, "%s ", platform[i]->name);
	}
	fprintf(stdout, "\n");

	exit(0);
}

static void ipc_dump(struct ipc_msg *msg)
{
	/* TODO: dump data here too */
	fprintf(stdout, "ipc: header 0x%x size %d reply %d\n",
			msg->header, msg->msg_size, msg->reply_size);
}

static void ipc_dump_err(struct ipc_msg *msg)
{
	/* TODO: dump data here too */
	fprintf(stderr, "ipc: header 0x%x size %d reply %d\n",
		msg->header, msg->msg_size, msg->reply_size);
}

void *fuzzer_create_io_region(struct fuzz *fuzzer, int id, int idx)
{
	struct fuzz_platform *plat = fuzzer->platform;
	struct fuzzer_reg_space *space;
	char shm_name[32];
	int err;
	void *ptr = NULL;

	space = &plat->reg_region[idx];

	sprintf(shm_name, "%s-io", space->name);

	err = qemu_io_register_shm(shm_name, id, space->desc.size, &ptr);
	if (err < 0)
		fprintf(stderr, "error: can't allocate IO %s:%d SHM %d\n", shm_name,
				id, err);

	return ptr;
}

void *fuzzer_create_memory_region(struct fuzz *fuzzer, int id, int idx)
{
	struct fuzz_platform *plat = fuzzer->platform;
	struct fuzzer_mem_desc *desc;
	char shm_name[32];
	int err;
	void *ptr = NULL;

	desc = &plat->mem_region[idx];

	/* shared via SHM (not shared on real HW) */
	sprintf(shm_name, "%s-mem", desc->name);
	err = qemu_io_register_shm(shm_name, id, desc->size, &ptr);
	if (err < 0)
		fprintf(stderr, "error: can't allocate %s:%d SHM %d\n", shm_name,
				id, err);

	return ptr;
}

void send_volume_command(struct fuzz *fuzzer, int volume)
{
	struct sof_ipc_ctrl_data *cdata;
	int i, ret;

	cdata = (struct sof_ipc_ctrl_data *)
		malloc(sizeof(struct sof_ipc_ctrl_data) +
		2 * sizeof(struct sof_ipc_ctrl_value_comp));

	cdata->num_elems = 2;
	cdata->rhdr.hdr.cmd = SOF_IPC_GLB_COMP_MSG | SOF_IPC_COMP_SET_VALUE;
	cdata->cmd = SOF_CTRL_CMD_VOLUME;
	cdata->type = SOF_CTRL_TYPE_VALUE_CHAN_SET;
	cdata->comp_id = 2;
	cdata->rhdr.hdr.size = sizeof(struct sof_ipc_ctrl_data) +
				2 * sizeof(struct sof_ipc_ctrl_value_comp);

	for (i = 0; i < 2; i++) {
		cdata->chanv[i].channel = i;
		cdata->chanv[i].value = volume;
	}

	/* configure fuzzer msg */
	fuzzer->msg.header = cdata->rhdr.hdr.cmd;
	memcpy(fuzzer->msg.msg_data, cdata, cdata->rhdr.hdr.size);
	fuzzer->msg.msg_size = cdata->rhdr.hdr.size;
	fuzzer->msg.reply_size = cdata->rhdr.hdr.size;

	/* load volume component */
	ret = fuzzer_send_msg(fuzzer);
	if (ret < 0)
		fprintf(stderr, "error: message tx failed\n");

	free(cdata);
}

/* frees all SHM and message queues */
void fuzzer_free_regions(struct fuzz *fuzzer)
{
	struct fuzz_platform *plat = fuzzer->platform;
    int i;

    for (i = 0; i < plat->num_mem_regions; i++)
        qemu_io_free_shm(i);

    for (i = 0; i < plat->num_reg_regions; i++)
    	 qemu_io_free_shm(i);

    qemu_io_free();
}

static fuzz_ipc_size(uint32_t *size)
{
	*size = rand() %  SOF_IPC_MSG_MAX_SIZE + 1;
}

/* fuzz the ipc commands */
static void fuzz_ipc_cmd(uint32_t *cmd)
{
	/* currently there are 10 % global IPC commands */
	uint32_t glb_cmd = rand() % 9 + 1;
	uint32_t cmd_type = 0;
	uint32_t count, index;

	switch (SOF_GLB_TYPE(glb_cmd)) {
	case SOF_IPC_GLB_TPLG_MSG:
		count = ARRAY_SIZE(tplg_cmd_types);
		index = rand() % count;
		cmd_type = tplg_cmd_types[index];
		break;
	case SOF_IPC_GLB_PM_MSG:
		count = ARRAY_SIZE(pm_cmd_types);
		index = rand() % count;
		cmd_type = pm_cmd_types[index];
		break;
	case SOF_IPC_GLB_COMP_MSG:
		count = ARRAY_SIZE(comp_cmd_types);
		index = rand() % count;
		cmd_type = comp_cmd_types[index];
		break;
	case SOF_IPC_GLB_STREAM_MSG:
		count = ARRAY_SIZE(stream_cmd_types);
		index = rand() % count;
		cmd_type = stream_cmd_types[index];
		break;
	default:
		break;
	}

	printf("glb %x type %x\n", SOF_GLB_TYPE(glb_cmd), cmd_type);

	if (cmd_type)
		*cmd = SOF_GLB_TYPE(glb_cmd) | cmd_type;
}

static void fuzz_ipc(struct ipc_msg *msg)
{
	struct sof_ipc_cmd_hdr *hdr = (struct sof_ipc_cmd_hdr *)msg->msg_data;

	/* fuzz cmd */
	fuzz_ipc_cmd(&hdr->cmd);
	msg->header = hdr->cmd;

	/* fuzz the size */
	fuzz_ipc_size(&hdr->size);
	msg->msg_size = hdr->size;

	printf("fuzzed ");
	ipc_dump(msg);
}

/* called by platform when it receives IPC message */
void fuzzer_ipc_msg_rx(struct fuzz *fuzzer)
{
	struct sof_ipc_comp_reply r;
	struct sof_ipc_cmd_hdr hdr;
	uint32_t cmd;

	printf("in %s\n", __func__);
	/* read mailbox */
	fuzzer->platform->mailbox_read(fuzzer, 0, &hdr, sizeof(hdr));
	cmd = hdr.cmd & SOF_GLB_TYPE_MASK;
	printf("cmd is 0x%x\n", cmd);

	/* check message type */
	switch (cmd) {
	case SOF_IPC_GLB_REPLY:
		fprintf(stderr, "error: ipc reply unknown\n");
		break;
	case SOF_IPC_FW_READY:
		fuzzer->platform->fw_ready(fuzzer);
		fuzzer->boot_complete = 1;
		break;
	case SOF_IPC_GLB_COMPOUND:
	case SOF_IPC_GLB_TPLG_MSG:
	case SOF_IPC_GLB_PM_MSG:
	case SOF_IPC_GLB_COMP_MSG:
	case SOF_IPC_GLB_STREAM_MSG:
	case SOF_IPC_GLB_TRACE_MSG:
		printf("cmd is 0x%x\n", cmd);
		fuzzer->platform->mailbox_read(fuzzer, 0, &r, sizeof(r));
		break;
	default:
		fprintf(stderr, "error: unknown DSP message 0x%x\n", cmd);
		break;
	}

}

/* called by platform when it receives IPC message reply */
void fuzzer_ipc_msg_reply(struct fuzz *fuzzer)
{
	int ret;

	ret = fuzzer->platform->get_reply(fuzzer, &fuzzer->msg);
	if (ret < 0)
		fprintf(stderr, "error: incorrect DSP reply\n");

	ipc_dump(&fuzzer->msg);

	pthread_mutex_lock(&ipc_mutex);
	pthread_cond_signal(&ipc_cond);
	pthread_mutex_unlock(&ipc_mutex);
}

/* called by platform when FW crashses */
void fuzzer_ipc_crash(struct fuzz *fuzzer, unsigned offset)
{
	/* TODO: DSP FW has crashed. dump stack, regs, last IPC, log etc */
	fprintf(stderr, "DSP has crashed\n");
	exit(EXIT_FAILURE);
}

/* TODO: this is hardcoded atm, needs to be able to send any message */
int fuzzer_send_msg(struct fuzz *fuzzer)
{
	struct timespec timeout;
	struct timeval tp;
	int ret;

	ipc_dump(&fuzzer->msg);

	/* fuzz the ipc messages */
	if (enable_fuzzer)
		fuzz_ipc(&fuzzer->msg);

	/* send msg */
	ret = fuzzer->platform->send_msg(fuzzer, &fuzzer->msg);
	if (ret < 0) {
		fprintf(stderr, "error: message tx failed\n");
	}
	/* wait for ipc reply */
	gettimeofday(&tp, NULL);
	timeout.tv_sec  = tp.tv_sec;
	timeout.tv_nsec = tp.tv_usec * 1000;
	timeout.tv_nsec += 300000000; /* 300ms timeout */

	/* first lock the boot wait mutex */
	pthread_mutex_lock(&ipc_mutex);

	/* now wait for mutex to be unlocked by boot ready message */
	ret = pthread_cond_timedwait(&ipc_cond, &ipc_mutex, &timeout);
	if (ret == ETIMEDOUT) {
		ret = -EINVAL;
		fprintf(stderr, "error: IPC timeout\n");
		ipc_dump_err(&fuzzer->msg);
		pthread_mutex_unlock(&ipc_mutex);
		exit(0);
	}

	pthread_mutex_unlock(&ipc_mutex);

	/*
	 * sleep for 5 ms before continuing sending the next message.
	 * This helps with the condition signaling working better.
	 * Otherwise the condition seems to always satisfy and
	 * the fuzzer never waits for a response from the DSP.
	 */
	usleep(50000);

	return ret;
}

int main(int argc, char *argv[])
{
	struct fuzz fuzzer;
	int ret;
	char opt;
	char *topology_file;
	char *platform_name = NULL;
	int i, j;
	int regions = 0;

	/* parse arguments */
	while ((opt = getopt(argc, argv, "ht:p:f")) != -1) {
		switch (opt) {
		case 't':
			topology_file = optarg;
			break;
		case 'p':
			platform_name = optarg;
			break;
		case 'f':
			enable_fuzzer = 1;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
			break;
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	/* initialise emulated target device */
	if (!platform_name) {
		fprintf(stderr, "error: no target platform specified\n");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	/* find platform */
	for (i = 0; i < ARRAY_SIZE(platform); i++) {
		if (!strcmp(platform[i]->name, platform_name))
			goto found;
	}

	/* no platform found */
	fprintf(stderr, "error: platform %s not supported\n", platform_name);
	usage(argv[0]);
	exit(EXIT_FAILURE);

found:
	ret = platform[i]->init(&fuzzer, platform[i]);
	if (ret == ETIMEDOUT) {
		fprintf(stderr, "error: platform %s failed to initialise\n",
				platform_name);
		exit(EXIT_FAILURE);
	}

	fprintf(stdout, "FW boot complete\n");

	/* initialize condition */
	pthread_cond_init(&ipc_cond, NULL);

	/*FIXME: allocate max ipc size bytes for the msg and reply for now */
	fuzzer.msg.msg_data = malloc(SOF_IPC_MSG_MAX_SIZE);
	fuzzer.msg.reply_data = malloc(SOF_IPC_MSG_MAX_SIZE);

	/* load topology */
	ret = parse_tplg(&fuzzer, "../topology/sof-byt-rt5651.tplg");
	if (ret < 0)
		exit(EXIT_FAILURE);

	for (j = 0; j < 10000; j++) {
		if (j % 2)
			send_volume_command(&fuzzer, 1 << 16);
		else
			send_volume_command(&fuzzer, 0);
	}

	pthread_mutex_destroy(&ipc_mutex);
	pthread_cond_destroy(&ipc_cond);

	/* TODO: at this point platform should be initialised and we can send IPC */

	/* TODO enable trace */

	/* TODO load topology to DSP */

	/* TODO fuzz IPC */

	/* all done - now free platform */
	platform[i]->free(&fuzzer);
	return 0;
}