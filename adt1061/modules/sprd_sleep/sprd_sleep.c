// SPDX-License-Identifier: GPL-2.0
/*
 * sprd_sleep: diagnostics for CPU power-down (PSCI CPU_SUSPEND) wakeup.
 *
 * A monitor thread on the boot CPU periodically pings a target CPU with an
 * IPI. When the ping is not answered it dumps the target's GIC redistributor
 * and PSCI state, then tries to recover it step by step so the log shows which
 * mechanism is missing. No hardware values are assumed: the redistributor
 * frame is located through the GIC DT node and GICR_TYPER.
 */
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/smp.h>
#include <linux/atomic.h>
#include <uapi/linux/psci.h>
#include <linux/sched.h>
#include <linux/idle_inject.h>
#include <linux/cpuidle.h>
#include <linux/completion.h>
#include <asm/cputype.h>

#define GICR_CTLR		0x00
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI_OFF		0x10000
#define GICR_IGROUPR0		0x080
#define GICR_ISENABLER0		0x100
#define GICR_ISPENDR0		0x200
#define GICR_IGRPMODR0		0xd00
#define GICR_TYPER_LAST		BIT(4)
#define GICR_WAKER_PS		BIT(1)
#define GICR_WAKER_CA		BIT(2)

static int target_cpu = -1;
module_param(target_cpu, int, 0444);
MODULE_PARM_DESC(target_cpu, "CPU to monitor (default: last possible CPU)");
static unsigned long skip_mask = 0x3e;	/* test only cpu6 and cpu7 */
module_param(skip_mask, ulong, 0444);
MODULE_PARM_DESC(skip_mask, "bitmask of CPUs excluded from the forced power-down test");
static unsigned int pd_iters = 40;
module_param(pd_iters, uint, 0644);
static unsigned int pd_ms = 5;
module_param(pd_ms, uint, 0644);
static unsigned int run_secs = 120;
module_param(run_secs, uint, 0644);

static atomic_t pm_enter[NR_CPUS], pm_exit[NR_CPUS];
static void __iomem *rd_bases[NR_CPUS];	/* redistributor frame per CPU */
#define rd_base rd_bases[target_cpu]

static int pm_notify(struct notifier_block *nb, unsigned long cmd, void *v)
{
	int cpu = smp_processor_id();

	if (cmd == CPU_PM_ENTER)
		atomic_inc(&pm_enter[cpu]);
	else if (cmd == CPU_PM_EXIT)
		atomic_inc(&pm_exit[cpu]);
	return NOTIFY_OK;
}
static struct notifier_block pm_nb = { .notifier_call = pm_notify };

static u64 cpu_mpidr(int cpu)
{
	struct device_node *np = of_cpu_device_node_get(cpu);
	const __be32 *reg;
	u64 v = 0;

	if (np) {
		reg = of_get_property(np, "reg", NULL);
		if (reg)
			v = of_read_number(reg, of_n_addr_cells(np));
		of_node_put(np);
	}
	return v & MPIDR_HWID_BITMASK;
}

static u64 psci_call(u32 fn, u64 a1, u64 a2, u64 a3)
{
	struct arm_smccc_res res;

	arm_smccc_smc(fn, a1, a2, a3, 0, 0, 0, 0, &res);
	return res.a0;
}

static void dump_state(const char *why)
{
	u64 mpidr = cpu_mpidr(target_cpu);
	void __iomem *sgi = rd_base + GICR_SGI_OFF;
	int c;

	pr_info("sprd_sleep: [%s] cpu%d mpidr 0x%llx psci affinity_info=%lld\n",
		why, target_cpu, mpidr & MPIDR_HWID_BITMASK,
		(s64)psci_call(PSCI_0_2_FN64_AFFINITY_INFO,
			       mpidr & MPIDR_HWID_BITMASK, 0, 0));
	pr_info("sprd_sleep: [%s] GICR CTLR 0x%x WAKER 0x%x ISENABLER0 0x%x ISPENDR0 0x%x IGROUPR0 0x%x IGRPMODR0 0x%x\n",
		why, readl_relaxed(rd_base + GICR_CTLR),
		readl_relaxed(rd_base + GICR_WAKER),
		readl_relaxed(sgi + GICR_ISENABLER0),
		readl_relaxed(sgi + GICR_ISPENDR0),
		readl_relaxed(sgi + GICR_IGROUPR0),
		readl_relaxed(sgi + GICR_IGRPMODR0));
	for_each_possible_cpu(c)
		pr_info("sprd_sleep: [%s] cpu%d online=%d pm enter=%d exit=%d\n",
			why, c, cpu_online(c), atomic_read(&pm_enter[c]),
			atomic_read(&pm_exit[c]));
}

