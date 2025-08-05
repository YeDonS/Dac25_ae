#!/bin/bash

echo "=== 修复ZNS编译问题 ==="

# 切换到正确目录
cd /home/femu/fast24_ae/nvmevirt_DA

# 备份原文件
echo "备份原文件..."
cp admin.c admin.c.backup
cp main.c main.c.backup

# 1. 修复admin.c - 将所有ZNS相关的条件编译改为永假
echo "修复admin.c中的ZNS条件编译..."

# 方法1: 直接替换条件编译为 #if 0
sed -i 's/#if (BASE_SSD == WD_ZN540)/#if 0 \/\/ Disabled ZNS support/g' admin.c

# 方法2: 如果上面不工作，使用更精确的替换
if grep -q "BASE_SSD.*WD_ZN540" admin.c; then
    echo "sed替换失败，使用备用方法..."
    
    # 使用perl进行更复杂的替换
    perl -i -pe 's/#if \(BASE_SSD == WD_ZN540\)/#if 0 \/\/ Disabled ZNS support/g' admin.c
fi

# 2. 修复main.c中的ZNS条件编译
echo "修复main.c中的ZNS条件编译..."
sed -i 's/#if (BASE_SSD == WD_ZN540)/#if 0 \/\/ Disabled ZNS support/g' main.c

if grep -q "BASE_SSD.*WD_ZN540" main.c; then
    perl -i -pe 's/#if \(BASE_SSD == WD_ZN540\)/#if 0 \/\/ Disabled ZNS support/g' main.c
fi

# 3. 验证修复结果
echo "验证修复结果..."
echo "=== admin.c中的条件编译 ==="
grep -n "#if.*Disabled ZNS\|BASE_SSD.*WD_ZN540" admin.c || echo "未找到问题条件编译"

echo "=== main.c中的条件编译 ==="
grep -n "#if.*Disabled ZNS\|BASE_SSD.*WD_ZN540" main.c || echo "未找到问题条件编译"

# 4. 额外的安全检查 - 如果还有问题，直接注释掉问题函数
if grep -q "struct zns_ftl\|struct nvme_id_zns" admin.c; then
    echo "⚠️  发现残留的ZNS结构体引用，进行额外修复..."
    
    # 创建临时修复脚本
    cat > temp_fix_admin.py << 'EOF'
import re

with open('admin.c', 'r') as f:
    content = f.read()

# 找到并替换有问题的函数定义
# 将整个ZNS函数块注释掉
content = re.sub(
    r'(#if.*\n)(static void __nvmev_admin_identify_zns_namespace.*?\n)(.*?)(static void __nvmev_admin_identify_zns_ctrl.*?\n)(.*?)(#endif)',
    r'#if 0 // Disabled ZNS functions\n\2\3\4\5\6',
    content,
    flags=re.DOTALL
)

with open('admin.c', 'w') as f:
    f.write(content)
EOF
    
    python3 temp_fix_admin.py
    rm temp_fix_admin.py
fi

# 5. 清理编译缓存并测试编译
echo "清理编译缓存..."
make clean

echo "测试编译..."
make APPROACH=on

if [ $? -eq 0 ]; then
    echo "✅ 编译成功！修复完成。"
else
    echo "❌ 编译仍然失败，尝试终极修复方案..."
    
    # 终极方案：完全移除ZNS相关代码
    echo "应用终极修复方案..."
    
    # 注释掉所有ZNS相关的case语句
    sed -i '/case 0x05:/,/break;/ { s/^/\/\/ /; }' admin.c
    sed -i '/case 0x06:/,/break;/ { s/^/\/\/ /; }' admin.c
    
    # 再次尝试编译
    make clean
    make APPROACH=on
    
    if [ $? -eq 0 ]; then
        echo "✅ 终极修复成功！"
    else
        echo "❌ 修复失败，请手动检查错误。"
        echo "备份文件位于: admin.c.backup 和 main.c.backup"
    fi
fi

echo "=== 修复脚本执行完成 ===" 