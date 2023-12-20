/*
 * (C) Copyright 2023
 * Wang Zhe, dofingert@gmail.com
 */

#include <common.h>
#include <command.h>
#include <mapmem.h>
#include <asm/cache.h>
#include <linux/bitops.h>

struct decode_ip_ctl
{
    volatile uint32_t ctrl;
    volatile uint32_t status;
    volatile uint32_t src;
    volatile uint32_t dst;
    volatile uint32_t stride;
    volatile uint32_t iocen;
};

struct i2s_ip_ctl
{
    volatile uint32_t version;         // 0x0 - 0x3
    volatile uint32_t conf;            // 0x4 - 0x7
    volatile uint32_t pad0[66];      // 0x8 - 0x10f
    volatile uint32_t mm2s_ctrl;       // 0x110
    volatile uint32_t mm2s_status;     // 0x114
    volatile uint32_t mm2s_multiplier; // 0x118
    volatile uint32_t mm2s_period;     // 0x11c
    volatile uint32_t mm2s_dmaaddr;    // 0x120
    volatile uint32_t mm2s_dmaaddr_msb; // 0x124
    volatile uint32_t mm2s_transfer_count; // 0x128
    volatile uint32_t pad1[6];             // 0x12c - 0x143
    volatile uint32_t mm2s_channel_offset; // 0x144
};

struct fb_ip_ctl
{
    volatile uint32_t conf;
    volatile uint32_t status;
    volatile uint32_t addr;
};

/*
    8M RESERVE;
    8M VBUF:
    |-------------------------|  8M == 0x0F80_0000 == 248M
    | Framebuffer             |
    |-------------------------|  6M
    | Framebuffer             |
    |-------------------------|  4M
    | Framebuffer             |
    |-------------------------|  2M
    | Framebuffer             |
    |-------------------------|  0M == 0x0F00_0000 == 240M
*/
#define FRAMEBUFFER_SIZE (2 * 1024 * 1024)
#define FRAMEBUFFER_START (0x0F000000)
/*
    File Format:

    |-------------------------|
    | BHeader (512 bytes)     |
    |-------------------------|
    | Video Frame + Pad       |
    |-------------------------|  x 128
    | Audio Frame + Pad       |
    |-------------------------|
    | BHeader (512 bytes)     |
    |-------------------------|
    | Video Frame + Pad       |
    |-------------------------|  x 128
    | Audio Frame + Pad       |
    |-------------------------|
*/
struct headerb
{
    uint32_t frame_size[128];
};
static int sample_perframe;
static int fps;
static struct i2s_ip_ctl *i2s_ctl;
static struct fb_ip_ctl  *fb_ctl;
static struct decode_ip_ctl *decode_ctl;
static uint32_t *ab_array[4];

static void fb_play_one_frame(uint32_t fbuf_ptr) {
    fb_ctl->addr = fbuf_ptr;
    fb_ctl->conf = BIT(0);
}

static void open_i2s_device(void) {
    i2s_ctl->mm2s_ctrl = BIT(1);
    uint32_t iters=0;
    while(i2s_ctl->mm2s_ctrl & BIT(1)) {
        if((iters++) & 0xffff) {
            printf("Open I2S STUCK %x", i2s_ctl->mm2s_ctrl);
            i2s_ctl->mm2s_ctrl = 0;
        }
    }
    i2s_ctl->mm2s_ctrl = BIT(13) | (0x1 << 16) | (0x2 << 19);
    i2s_ctl->mm2s_multiplier = 512;
}

static void close_i2s_device(void) {
    i2s_ctl->mm2s_ctrl = BIT(1);
    uint32_t iters=0;
    while(i2s_ctl->mm2s_ctrl & BIT(1)){
        if((iters++) & 0xffff) {
            printf("Close I2S STUCK");
            i2s_ctl->mm2s_ctrl = 0;
        }
    }
}

static void i2s_play_one_frame(uint32_t abuf_ptr) {
    i2s_ctl->mm2s_dmaaddr = abuf_ptr;
    i2s_ctl->mm2s_dmaaddr_msb = 0;
}

static int play_one_frame(int fb_num)
{
    uint32_t vbuf_ptr = FRAMEBUFFER_START + FRAMEBUFFER_SIZE * fb_num;
    uint32_t abuf_ptr = ab_array[fb_num];
    i2s_play_one_frame(abuf_ptr);
    fb_play_one_frame(vbuf_ptr);
    return 0;
}

static uint32_t padding(uint32_t in)
{
    return ((in + 511) / 512) * 512;
}

static int decode_one_frame(int frame_size,
                            void *jpeg_file_ptr,
                            int fb_num)
{
    int vframe_size;
    vframe_size = frame_size;

    decode_ctl->ctrl = 0x40000000;

    // 配置 decode ip 的地址
    decode_ctl->src = ((uint32_t)jpeg_file_ptr) & 0x1fffffff;
    decode_ctl->dst = FRAMEBUFFER_START + FRAMEBUFFER_SIZE * fb_num;
    decode_ctl->stride = 1280; // FIXED 1280x720p, but resolution might be various.

    // 配置开始 decode ip 的解码
    decode_ctl->iocen = 1; // 打开中断输出，清理旧的中断
    decode_ctl->ctrl  = 0x80000000 | vframe_size;
    // printf("decoder_mmio %x %x %x\n", decode_ctl->src, decode_ctl->dst, decode_ctl->ctrl, decode_ptr);

    // 音频数据指针写入到音频指针缓冲区
    ab_array[fb_num] = ((uint32_t)(jpeg_file_ptr + padding(vframe_size))) & 0x1fffffff;
    return 0;
}