static void ping_fn(void *info)
{
	atomic_inc((atomic_t *)info);
}

static bool ping(atomic_t *cnt, unsigned int tmo_ms)
{
	static call_single_data_t csd;
	int before = atomic_read(cnt);

	csd.func = ping_fn;
	csd.info = cnt;
	smp_call_function_single_async(target_cpu, &csd);
	while (tmo_ms--) {
		if (atomic_read(cnt) != before)
			return true;
		msleep(1);
	}
	return false;
}


/*
 * Force the deepest cpuidle state with the kernel's idle injection
 * (FIFO kthread -> play_idle_precise()). One injection device per CPU is
 * created once because idle_inject_unregister() is not exported.
 */
static struct idle_inject_device *ii_dev[NR_CPUS];
static bool pd_active = true;

static int pd_setup(void)
{
	struct cpumask mask;
	int c;

	for_each_possible_cpu(c) {
		if (!c)
			continue;
		cpumask_clear(&mask);
		cpumask_set_cpu(c, &mask);
		ii_dev[c] = idle_inject_register(&mask);
		if (!ii_dev[c])
			return -ENOMEM;
		idle_inject_set_latency(ii_dev[c], UINT_MAX);
	}
	return 0;
}

static void pd_run(unsigned long mask, unsigned int idle_ms,
		   unsigned int total_ms, const char *label)
{
	int c, e0[NR_CPUS];

	pr_info("sprd_sleep: PDTEST %s start (cpus 0x%lx, idle %ums, %ums)\n",
		label, mask, idle_ms, total_ms);
	for_each_possible_cpu(c) {
		e0[c] = atomic_read(&pm_enter[c]);
		if (!c || !(mask & BIT(c)))
			continue;
		idle_inject_set_duration(ii_dev[c], 1000, idle_ms * 1000);
		idle_inject_start(ii_dev[c]);
	}
	msleep(total_ms);
	for_each_possible_cpu(c)
		if (c && (mask & BIT(c)))
			idle_inject_stop(ii_dev[c]);
	msleep(100);
	pr_info("sprd_sleep: PDTEST %s OK; pm enter/exit delta:", label);
	for_each_possible_cpu(c)
		if (c && (mask & BIT(c)))
			pr_cont(" c%d=+%d/%d", c, atomic_read(&pm_enter[c]) - e0[c],
				atomic_read(&pm_exit[c]));
	pr_cont("\n");
}

static int pd_test(void *unused)
{
	unsigned long all = 0;
	int c;

	if (pd_setup()) {
		pr_err("sprd_sleep: idle injection setup failed\n");
		WRITE_ONCE(pd_active, false);
		return 0;
	}
	for_each_possible_cpu(c)
		if (c && !(skip_mask & BIT(c)))
			all |= BIT(c);

	/* one CPU at a time, highest CPU first, growing sleep time */
	for (c = nr_cpu_ids - 1; c > 0; c--) {
		static const unsigned int idle_ms[] = { 20, 100, 500 };
		unsigned int i;

		if (!(all & BIT(c)))
			continue;
		for (i = 0; i < ARRAY_SIZE(idle_ms); i++) {
			char l[32];

			snprintf(l, sizeof(l), "cpu%d-idle%ums", c, idle_ms[i]);
			pd_run(BIT(c), idle_ms[i], 1000 + 4 * idle_ms[i], l);
		}
	}
	pr_info("sprd_sleep: PDTEST all phases finished\n");
	WRITE_ONCE(pd_active, false);
	return 0;
}

