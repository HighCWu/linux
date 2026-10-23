#include <linux/seq_file.h>

static int c_show(struct seq_file *m, void *v)
{
	int cpu;

	/* No CPU-local state is read here. In particular, do not wait for an
	 * IPI callback: a Wasm CPU executing userspace without a kernel entry
	 * cannot service it, and reading proc must not stop that computation. */
	for_each_online_cpu(cpu) {
		seq_printf(m, "processor       : %d\n", cpu);
		seq_printf(m, "\n");
	}

	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void c_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start = c_start,
	.next = c_next,
	.stop = c_stop,
	.show = c_show,
};
