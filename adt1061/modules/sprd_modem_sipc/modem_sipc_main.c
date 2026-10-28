// SPDX-License-Identifier: GPL-2.0
/*
 * sprd_modem_sipc: modem-side SIPC clients (seth, spool, spipe, sipx,
 * swcn_blk) on top of the core SIPC transport in sprd_sipc.ko.
 *
 * Under MODULE every initcall level collapses into one init_module(), so the
 * per-file init/exit functions are called from here.
 */
#include <linux/init.h>
#include <linux/module.h>

int __init seth_init(void);
void __exit seth_exit(void);
int __init spool_init(void);
void __exit spool_exit(void);
int __init spipe_init(void);
void __exit spipe_exit(void);
int __init sipx_init(void);
void __exit sipx_exit(void);
int __init swcnblk_init(void);
void __exit swcnblk_exit(void);

static int __init sprd_modem_sipc_init(void)
{
	/* same order as the vendor Makefile (initcall/link order) */
	spipe_init();
	spool_init();
	seth_init();
	sipx_init();
	swcnblk_init();

	return 0;
}

static void __exit sprd_modem_sipc_exit(void)
{
	swcnblk_exit();
	sipx_exit();
	seth_exit();
	spool_exit();
	spipe_exit();
}

module_init(sprd_modem_sipc_init);
module_exit(sprd_modem_sipc_exit);

MODULE_DESCRIPTION("SIPC modem clients");
MODULE_LICENSE("GPL");