static int monitor(void *unused)
{
	atomic_t cnt = ATOMIC_INIT(0);
	unsigned long end = jiffies + run_secs * HZ;
	unsigned long okc = 0, next_rep = jiffies + 5 * HZ;
	u32 w;
	int c;

	/* no IPIs while the power-down tests run: they would cut the sleeps short */
	while (READ_ONCE(pd_active) && !kthread_should_stop())
		msleep(200);
	end = jiffies + run_secs * HZ;
	pr_info("sprd_sleep: monitoring cpu1..%d for %us\n",
		(int)cpumask_last(cpu_possible_mask), run_secs);
	while (!kthread_should_stop() && time_before(jiffies, end)) {
		for_each_possible_cpu(c) {
			if (!c)
				continue;
			target_cpu = c;
			if (ping(&cnt, 300))
				continue;
			dump_state("ping timeout");
			if (ping(&cnt, 300)) {
				pr_info("sprd_sleep: cpu%d second ping answered\n", c);
				continue;
			}
			w = readl_relaxed(rd_base + GICR_WAKER);
			pr_info("sprd_sleep: cpu%d clearing ProcessorSleep (WAKER 0x%x)\n", c, w);
			writel_relaxed(w & ~GICR_WAKER_PS, rd_base + GICR_WAKER);
			msleep(50);
			dump_state("after WAKER clear");
			if (ping(&cnt, 500))
				pr_info("sprd_sleep: cpu%d woke after WAKER clear => redistributor sleep request blocks wakeup\n", c);
			else
				pr_info("sprd_sleep: cpu%d still not responding, giving up\n", c);
			return 0;
		}
		okc++;
		if (time_after(jiffies, next_rep)) {
			pr_info("sprd_sleep: round %lu ok; pm enter/exit:", okc);
			for_each_possible_cpu(c)
				pr_cont(" c%d=%d/%d", c, atomic_read(&pm_enter[c]),
					atomic_read(&pm_exit[c]));
			pr_cont("\n");
			next_rep = jiffies + 5 * HZ;
		}
		msleep(100);
	}
	pr_info("sprd_sleep: done after %lu rounds\n", okc);
	return 0;
}

static int find_rd(void)
{
	struct device_node *np;
	struct resource res;
	u64 want = cpu_mpidr(target_cpu);
	u64 stride = 0;
	void __iomem *base;
	resource_size_t off;
	int ret = -ENODEV;

	np = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	if (!np || of_address_to_resource(np, 1, &res)) {
		of_node_put(np);
		return -ENODEV;
	}
	of_property_read_u64(np, "redistributor-stride", &stride);
	of_node_put(np);

	base = ioremap(res.start, resource_size(&res));
	if (!base)
		return -ENOMEM;
	for (off = 0; off < resource_size(&res); ) {
		u64 typer = readq_relaxed(base + off + GICR_TYPER);
		u64 aff = ((typer >> 32) & 0xffffff) |
			  (((typer >> 56) & 0xff) << 24);

		if (aff == want) {
			rd_base = ioremap(res.start + off, 0x20000);
			ret = rd_base ? 0 : -ENOMEM;
			pr_info("sprd_sleep: cpu%d redistributor at %pa+0x%llx\n",
				target_cpu, &res.start, (u64)off);
			break;
		}
		if (typer & GICR_TYPER_LAST)
			break;
		off += stride ? stride : 0x20000;
	}
	iounmap(base);
	return ret;
}

static struct task_struct *mon_task;

static int __init sprd_sleep_init(void)
{
	int c, ret;

	for_each_possible_cpu(c) {
		if (!c)
			continue;
		target_cpu = c;
		ret = find_rd();
		if (ret) {
			pr_err("sprd_sleep: cpu%d redistributor not found (%d)\n", c, ret);
			return ret;
		}
	}
	cpu_pm_register_notifier(&pm_nb);
	target_cpu = cpumask_last(cpu_possible_mask);
	dump_state("init");
	mon_task = kthread_run(monitor, NULL, "sprd_sleep_mon");
	kthread_run(pd_test, NULL, "sprd_pdtest");
	return PTR_ERR_OR_ZERO(mon_task);
}
module_init(sprd_sleep_init);

MODULE_IMPORT_NS(IDLE_INJECT);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Unisoc CPU power-down wakeup diagnostics");
