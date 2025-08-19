#!/bin/bash

# 混合存储状态监控脚本
LOG_FILE="/tmp/hybrid_monitor.log"
INTERVAL=2

echo "开始监控混合存储状态..." | tee $LOG_FILE
echo "时间,SLC写入数,QLC写入数,迁移数,后台线程状态" | tee -a $LOG_FILE

while true; do
    timestamp=$(date '+%H:%M:%S')
    
    # 检查后台线程状态
    migration_thread=$(ps aux | grep nvmev_migration | grep -v grep | wc -l)
    gc_thread=$(ps aux | grep nvmev_gc | grep -v grep | wc -l)
    
    # 从dmesg获取统计信息（这需要在实际内核模块中添加调试输出）
    slc_writes=$(dmesg | grep "SLC write" | wc -l)
    qlc_writes=$(dmesg | grep "QLC write" | wc -l) 
    migrations=$(dmesg | grep "Migrated.*pages" | wc -l)
    
    # 检查是否有迁移活动
    recent_migration=$(dmesg | tail -10 | grep -i "migration\|migrate" | wc -l)
    recent_gc=$(dmesg | tail -10 | grep -i "gc\|garbage" | wc -l)
    
    status="Migration:$migration_thread,GC:$gc_thread"
    if [ $recent_migration -gt 0 ]; then
        status="$status,MIGRATING"
    fi
    if [ $recent_gc -gt 0 ]; then
        status="$status,GC_ACTIVE"  
    fi
    
    echo "$timestamp,$slc_writes,$qlc_writes,$migrations,$status" | tee -a $LOG_FILE
    
    sleep $INTERVAL
done
