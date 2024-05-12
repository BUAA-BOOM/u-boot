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

static volatile int shared_value;

typedef struct {
    volatile uint32_t status;
    volatile uint32_t en;
    volatile uint32_t set;
    volatile uint32_t clear;
    volatile uint32_t pad[4];
    volatile uint64_t mbuf[4];
} ipi_t;

static ipi_t *ipi[MAX_CORE];

static void init_ipi() {
    for(int i = 0 ; i < MAX_CORE ; i += 1) {
        ipi[i] = (ipi_t *)(IPI_BASE + i * 0x1000);
    }
}

static void test_function(uint32_t core, uint32_t param) {
    shared_value += 1;
    // printf("There is core %d speaking, parm: %x!\n", core, param);
    while(1) {
        volatile int i = 100000000;
        while(i--);
        // printf("Core %d is still alive!\n", core);
        shared_value += 1;
    }
}

static void send_action(uint32_t core, uint32_t action) {
    printf("Sending action %08x to core %d with %p.\n", action, core, *(ipi + core));
    ipi[core]->en  = 0xffffffff;
    ipi[core]->set = action;
}

static void wakeup_core(uint32_t core, uintptr_t prog, uintptr_t sp, uintptr_t tp, uintptr_t parm) {
    printf("Waking up core %d with %p.\n", core, *(ipi + core));
    ipi[core]->mbuf[0] = prog;
    ipi[core]->mbuf[1] = sp;
    ipi[core]->mbuf[2] = tp;
    ipi[core]->mbuf[3] = parm;
    send_action(core, 0x1);
    return;
}

static int do_testmb(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[]) {
    init_ipi();
    printf("Mailbox test begin!\n");
    wakeup_core(1, (uintptr_t)test_function, 0xa2000000 - 1 * 0x8000, 0xa1f00000 - 1 * 0x8000, 0x12345678);
    printf("finish call, waiting for son reply!\n");
    int old_shared_value = 0;
    while(1) {
        int new_value = shared_value;
        if(old_shared_value != new_value) {
            printf("New value %d!\r", new_value);
            old_shared_value = new_value;
        }
    }
    return 0;
}

U_BOOT_CMD(
    testmb, 10, 0,  do_testmb,
    "test whether mailbox of wired SoC function correctly.\n",
    "<1080p/720p/480p>"
)