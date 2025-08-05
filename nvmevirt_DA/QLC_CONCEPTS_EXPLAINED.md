# QLC 存储中的两个"4"概念解释

在混合 SLC/QLC 存储实现中，有两个不同的"4"概念，它们服务于不同的目的：

## 概念 1：QLC_REGIONS (存储区域划分)

### 定义位置：`ssd_config.h`
```c
#define QLC_REGIONS (4)  /* QLC 分为 4 个区域 */
```

### 作用：
- **存储管理**：将 QLC 存储空间划分为 4 个独立的区域
- **并发写入**：支持多个区域并行写入，提高性能
- **负载均衡**：通过轮询方式在不同区域间分散写入负载
- **Write Pointer 管理**：每个区域有独立的写指针和 line 管理

### 使用场景：
- 迁移时选择目标 QLC 区域
- QLC 写指针轮询
- Line 管理和垃圾收集




## 概念 2：QLC_CELL_LEVELS (单元类型)

### 定义位置：`ssd_config.h` + `ssd.h`
```c
// ssd_config.h
#define QLC_CELL_LEVELS (4)  /* QLC 有 4 种单元类型 */

// ssd.h  
enum { 
    QLC_CELL_TYPE_TOP = CELL_TYPE_MAX,    /* QLC TOP page */
    QLC_CELL_TYPE_UPPER,                   /* QLC UPPER page */  
    QLC_CELL_TYPE_MIDDLE,                  /* QLC MIDDLE page */
    QLC_CELL_TYPE_LOWER,                   /* QLC LOWER page */
    MAX_QLC_CELL_TYPES
};
```

### 作用：
- **物理特性**：QLC NAND Flash 单元的 4 种不同状态
- **延迟差异**：不同页面类型具有不同的读写延迟
- **真实模拟**：模拟真实 QLC 硬件的性能特性
- **延迟计算**：根据页面在块中的位置确定延迟类型

### 使用场景：
- 计算 QLC 读延迟：`spp->qlc_pg_rd_lat[qlc_cell]`
- 确定页面类型：`uint32_t qlc_cell = ppa->g.pg % QLC_CELL_LEVELS`




## 概念 3：QLC_PAGES_PER_BLOCK_MULTIPLIER (页数倍数)

### 定义位置：`ssd_config.h`
```c
#define QLC_PAGES_PER_BLOCK_MULTIPLIER (4)  /* QLC 每块页数是 SLC 的 4 倍 */
```

### 作用：
- **容量特性**：QLC 每个物理块比 SLC 块能存储更多页面
- **地址映射**：影响 PPA 地址计算和块管理
- **容量计算**：影响总存储容量和空间分配

### 使用场景：
- QLC 写指针推进：`uint32_t qlc_pgs_per_blk = spp->pgs_per_blk * QLC_PAGES_PER_BLOCK_MULTIPLIER`
- 块满判断：`if (wp->pg != qlc_pgs_per_blk)`




## 配置关系图

```
混合 SLC/QLC 存储
├── SLC 区域 (20%)
│   ├── 使用原有 pgs_per_blk
│   └── Die Affinity 写入策略
│
└── QLC 区域 (80%)
    ├── 区域划分：QLC_REGIONS = 4
    │   ├── Region 0: qlc_wp[0], qlc_lm[0]
    │   ├── Region 1: qlc_wp[1], qlc_lm[1]  
    │   ├── Region 2: qlc_wp[2], qlc_lm[2]
    │   └── Region 3: qlc_wp[3], qlc_lm[3]
    │
    ├── 单元类型：QLC_CELL_LEVELS = 4
    │   ├── TOP: 最快读延迟
    │   ├── UPPER: 中等读延迟  
    │   ├── MIDDLE: 较慢读延迟
    │   └── LOWER: 最慢读延迟
    │
    └── 容量特性：QLC_PAGES_PER_BLOCK_MULTIPLIER = 4
        └── 每块页数 = SLC 的 4 倍
```




## 为什么需要 ssd.h 中的 Cell Type 定义？

虽然在 `ssd_config.h` 中定义了 `QLC_CELL_LEVELS (4)`，但 `ssd.h` 中的枚举定义仍然必要：

### 1. 类型安全
```c
// 有了枚举，编译器可以进行类型检查
enum { QLC_CELL_TYPE_TOP, QLC_CELL_TYPE_UPPER, ... };
int qlc_pg_rd_lat[MAX_QLC_CELL_TYPES];  // 数组大小明确
```

### 2. 代码可读性
```c
// 更清晰的代码
if (qlc_cell == QLC_CELL_TYPE_TOP) { ... }
// 而不是
if (qlc_cell == 0) { ... }
```

### 3. 延迟数组索引
```c
// 直接用作数组索引
nand_etime = nand_stime + spp->qlc_pg_rd_lat[qlc_cell];
```

### 4. 扩展性
如果将来需要支持其他类型的 NAND（如 PLC 等），枚举定义提供了更好的扩展性。




## 配置调整指南

### 调整 QLC 区域数量
```c
#define QLC_REGIONS (8)  // 增加到 8 个区域以提高并发性
```
**影响**：需要相应调整 `conv_ftl.h` 中的数组大小：
```c
struct write_pointer qlc_wp[QLC_REGIONS];
struct line_mgmt qlc_lm[QLC_REGIONS];
```

### 调整页数倍数
```c
#define QLC_PAGES_PER_BLOCK_MULTIPLIER (8)  // 改为 8 倍
```
**影响**：影响 QLC 总容量和地址映射

### 调整单元类型数量
```c
#define QLC_CELL_LEVELS (2)  // 简化为 2 种类型
```
**影响**：需要相应调整 `ssd.h` 中的枚举定义和延迟数组大小




## 总结

- **QLC_REGIONS**：存储管理层面的区域划分
- **QLC_CELL_LEVELS**：物理硬件层面的单元类型  
- **QLC_PAGES_PER_BLOCK_MULTIPLIER**：容量特性的倍数关系

这三个"4"分别控制不同方面的系统行为，互相独立但协同工作，共同实现了完整的混合 SLC/QLC 存储模拟。 