/*
 * (C) Copyright 2023
 * Wang Zhe, dofingert@gmail.com
 */

#include <common.h>
#include <command.h>
#include <mapmem.h>

#define IPI_BASE 0x9d200000
#define MAX_CORE 4
// #define STATUS   0x00
// #define EN       0x04
// #define SET      0x08
// #define CLEAR    0x0c
// #define MBUF     0x20

typedef struct {
    volatile uint32_t status;
    volatile uint32_t en;
    volatile uint32_t set;
    volatile uint32_t clear;
    volatile uint32_t pad[4];
    volatile uint32_t mbuf[4];
} ipi_t;

static ipi_t *ipi[MAX_CORE];

static void init_ipi() {
    for(int i = 0 ; i < MAX_CORE ; i += 1) {
        ipi[i] = (ipi_t *)(IPI_BASE + i * 0x1000);
    }
}

static void send_action(uint32_t core, uint32_t action) {
    printf("Sending action %08x to core %d with %p.\n", action, core, *(ipi + core));
    ipi[core]->en  = 0xffffffff;
    ipi[core]->set = action;
}

static int do_testmb(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[]) {
    init_ipi();
    printf("Mailbox test begin!\n");
    send_action(1, 0x12345);
    printf("finish!\n");
    return 0;
}

U_BOOT_CMD(
    testmb, 10, 0,  do_testmb,
    "test whether mailbox of wired SoC function correctly.\n",
    "<1080p/720p/480p>"
)