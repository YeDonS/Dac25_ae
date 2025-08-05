# 混合 SLC/QLC 存储配置说明

本文档说明了 NVMeVirt 混合存储模拟器的配置参数和使用方法。

## 配置参数说明

### 1. 基本配置 (ssd_config.h)

#### 容量分配
```c
#define SLC_CAPACITY_PERCENT (20)  /* SLC 占总容量的 20% */
#define QLC_CAPACITY_PERCENT (80)  /* QLC 占总容量的 80% */
#define QLC_REGIONS (4)            /* QLC 分为 4 个区域 */
```

#### 延迟参数
```c
/* QLC 4KB 读延迟：约为 SLC 的 1.5-4.5 倍 */
#define QLC_4KB_READ_LATENCY_TOP    ((35760 - 6000) * 25 / 10) // 2.5x
#define QLC_4KB_READ_LATENCY_UPPER  ((35760 + 6000) * 27 / 10) // 2.7x  
#define QLC_4KB_READ_LATENCY_MIDDLE ((35760 + 6000) * 28 / 10) // 2.8x
#define QLC_4KB_READ_LATENCY_LOWER  ((35760 + 6000) * 30 / 10) // 3.0x

/* QLC 页读延迟：约为 SLC 的 1.5-4.5 倍 */
#define QLC_READ_LATENCY_TOP    ((36013 - 6000) * 25 / 10) // 2.5x
#define QLC_READ_LATENCY_UPPER  ((36013 + 6000) * 27 / 10) // 2.7x
#define QLC_READ_LATENCY_MIDDLE ((36013 + 6000) * 28 / 10) // 2.8x  
#define QLC_READ_LATENCY_LOWER  ((36013 + 6000) * 30 / 10) // 3.0x

/* QLC 写延迟：约为 SLC 的 4 倍 */
#define QLC_PROG_LATENCY (185000 * 4)

/* QLC 擦除延迟：约为 SLC 的 3 倍 */  
#define QLC_ERASE_LATENCY (3000000) // 3ms
```

#### 迁移配置
```c
#define MIGRATION_THRESHOLD (10)   /* 访问次数迁移阈值 */
#define MIGRATION_LATENCY (NAND_READ_LATENCY_LSB + QLC_PROG_LATENCY)
```

## 工作原理

### 1. 写入策略
- **初次写入**：所有新数据都先写入 SLC
- **Die 轮询**：SLC 写入使用 advance_write_pointer_DA（Die Affinity）
- **性能优势**：利用 SLC 的高速写入特性

### 2. 读取策略
- **延迟区分**：根据数据位置（SLC/QLC）使用不同延迟
- **热数据跟踪**：记录页面访问次数和最后访问时间
- **性能优化**：SLC 数据读取速度更快

### 3. 迁移策略
- **冷数据检测**：长时间未访问且访问次数低于阈值的数据
- **QLC 写入**：迁移时使用 advance_write_pointer（顺序写入）
- **区域轮询**：QLC 分为 4 个区域，轮询写入

### 4. QLC 特性
- **页数关系**：QLC 每个块的页数是 SLC 的 4 倍
- **延迟特性**：
  - 读延迟：约为 SLC 的 1.5-4.5 倍
  - 写延迟：约为 SLC 的 4 倍
  - 擦除延迟：约为 SLC 的 3 倍

## 配置调整指南

### 1. 容量比例调整
如果需要调整 SLC/QLC 容量比例，修改：
```c
#define SLC_CAPACITY_PERCENT (30)  // 改为 30%
#define QLC_CAPACITY_PERCENT (70)  // 改为 70%
```

### 2. 延迟参数调整
可以根据实际硬件特性调整延迟倍数：
```c
#define QLC_PROG_LATENCY (185000 * 5)  // 改为 5 倍
```

### 3. 迁移阈值调整
调整迁移触发条件：
```c
#define MIGRATION_THRESHOLD (20)   // 提高迁移阈值
```

### 4. QLC 区域数调整
```c
#define QLC_REGIONS (8)            // 改为 8 个区域
```

## 编译和测试

### 1. 编译
```bash
make clean
make APPROACH=on
```

### 2. 加载模块
```bash
sudo insmod nvmev_on.ko memmap_start=16G memmap_size=10G cpus=0,1,2,3
```

### 3. 运行测试
```bash
chmod +x test_hybrid_ssd.sh
sudo ./test_hybrid_ssd.sh
```

### 4. 查看日志
```bash
dmesg | grep -E "(SLC|QLC|Migration|Write Stats)"
```

## 预期输出

### 1. 初始化日志
```
[nvmev] SLC blocks per plane: 1638 (20%), QLC blocks per plane: 6554 (80%)
[nvmev] Heat tracking initialized for 134217728 pages
[nvmev] SLC/QLC Hybrid Mode: SLC 1638 blks, QLC 6554 blks (4 regions)
```

### 2. 运行时统计
```
[nvmev] Write Stats: SLC writes=10000, QLC writes=0, Migrations=150
[nvmev] Migrated LPN 12345 from SLC to QLC region 2
```

## 性能预期

- **写入性能**：初期保持 SLC 级别的高性能
- **读取性能**：SLC 数据读取快，QLC 数据读取慢
- **容量利用**：20% SLC 用于热数据，80% QLC 用于冷数据
- **迁移开销**：后台自动迁移，对前台 I/O 影响最小

## 故障排除

### 1. 编译错误
- 确保内核头文件正确安装
- 检查 DIEAFFINITY 宏定义

### 2. 运行时错误
- 检查内存预留是否足够
- 确认 CPU 绑定设置正确

### 3. 性能异常
- 查看 dmesg 日志确认迁移是否正常工作
- 调整迁移阈值参数 