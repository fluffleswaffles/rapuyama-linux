// read-only debugfs view of the E5's preserved ISASW (modem firmware) memory,
// for the cellular port: /sys/kernel/debug/e5mem/<phys>. Nothing is written.
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/io.h>

static const struct { phys_addr_t base; size_t size; } reg[] = {
	{ 0x10480000, 0x3ae000 },	/* ISASW code (V c6300000) */
	{ 0x100ec000, 0x2000 },		/* SIM patch window (V c60f0000) */
	{ 0x1fb60000, 0x180000 },	/* symbian top, unidentified */
	{ 0x1fd50000, 0x280000 },	/* ISASW data (V c6800000) */
	{ 0x1ffe0000, 0x10000 },	/* secure message pool (V c6b00000) */
	/* everything below Linux RAM (symbian page tables, dsp areas), 2 MB each */
	{ 0x10000000, 0x200000 }, { 0x10200000, 0x200000 }, { 0x10400000, 0x200000 },
	{ 0x10600000, 0x200000 }, { 0x10800000, 0x200000 }, { 0x10a00000, 0x200000 },
	{ 0x10c00000, 0x200000 }, { 0x10e00000, 0x200000 }, { 0x11000000, 0x200000 },
	{ 0x11200000, 0x200000 }, { 0x11400000, 0x200000 },
};
static void *virt[ARRAY_SIZE(reg)];
static struct debugfs_blob_wrapper blob[ARRAY_SIZE(reg)];
static struct dentry *dir;

static int __init e5mem_init(void)
{
	unsigned int i;
	char name[16];

	dir = debugfs_create_dir("e5mem", NULL);
	for (i = 0; i < ARRAY_SIZE(reg); i++) {
		virt[i] = memremap(reg[i].base, reg[i].size, MEMREMAP_WB);
		if (!virt[i]) {
			pr_warn("e5mem: %pa not mapped\n", &reg[i].base);
			continue;
		}
		blob[i].data = virt[i];
		blob[i].size = reg[i].size;
		snprintf(name, sizeof(name), "%08x", (u32)reg[i].base);
		debugfs_create_blob(name, 0400, dir, &blob[i]);
	}
	return 0;
}

static void __exit e5mem_exit(void)
{
	unsigned int i;

	debugfs_remove_recursive(dir);
	for (i = 0; i < ARRAY_SIZE(reg); i++)
		if (virt[i])
			memunmap(virt[i]);
}

module_init(e5mem_init);
module_exit(e5mem_exit);
MODULE_LICENSE("GPL");
