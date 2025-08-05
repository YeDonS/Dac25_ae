#!/bin/bash

# 修复编译问题脚本

echo "修复 conv_ftl.c 中的编译问题..."

# 1. 修复 init_slc_qlc_blocks 函数定义
sed -i '559s/static void$/static void init_slc_qlc_blocks(struct conv_ftl *conv_ftl)/' conv_ftl.c

# 2. 修复 init_lines_DA 函数定义
sed -i '151s/static void$/static void init_lines_DA(struct conv_ftl *conv_ftl)/' conv_ftl.c

# 3. 在 migrate_page_to_qlc 函数中，移除对 ktime_get_ns 的直接调用，使用 ssd 的时钟
sed -i 's/ktime_get_ns()/__get_ioclock(conv_ftl->ssd)/g' conv_ftl.c

# 4. 修复 update_heat_info 函数中的时间获取
sed -i 's/ktime_get_ns()/__get_ioclock(conv_ftl->ssd)/g' conv_ftl.c

# 5. 确保 old_ppa 被初始化
sed -i '/struct ppa old_ppa;/a\        old_ppa.ppa = UNMAPPED_PPA;' conv_ftl.c

echo "修复完成！"
echo ""
echo "现在尝试编译..."
make clean
make APPROACH=on

if [ $? -eq 0 ]; then
    echo "编译成功！"
else
    echo "编译失败，请检查错误信息。"
fi 