/*
 * (C) Copyright 2023
 * Wang Zhe, dofingert@gmail.com
 */

#include <common.h>
#include <command.h>
#include <mapmem.h>
#include <asm/cache.h>
#include <linux/bitops.h>

struct vcfg_t{
    uint32_t hcfg[4];
    uint32_t vcfg[4];
};

extern uint32_t support_resolution[];
extern struct vcfg_t support_cfg[];

// rgb565-color_mode == 3
extern int set_fb_args(struct vcfg_t use_cfg, int color_mode);

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
    volatile uint32_t length;
    volatile uint32_t hcfg[4];
    volatile uint32_t vcfg[4];
};


static inline uint32_t padding(uint32_t in, uint32_t align)
{
    return ((in + (align-1)) / align) * align;
}
struct headerb
{
    uint32_t frame_size[128];
};

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
        uint32_t aframe_size = padding(2000 * 4, 512);
        *binary += padding(hdr_b->frame_size[sub_frame_cnt++], 512) + aframe_size;
    }
    if(sub_frame_cnt >= 128) {
        sub_frame_cnt = 0;
        hdr_b = *binary;
        *binary += 512;
    }
    return hdr_b->frame_size[sub_frame_cnt];
}


static struct i2s_ip_ctl *i2s_ctl = (void*) 0x9d0b0000;
static struct fb_ip_ctl  *fb_ctl = (void*) 0x9d0d0000;
static struct decode_ip_ctl *decode_ctl = (void*) 0x9d0a0000;
static uint32_t frame_size_y, frame_size_x, dec_color_mode;
static uint32_t framebuffer_size = 1179648; // 1152KB == 4K * 288
#define FRAMEBUFFER_START (0x0F000000)
static uint32_t *ab_array[4];

static void fb_play_one_frame(int height) {
    uint32_t fbuf_ptr = FRAMEBUFFER_START + frame_size_x * height * 2;
    fb_ctl->addr = fbuf_ptr;
    fb_ctl->status = BIT(1);
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
    i2s_ctl->mm2s_ctrl = 0;
}

static void i2s_play_one_frame(uint32_t abuf_ptr) {
    i2s_ctl->mm2s_dmaaddr = abuf_ptr;
    i2s_ctl->mm2s_dmaaddr_msb = 0;
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
    decode_ctl->dst = FRAMEBUFFER_START + framebuffer_size * fb_num;
    decode_ctl->stride = frame_size_x * 2; // FIXED 1280x720p @ 16bits, but resolution might be various.

    // 配置开始 decode ip 的解码
    decode_ctl->iocen = 1 | ((0 & 1) << 1); // 打开中断输出，清理旧的中断
    decode_ctl->ctrl  = 0x80000000 | vframe_size;
    printf("decoder_mmio %x %x %x %x\n", decode_ctl->src, decode_ctl->dst, decode_ctl->ctrl, decode_ctl);

    return 0;
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
        if((iters & 0xfffff) == 0) {
            printf("NIR%d,%x\n",iters, estate);
            if(!forever) return 0;
            forever--;
        }
    }
}

struct la_slides_header
{
    uint32_t magic;
    int frame_count;
    uint32_t display_mode;
};

static struct la_slides_header* header;
static struct la_slides_header header_static;

// 之后使用数组记录所有帧开始偏移及长度。
struct la_slides_frame_desc
{
    uint32_t frame_offset;
    uint32_t frame_size;
};

static struct la_slides_frame_desc* descs;

// 初始化文件信息的函数，输入二进制文件，打印调试信息，返回总帧数
static inline uint32_t init_slide(void *binary) {
    void *base = binary;
    header = &header_static;
    header->display_mode = 480;
    header->frame_count = 0;
    // descs = (void*)(header + 1);
    // printf("Header in %x, desc in %x\n", (uint32_t)header, (uint32_t)descs);

    // Set framebuffer
    printf("Display mode %dp\n", header->display_mode);
    struct vcfg_t *use_cfg = &support_cfg[1]; // Default == 1024x576
    for(int i = 0 ; i < 4; i++) {
        if(header->display_mode == support_resolution[i]) {
            use_cfg = &support_cfg[i];
            break;
        }
        if(i == 3) {
            printf("Display mode %d Not found! use 1024p\n", header->display_mode);
        }
    }
    frame_size_x = use_cfg->hcfg[2];
    frame_size_y = use_cfg->vcfg[2];
    framebuffer_size = frame_size_x * frame_size_y * 2;
    printf("Using %dx%d with size %d\n", frame_size_x, frame_size_y, framebuffer_size);
    set_fb_args(*use_cfg, 3);

    // 分配 descs 区域
    descs = (void *)(0xa0000000 + FRAMEBUFFER_START + framebuffer_size * 3);
    int f = 0;
    while(1) {
        int frame_size = get_next_frame_ptr(f == 0, &binary);
        descs[f].frame_offset = binary - base;
        descs[f].frame_size = frame_size;
        if(frame_size == 0) break;
        f++;
        // printf("Detect %d frames!\n", f);
    }
    header->frame_count = f;
    printf("Detect %d frames!\n", f);
    return 0;
}

