#include <linux/kthread.h>
#include "marlin_platform.h"
#include <linux/vmalloc.h>

#include "rdc_debug.h"
#include "wcn_txrx.h"
#include "mdbg_type.h"
#include "../include/wcn_dbg.h"

static struct task_struct *wcn_debug_thread;
struct completion dumpmem_complete;
/*
 * GKI: modules cannot create files (filp_open/kernel_write are not in the KMI),
 * so the CP log / memory dump streams are drained and accounted for instead of
 * being written to /data/wcn/cp2log.txt and /data/wcn/cp2mem.mem. Use the
 * mdbg userspace interface to capture them.
 */

int log_rx_callback(void *addr, unsigned int len, unsigned int fifo_id)
{
	WCN_DEBUG("%s: drop %u bytes of cp log\n", __func__, len);
	mdbg_read_release(fifo_id);

	return 0;
}

static int wcn_dbg_thread(void *unused)
{
	char *buffer = NULL;
	long int read_size;
	ssize_t count = 2 * 1024 * 1024;

	while (!kthread_should_stop()) {
		wait_for_completion(&dumpmem_complete);

		buffer = vmalloc(2 * 1024 * 1024);
		if (!buffer)
			return -ENOMEM;

		read_size = mdbg_receive(buffer, count);
		WCN_INFO("mem read size is %ld\n", read_size);
		vfree(buffer);
		if (read_size < 0)
			return read_size;
	}

	return 0;
}

static int launch_debug(void)
{
	if (wcn_debug_thread)
		return 0;

	wcn_debug_thread = kthread_create(wcn_dbg_thread, NULL,
				"wcn_dbg_thread");
	if (IS_ERR(wcn_debug_thread)) {
		WCN_ERR("cannot start thread wcn_debug_thread");
		wcn_debug_thread = NULL;
		return -ENOMEM;
	}
	wake_up_process(wcn_debug_thread);

	return 0;
}

int wcn_debug_init(void)
{
	int err;

	if (functionmask[7] == 0)
		return 0;
	init_completion(&dumpmem_complete);

	WCN_INFO("%s entry\n", __func__);

	if (unlikely(!wcn_debug_thread)) {
		err = launch_debug();
		if (err)
			goto Err;
	}

	return 0;

Err:
	return err;
}
