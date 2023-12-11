/*
 * (C) Copyright 2023
 * Wang Zhe, dofingert@gmail.com
 */

#include <common.h>
#include <command.h>
#include <mapmem.h>
#include <asm/cache.h>
#include <bitops.h>

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
    volatile uint32_t addr;
    volatile uint32_t ctrl;
    volatile uint32_t iesr;
    volatile uint32_t ccr;
};

/*
    8M VBUF:
    |-------------------------| 8M == 0x0800_0000 == 128M
    | Framebuffer             |
    |-------------------------| 6M
    | Framebuffer             |
    |-------------------------| 4M
    | Framebuffer             |
    |-------------------------| 2M
    | Framebuffer             |
    |-------------------------| 0M == 0x0780_0000 == 120M
*/
#define FRAMEBUFFER_SIZE (2 * 1024 * 1024)
#define FRAMEBUFFER_START (0x07800000)
#define AUDIOBUFFER_SIZE (512 * 1024)
#define AUDIOBUFFER_START (FRAMEBUFFER_START + 3 * FRAMEBUFFER_SIZE)
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
static int play_num;   // Only be written by play thread
static int decode_num; // Only be written by decode thread
static int sample_perframe;
static int fps;
static struct i2s_ip_ctl *i2s_ctl;
static struct fb_ip_ctl  *fb_ctl;
static struct decode_ip_ctl *decode_mmio;
static uint32_t ab_array[4];

static void fb_play_one_frame(uint32_t fbuf_ptr) {
    fb_ctl->addr = fbuf_ptr;
}
static void open_i2s_device() {
    i2s_ctl->mm2s_ctrl = BIT(1);
    while(i2s_ctl->mm2s_ctrl && BIT(1));
    i2s_ctl->mm2s_ctrl = BIT(13) | (0x1 << 16) | (0x2 << 19);
    i2s_ctl->mm2s_multiplier = 512;
}
static void close_i2s_device() {
    i2s_ctl->mm2s_ctrl = BIT(1);
    while(i2s_ctl->mm2s_ctrl && BIT(1));
}
static void i2s_play_one_frame(uint32_t abuf_ptr) {
    i2s_ctl->mm2s_period = (1 << 16) | (sample_perframe);
    i2s_ctl->mm2s_dmaaddr = abuf_ptr;
    i2s_ctl->mm2s_dmaaddr_msb = 0;
    i2s_ctl->mm2s_channel_offset = sample_perframe / 2;
    i2s_ctl->mm2s_ctrl |= BIT(0);
}
static int play_one_frame(int fb_num)
{
    uint32_t vbuf_ptr = FRAMEBUFFER_START + FRAMEBUFFER_SIZE * fb_num;
    uint32_t abuf_ptr = ab_array[fb_num];
    i2s_play_one_frame(abuf_ptr);
    fb_play_one_frame(fbuf_ptr);
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
    printf("D%d\n", vframe_size);

    decode_mmio->ctrl = 0x40000000;

    // 配置 decode ip 的地址
    decode_mmio->src = ((uint32_t)jpeg_file_ptr) & 0x1fffffff;
    decode_mmio->dst = FRAMEBUFFER_START + FRAMEBUFFER_SIZE * fb_num;
    decode_mmio->stride = 1024;

    // 配置开始 decode ip 的解码
    decode_mmio->iocen = 1; // 打开中断输出，清理旧的中断
    decode_mmio->ctrl  = 0x80000000 | vframe_size;
    // printf("decoder_mmio %x %x %x\n", decode_mmio->src, decode_mmio->dst, decode_mmio->ctrl, decode_ptr);

    // 音频数据指针写入到音频指针缓冲区
    ab_array[fb_num] = ((uint32_t)(jpeg_file_ptr + vframe_size)) & 0x1fffffff;
    return 0;
}

// return 0 on finish.
static int get_next_frame_ptr(int init, void **binary)
{
    static sub_frame_cnt;
    static struct headerb *hdr_b;
    if(init || sub_frame_cnt >= 127) {
        sub_frame_cnt = 0;
        hdr_b = *binary;
        *binary += 512;
    } else {
        *binary += hdr_b->frame_size[sub_frame_cnt++];
    }
    return hdr_b->frame_size[sub_frame_cnt];
}

int wait_an_interrupt()
{
    while(1) {
        uint32_t irqs;
        asm volatile(
		"	csrrd	%0, csr_estat\n"
		: "=r"(irqs) :: "memory");
        irqs = (irqs >> 3) & 0x3;
        if(irqs) return irqs;
    }
}

static int mediaplayer(void *binary)
{
    int decode_ptr = 0, play_ptr = 0, finish_flag = 0, fifo_size = 0;
    int frame_size;
    // 初始化 I2S 控制器
    open_i2s_device();
    // 先解码四帧填满
    for(int i = 0 ; i < 4 ; i++) {
        frame_size = get_next_frame_ptr(i == 0, &binary);
        if(frame_size == 0) {
            finish_flag = 1;
            break;
        }
        decode_one_frame(frame_size,binary,decode_ptr++);
        wait_an_interrupt();
        decode_ptr &= 3;
        fifo_size++;
    }
    // 之后播放第一帧
    play_one_frame(play_ptr++);
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
        int irq = wait_an_interrupt();
        if(irq & 0x1) {
            // PLAY NEEDED
            if(fifo_size) {
                play_one_frame(play_ptr++);
                play_ptr &= 3;
                fifo_size --;
            } else {
                // FIFO UNDER FLOW
                iter++;
                if((iter & 0xffff) == 0) printf("UF\n");
            }
        }
        if((irq & 0x2) && !finish_flag) {
            // DECODE OK
            if(fifo_size < 3) {
                frame_size = get_next_frame_ptr(i == 0, &binary);
                if(frame_size == 0) {
                    finish_flag = 1;
                    continue;
                }
                decode_one_frame(decode_ptr++);
                decode_ptr &= 3;
                fifo_size += 1;
            } else {
                // FIFO OVER FLOW
                iter++;
                if((iter & 0xffff) == 0) printf("OF\n");
            }
        }
    }
    decode_mmio->iocen = 0; // 关闭中断输出，清理旧的中断
    close_i2s_device();
}

int do_playmedia(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if(argc < 2) {
		printf("Usage: playmedia [media_binary_location]\n");
		return 0;
	}
	uint32_t location;
	sscanf(argv[1], "0x%x", &location);
	if(location < 0xa0000000) {
		printf("Location should'nt be lower than 0xa0000000.\n");
		return 0;
	}
	return mediaplayer((void*)location);
}

U_BOOT_CMD(
	playmedia,	7,	0,	do_playmedia,
	"play a mjpeg file from a specified memory location.\n"
);