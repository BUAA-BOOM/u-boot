# QEMU / Chiplab 构建命令
export DEVICE_TREE=la32rsoc_demo
export ARCH=la32r
export CROSS_COMPILE=loongarch32r-linux-gnusf-
clear && make la32rsoc_defconfig && make -j && loongarch32r-linux-gnusf-objdump -S u-boot > u-boot.S

# MegaSoC 构建命令
export DEVICE_TREE=la32rmega_demo; \
export ARCH=la32r; \
export CROSS_COMPILE=loongarch32r-linux-gnusf-; \
clear && make la32rmega_defconfig && make -j && loongarch32r-linux-gnusf-objdump -S u-boot > u-boot.S; \
loongarch32r-linux-gnusf-objcopy ./u-boot -O binary u-boot.bin

# WiredSoc 构建命令
export DEVICE_TREE=wired_demo; \
export ARCH=la32r; \
export CROSS_COMPILE=loongarch32r-linux-gnusf-; \
clear && make wired_defconfig && make -j && loongarch32r-linux-gnusf-objdump -S u-boot > u-boot.S; \
loongarch32r-linux-gnusf-objcopy ./u-boot -O binary u-boot.bin


ffmpeg -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 -s 640x480 -framerate 24 -pattern_type glob -i '*.png' -c:a aac  -c:v libx264 -pix_fmt yuv420p  -shortest out.mp4