/*
 *
 * Copyright (C) 2013-2021 Authors
 *
 * This source file may be used and distributed without
 * restriction provided that this copyright statement is not
 * removed from the file and that any derivative work contains
 * the original copyright notice and the associated disclaimer.
 *
 * This source file is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation;
 * either version 2.1 of the License, or (at your option) any
 * later version.
 *
 * This source is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this source; if not, download it
 * from https://www.gnu.org/licenses/licenses.html
 */

#include <common.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <command.h>
#include <asm/io.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <mmc.h>

#define LITEX_PHY_CARDDETECT  0x00
#define LITEX_PHY_CLOCKERDIV  0x04
#define LITEX_PHY_INITIALIZE  0x08
#define LITEX_PHY_WRITESTATUS 0x0C
#define LITEX_CORE_CMDARG     0x00
#define LITEX_CORE_CMDCMD     0x04
#define LITEX_CORE_CMDSND     0x08
#define LITEX_CORE_CMDRSP     0x0C
#define LITEX_CORE_CMDEVT     0x1C
#define LITEX_CORE_DATEVT     0x20
#define LITEX_CORE_BLKLEN     0x24
#define LITEX_CORE_BLKCNT     0x28
#define LITEX_BLK2MEM_BASE    0x00
#define LITEX_BLK2MEM_LEN     0x08
#define LITEX_BLK2MEM_ENA     0x0C
#define LITEX_BLK2MEM_DONE    0x10
#define LITEX_BLK2MEM_LOOP    0x14
#define LITEX_MEM2BLK_BASE    0x00
#define LITEX_MEM2BLK_LEN     0x08
#define LITEX_MEM2BLK_ENA     0x0C
#define LITEX_MEM2BLK_DONE    0x10
#define LITEX_MEM2BLK_LOOP    0x14
#define LITEX_MEM2BLK         0x18
#define LITEX_IRQ_STATUS      0x00
#define LITEX_IRQ_PENDING     0x04
#define LITEX_IRQ_ENABLE      0x08

#define SD_CTL_DATA_XFER_NONE  0
#define SD_CTL_DATA_XFER_READ  1
#define SD_CTL_DATA_XFER_WRITE 2

#define SD_CTL_RESP_NONE       0
#define SD_CTL_RESP_SHORT      1
#define SD_CTL_RESP_LONG       2
#define SD_CTL_RESP_SHORT_BUSY 3

#define SD_BIT_DONE    BIT(0)
#define SD_BIT_WR_ERR  BIT(1)
#define SD_BIT_TIMEOUT BIT(2)
#define SD_BIT_CRC_ERR BIT(3)

#define SD_SLEEP_US       5
#define SD_TIMEOUT_US 20000

#define SDIRQ_CARD_DETECT    1
#define SDIRQ_SD_TO_MEM_DONE 2
#define SDIRQ_MEM_TO_SD_DONE 4
#define SDIRQ_CMD_DONE       8

struct sdc_priv {
	void *sdphy;
	void *sdcore;
	void *sdreader;
	void *sdwriter;
	void *sdirq;

	unsigned int ref_clk;
	unsigned int sd_clk;
};

#if CONFIG_IS_ENABLED(DM_MMC)
static int sdc_get_cd(struct udevice * udev) {
    return 1;
}

#if CONFIG_IS_ENABLED(BLK)
static int sdc_bind(struct udevice * dev) {
    struct sdc_plat * plat = dev_get_plat(dev);
    return mmc_bind(dev, &plat->mmc, &plat->cfg);
}
#endif

static const struct udevice_id mmc_ids[] = {
    { .compatible = "litex,mmc-0.1" },
    {}
};

static const struct dm_mmc_ops sdc_ops = {
    .get_cd = sdc_get_cd,
    .send_cmd = sdc_send_cmd,
    .set_ios = sdc_set_ios,
};

U_BOOT_DRIVER(sdc_priv) = {
    .name = "litex-mmc-0.1",
    .id = UCLASS_MMC,
    .of_match = mmc_ids,
    .ops = &sdc_ops,
#if CONFIG_IS_ENABLED(BLK)
    .bind = sdc_bind,
#endif
    .probe = sdc_probe,
    .plat_auto = sizeof(struct sdc_plat),
    .priv_auto = sizeof(struct sdc_priv),
};

#endif /* CONFIG_IS_ENABLED(DM_MMC) */
