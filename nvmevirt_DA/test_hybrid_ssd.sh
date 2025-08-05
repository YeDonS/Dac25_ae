#!/bin/bash

# 测试混合 SLC/QLC 存储功能
# 作者：混合存储测试脚本

echo "========================================="
echo "混合 SLC/QLC 存储测试脚本"
echo "========================================="

# 编译模块（使用 DIEAFFINITY=1 开启 Die Affinity）
echo "编译 NVMeVirt 模块..."
make clean
make APPROACH=on

# 加载模块
echo "加载 NVMeVirt 模块..."
rmmod nvmev 2>/dev/null || true
insmod nvmev_on.ko memmap_start=16G memmap_size=10G cpus=0,1,2,3

# 等待设备创建
sleep 2

# 检查设备
echo "检查 NVMe 设备..."
lsblk | grep nvme

# 获取设备名（假设是最后一个 nvme 设备）
DEVICE=$(lsblk | grep nvme | tail -1 | awk '{print $1}')
DEVICE="/dev/${DEVICE}"

echo "使用设备: $DEVICE"

# 创建文件系统
echo "创建文件系统..."
mkfs.ext4 -F $DEVICE

# 挂载设备
MOUNT_POINT="/mnt/test_hybrid"
mkdir -p $MOUNT_POINT
mount $DEVICE $MOUNT_POINT

# 测试场景1：顺序写入（应该写入 SLC）
echo ""
echo "测试场景1：顺序写入测试（写入 SLC）"
echo "========================================="
dd if=/dev/zero of=$MOUNT_POINT/test_seq.dat bs=4K count=10000 oflag=direct
sync

# 测试场景2：随机写入（应该写入 SLC）
echo ""
echo "测试场景2：随机写入测试（写入 SLC）"
echo "========================================="
fio --name=randwrite --ioengine=libaio --rw=randwrite --bs=4k --direct=1 \
    --size=100M --numjobs=1 --runtime=30 --time_based --filename=$MOUNT_POINT/test_rand.dat

# 测试场景3：读取测试（从 SLC 读取）
echo ""
echo "测试场景3：顺序读取测试"
echo "========================================="
dd if=$MOUNT_POINT/test_seq.dat of=/dev/null bs=4K count=10000 iflag=direct

# 测试场景4：混合读写（触发迁移）
echo ""
echo "测试场景4：混合读写测试（可能触发 SLC 到 QLC 迁移）"
echo "========================================="
fio --name=randrw --ioengine=libaio --rw=randrw --bs=4k --direct=1 \
    --size=100M --numjobs=1 --runtime=60 --time_based --filename=$MOUNT_POINT/test_mixed.dat \
    --rwmixread=70

# 查看 dmesg 日志
echo ""
echo "查看内核日志（检查 SLC/QLC 操作）"
echo "========================================="
dmesg | grep -E "(SLC|QLC|Migration|Write Stats)" | tail -20

# 清理
echo ""
echo "清理测试环境..."
umount $MOUNT_POINT
rmmod nvmev

echo ""
echo "测试完成！"
echo "=========================================" 