static void wait_decode() {
    wait_an_interrupt(0x2, 10);
}

static int get_tick() {
    unsigned int tim_low;
    asm volatile(
        "	rdcntvl.w	%0\n"
        :"=r"(tim_low):
        );
    return tim_low;
}

static inline int squart_interpole(int total, int now, int value) {
    // t in [0, 1]
    // t = now / total
    // f(0) = 0 * value, f(1) = 1 * value, f(1)' = 0;
    // f(t) = 2t - t^2
    // f(t) = 2 * now / total - now * now / (total * total)
    // f(t) * value = 2 * now * value / total - (now >> 16) * (now >> 16) * value / ((total >> 16) * (total >> 16))
    // In case of over flow, do a right shift.
    // uint32_t ret = 2 * now * value / total;
    // uint32_t now_2_hi = ((unsigned long long)now * now) >> 32;
    // uint32_t total_2_hi = ((unsigned long long)total * total) >> 32;
    // ret -= (now_2_hi >> 10) * value / (total_2_hi >> 10);
    float t = (float)now / (float)total;
    // float v = (float)value * (2.0f * t - t * t);
    if (t < (1/2.75)) {
        return 7.5625*t*t;
    }
    else if (t < (2/2.75)) {
        t -= (1.5/2.75);
        return 7.5625*t*t + 0.75;
    } 
    else if (t < (2.5/2.75)) {
        t -= (2.25/2.75);
        return 7.5625*t*t + 0.9375;
    } 
    else {
        t -= (2.625/2.75);
        return 7.5625*t*t + 0.984375;
    }
    return (int) v;
}

// 显示一页新的 PPT，dir 表示滚动方向。
// dir == -1，滚动向低地址方向。 framebuffer 1 切换到 framebuffer 0，在 0 处生成目标图像。
// 当前帧永远在 y=576 位置
// ticks 表示弹跳动画持续时间（CPU 周期数）
static inline void display_one_page(void *binary, int index, int dir, int ticks) {
    static int last_index = -1;
    // 解码目标帧
    int origin_height = frame_size_y * 1; // 原始位置
    int target_height;
    if(last_index == -1) {
        // 第一次，target == origin
        target_height = origin_height;
    } else {
        // 之后每一次，根据 dir 计算位置
        target_height = origin_height + dir * frame_size_y;
    }

    // 解码新帧到 target_height
    printf("Page%d info: offset=%x, size=%d, dir=%d\n",index, descs[index].frame_offset, descs[index].frame_size, dir);
    decode_one_frame(descs[index].frame_size, binary + descs[index].frame_offset, 1 + dir);
    wait_decode();

    // 开始播放插值动画，读取时间，计算，动画播放。
    uint32_t start_time = get_tick();
    uint32_t last_ptr = 0;
    while(1) {
        uint32_t now_time = get_tick();
        uint32_t diff = now_time - start_time;
        if(diff >= ticks) break;
        uint32_t ptr_position = squart_interpole(ticks, diff, frame_size_y);
        if(ptr_position - last_ptr > 1) {
            fb_play_one_frame(origin_height + dir * ptr_position);
            last_ptr = ptr_position;
        }
    }
    fb_play_one_frame(target_height);


    if(target_height != origin_height) {
        // 还需要挪回去
        decode_one_frame(descs[index].frame_size, binary + descs[index].frame_offset, 1);
        wait_decode();
        fb_play_one_frame(origin_height);
    }
    last_index = index;
}

static int do_playslides(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[]) {
    if(argc < 2) {
        printf("Usage: <slide address> [animation_ticks].\n");
        return 0;
    }
    decode_ctl->iocen = 0; // 关闭中断输出，清理旧的中断
    uint32_t binary_addr, animation_ticks = 100000000;
    sscanf(argv[1], "%x", &binary_addr);
    if(argc >= 3) sscanf(argv[2], "%d", &animation_ticks);
    void * binary = (void*)binary_addr;
    init_slide(binary);
    display_one_page(binary, 0, 0, animation_ticks);
    int wrong = 0;
    int now_index = 0;
    while(1) {
        char c;
        int dir = 0;
        int next_index = now_index;
        // scanf("%c", &c);
        c = fgetc(stdin);
        if(c == 'w') { // 上一页
            wrong = 0;
            dir = -1;
        } else if(c == 's') { // 下一页
            wrong = 0;
            dir = 1;
        } else if(c == 'q') { // 退出
            return 0;
        } else {
            if(!wrong) printf("Wrong input '%c'! use w/s to page up/down, q to quit\n", c);
            wrong = 1;
        }
        next_index += dir;
        if(next_index >= header->frame_count) {
            printf("Last slide.\n");
            next_index = 0;
        }
        if(next_index < 0) {
            printf("First slide.\n");
            next_index = header->frame_count - 1;
        }
        if(dir) display_one_page(binary, next_index, dir, animation_ticks);
        now_index = next_index;
    }
    decode_ctl->iocen = 0; // 关闭中断输出，清理旧的中断
}

U_BOOT_CMD(
    playslides, 10, 0,  do_playslides,
    "set framebuffer starting address and size.\n",
    "0x<start address> <size in byte>"
)
