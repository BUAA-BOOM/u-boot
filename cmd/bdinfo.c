// SPDX-License-Identifier: GPL-2.0+
/*
 * Implements the 'bd' command to show board information
 *
 * (C) Copyright 2003
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

#include <common.h>
#include <command.h>
#include <dm.h>
#include <env.h>
#include <lmb.h>
#include <net.h>
#include <video.h>
#include <vsprintf.h>
#include <asm/cache.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

void bdinfo_print_num_l(const char *name, ulong value)
{
	printf("%-12s= 0x%0*lx\n", name, 2 * (int)sizeof(value), value);
}

void bdinfo_print_num_ll(const char *name, unsigned long long value)
{
	printf("%-12s= 0x%.*llx\n", name, 2 * (int)sizeof(ulong), value);
}

static void print_eth(int idx)
{
	char name[10], *val;
	if (idx)
		sprintf(name, "eth%iaddr", idx);
	else
		strcpy(name, "ethaddr");
	val = env_get(name);
	if (!val)
		val = "(not set)";
	printf("%-12s= %s\n", name, val);
}

void bdinfo_print_mhz(const char *name, unsigned long hz)
{
	char buf[32];

	printf("%-12s= %6s MHz\n", name, strmhz(buf, hz));
}

static void print_bi_dram(const struct bd_info *bd)
{
	int i;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; ++i) {
		if (bd->bi_dram[i].size) {
			bdinfo_print_num_l("DRAM bank",	i);
			bdinfo_print_num_ll("-> start",	bd->bi_dram[i].start);
			bdinfo_print_num_ll("-> size",	bd->bi_dram[i].size);
		}
	}
}

__weak void arch_print_bdinfo(void)
{
}

static void show_video_info(void)
{
	const struct udevice *dev;
	struct uclass *uc;

	uclass_id_foreach_dev(UCLASS_VIDEO, dev, uc) {
		printf("%-12s= %s %sactive\n", "Video", dev->name,
		       device_active(dev) ? "" : "in");
		if (device_active(dev)) {
			struct video_priv *upriv = dev_get_uclass_priv(dev);

			bdinfo_print_num_ll("FB base", (ulong)upriv->fb);
			if (upriv->copy_fb)
				bdinfo_print_num_ll("FB copy",
						    (ulong)upriv->copy_fb);
			printf("%-12s= %dx%dx%d\n", "FB size", upriv->xsize,
			       upriv->ysize, 1 << upriv->bpix);
		}
	}
}

int do_bdinfo(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct bd_info *bd = gd->bd;

#ifdef DEBUG
	bdinfo_print_num_l("bd address", (ulong)bd);
#endif
	bdinfo_print_num_l("boot_params", (ulong)bd->bi_boot_params);
	print_bi_dram(bd);
	if (IS_ENABLED(CONFIG_SYS_HAS_SRAM)) {
		bdinfo_print_num_l("sramstart", (ulong)bd->bi_sramstart);
		bdinfo_print_num_l("sramsize", (ulong)bd->bi_sramsize);
	print_bi_flash(bd);

#if defined(CONFIG_SYS_SRAM_BASE)
	print_num ("sram start",	(ulong)bd->bi_sramstart);
	print_num ("sram size",		(ulong)bd->bi_sramsize);
#endif

	print_eth_ip_addr();
	print_baudrate();

	return 0;
}

#elif defined(CONFIG_MICROBLAZE)

int do_bdinfo(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	bd_t *bd = gd->bd;

	print_bi_dram(bd);
	print_bi_flash(bd);
#if defined(CONFIG_SYS_SRAM_BASE)
	print_num("sram start     ",	(ulong)bd->bi_sramstart);
	print_num("sram size      ",	(ulong)bd->bi_sramsize);
#endif
#if defined(CONFIG_CMD_NET) && !defined(CONFIG_DM_ETH)
	print_eths();
#endif
	print_baudrate();
	print_num("relocaddr", gd->relocaddr);
	print_num("reloc off", gd->reloc_off);
	print_num("fdt_blob", (ulong)gd->fdt_blob);
	print_num("new_fdt", (ulong)gd->new_fdt);
	print_num("fdt_size", (ulong)gd->fdt_size);

	return 0;
}

#elif defined(CONFIG_M68K)

int do_bdinfo(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	bd_t *bd = gd->bd;

	print_bi_mem(bd);
	print_bi_flash(bd);
#if defined(CONFIG_SYS_INIT_RAM_ADDR)
	print_num("sramstart",		(ulong)bd->bi_sramstart);
	print_num("sramsize",		(ulong)bd->bi_sramsize);
#endif
#if defined(CONFIG_SYS_MBAR)
	print_num("mbar",		bd->bi_mbar_base);
#endif
	print_mhz("cpufreq",		bd->bi_intfreq);
	print_mhz("busfreq",		bd->bi_busfreq);
#ifdef CONFIG_PCI
	print_mhz("pcifreq",		bd->bi_pcifreq);
#endif
#ifdef CONFIG_EXTRA_CLOCK
	print_mhz("flbfreq",		bd->bi_flbfreq);
	print_mhz("inpfreq",		bd->bi_inpfreq);
	print_mhz("vcofreq",		bd->bi_vcofreq);
#endif
	print_eth_ip_addr();
	print_baudrate();

	return 0;
}

#elif defined(CONFIG_MIPS) || defined(CONFIG_LA32R) 

int do_bdinfo(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	print_std_bdinfo(gd->bd);
	print_num("relocaddr", gd->relocaddr);
	print_num("reloc off", gd->reloc_off);

	return 0;
}

#elif defined(CONFIG_ARM)

static int do_bdinfo(cmd_tbl_t *cmdtp, int flag, int argc,
			char * const argv[])
{
	bd_t *bd = gd->bd;

	print_num("arch_number",	bd->bi_arch_number);
	print_bi_boot_params(bd);
	print_bi_dram(bd);

#ifdef CONFIG_SYS_MEM_RESERVE_SECURE
	if (gd->arch.secure_ram & MEM_RESERVE_SECURE_SECURED) {
		print_num("Secure ram",
			  gd->arch.secure_ram & MEM_RESERVE_SECURE_ADDR_MASK);
	}
	bdinfo_print_num_l("flashstart", (ulong)bd->bi_flashstart);
	bdinfo_print_num_l("flashsize", (ulong)bd->bi_flashsize);
	bdinfo_print_num_l("flashoffset", (ulong)bd->bi_flashoffset);
	printf("baudrate    = %u bps\n", gd->baudrate);
	bdinfo_print_num_l("relocaddr", gd->relocaddr);
	bdinfo_print_num_l("reloc off", gd->reloc_off);
	printf("%-12s= %u-bit\n", "Build", (uint)sizeof(void *) * 8);
	if (IS_ENABLED(CONFIG_CMD_NET)) {
		printf("current eth = %s\n", eth_get_name());
		print_eth(0);
		printf("IP addr     = %s\n", env_get("ipaddr"));
	}
	bdinfo_print_num_l("fdt_blob", (ulong)gd->fdt_blob);
	bdinfo_print_num_l("new_fdt", (ulong)gd->new_fdt);
	bdinfo_print_num_l("fdt_size", (ulong)gd->fdt_size);
	if (IS_ENABLED(CONFIG_DM_VIDEO))
		show_video_info();
#if defined(CONFIG_LCD) || defined(CONFIG_VIDEO)
	bdinfo_print_num_l("FB base  ", gd->fb_base);
#endif
#if CONFIG_IS_ENABLED(MULTI_DTB_FIT)
	bdinfo_print_num_l("multi_dtb_fit", (ulong)gd->multi_dtb_fit);
#endif
	if (gd->fdt_blob) {
		struct lmb lmb;

		lmb_init_and_reserve(&lmb, gd->bd, (void *)gd->fdt_blob);
		lmb_dump_all_force(&lmb);
	}

	arch_print_bdinfo();

	return 0;
}

U_BOOT_CMD(
	bdinfo,	1,	1,	do_bdinfo,
	"print Board Info structure",
	""
);