// return 0 on finish.
static int get_next_frame_ptr(int init, void **binary)
{
    static int sub_frame_cnt;
    static struct headerb *hdr_b;
    if(init) {
        sub_frame_cnt = 0;
        hdr_b = *binary;
        *binary += 512;
    } else {
        uint32_t aframe_size = padding(sample_perframe * 4);
        *binary += padding(hdr_b->frame_size[sub_frame_cnt++]) + aframe_size;
    }
    if(sub_frame_cnt >= 128) {
        sub_frame_cnt = 0;
        hdr_b = *binary;
        *binary += 512;
    }
    return hdr_b->frame_size[sub_frame_cnt];
}

static int wait_an_interrupt(int target_mask, int forever)
{
    uint32_t iters = 0;
    while(1) {
        uint32_t estate, irqs;
        asm volatile(
		"	csrrd	%0, 0x5\n"
		: "=r"(estate) :: "memory");
        irqs = (estate >> 4) & target_mask;
        if(irqs){
            return irqs;
        }

        iters++;
        if((iters & 0xfffff) == 0 && !forever) {
            printf("NIR%d,%x\n",iters, estate);
            return 0;
        }
    }
}

static int mediaplayer(void *binary)
{
    int decode_ptr = 0, play_ptr = 0, finish_flag = 0, fifo_size = 0, int_cnt = 0;
    int frame_size;
    // 初始化 I2S 控制器
    printf("offset of MM2S_CTRL is %x\n", (uint32_t)(&i2s_ctl->mm2s_ctrl) - (uint32_t)i2s_ctl);
    open_i2s_device();
    // 先解码四帧填满
    for(int i = 0 ; i < 4 ; i++) {
        frame_size = get_next_frame_ptr(i == 0, &binary);
        if(frame_size == 0) {
            finish_flag = 1;
            break;
        }
        decode_one_frame(frame_size,binary,decode_ptr++);
        wait_an_interrupt(0x2, 1);
        decode_ptr &= 3;
        fifo_size++;
    }
    // 之后播放第一帧
    play_one_frame(play_ptr++);
    i2s_ctl->mm2s_period = (2 << 16) | (sample_perframe * 2);
    i2s_ctl->mm2s_ctrl = BIT(0) | BIT(13) | (0x1 << 16) | (0x2 << 19);
    i2s_ctl->mm2s_channel_offset = sample_perframe * 1;
    play_ptr &= 3;
    fifo_size --;
    // FIFO_SIZE == 3, FULL.
    uint32_t iter;
    // 开始轮询中断
    while(1) {
        if(finish_flag && fifo_size == 0) {
            printf("Play end!\n");
            break;
        } // 播放完成，退出
        int irq = wait_an_interrupt(0x3, 0);
        if(irq & 0x1) {
            if(fifo_size) {
                // I2S IRQ DRIVENED PLAY NEEDED
                i2s_ctl->mm2s_status = BIT(31); // DISABLE INTERRUPT
                while(1) {
                    uint32_t estate;
                    asm volatile(
                        "	csrrd	%0, 0x5\n"
                        : "=r"(estate) :: "memory");
                    if(!(estate & (0x1 << 4))) break;
                }
                if((int_cnt++) & 1) {
                    int_cnt &= 1;
                } else {// Dosen't need to handle this in the real finish anymore.
                    play_one_frame(play_ptr++);
                    play_ptr &= 3;
                    fifo_size --;
                    iter = 0;
                }
            } else {
                // FIFO UNDER FLOW
                iter++;
                if((iter & 0xfff) == 0) printf("UF\n");
                if((iter & 0xfff) == 0) irq |= 0x2;
                // 强制继续播放
            }
        }
        if((irq & 0x2) && !finish_flag) {
            // DECODE OK
            if(fifo_size < 2) {
                iter = 0;
                frame_size = get_next_frame_ptr(0, &binary);
                if(frame_size == 0) {
                    finish_flag = 1;
                    continue;
                }
                decode_one_frame(frame_size,binary,decode_ptr++);
                decode_ptr &= 3;
                fifo_size += 1;
            } else {
                // FIFO OVER FLOW
                iter++;
                if((iter & 0xfffff) == 0) printf("OF\n");
            }
        }
    }
    decode_ctl->iocen = 0; // 关闭中断输出，清理旧的中断
    close_i2s_device();
    return 0;
}

int do_playmedia(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if(argc < 3) {
		printf("Usage: playmedia 0x[media_binary_addr] [fps]\n");
		return 0;
	}
	uint32_t location;
	sscanf(argv[1], "0x%x", &location);
    sscanf(argv[2], "%d", &fps);
	if(location < 0xa0000000) {
		printf("Location should'nt be lower than 0xa0000000.\n");
		return 0;
	}
    if(fps < 1 || fps > 25) {
        printf("FPS should between [1,25]");
    }
    sample_perframe = 48000 / fps; // FIXED 48000 SAMPLING RATE.
    fb_ctl = (void*) 0x9d0d0000;
    i2s_ctl = (void*) 0x9d0b0000;
    decode_ctl = (void*) 0x9d0a0000;
	return mediaplayer((void*)location);
}

U_BOOT_CMD(
	playmedia,	7,	0,	do_playmedia,
	"play a mjpeg file from a specified memory location.\n",
    "0x<addr> fps"
);