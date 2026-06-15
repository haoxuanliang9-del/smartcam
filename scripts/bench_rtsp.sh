#!/bin/bash
# RTSP 性能测试脚本
# 依次模拟 1/3/5/10 个客户端，采集 CPU 和内存数据

RTSP_URL="rtsp://127.0.0.1:8554/live"
DURATION=15          # 每轮测试持续秒数
SAMPLE_INTERVAL=1    # 采样间隔秒数
RESULT_DIR="/tmp/rtsp_bench"
PID_FILE="/tmp/smartcam.pid"

mkdir -p "$RESULT_DIR"

# 获取 smartcam 进程 PID（精确匹配可执行文件）
get_smartcam_pid() {
    pgrep -x "smartcam" | head -1
}

# 采集 CPU 和内存
collect_stats() {
    local label=$1
    local outfile="$RESULT_DIR/${label}.csv"
    local pid=$(get_smartcam_pid)

    if [ -z "$pid" ]; then
        echo "ERROR: smartcam not running"
        return 1
    fi

    echo "timestamp,cpu_pct,mem_mb,threads,clients" > "$outfile"

    local count=0
    while [ $count -lt $DURATION ]; do
        local stats=$(ps -p $pid -o %cpu,rss,nlwp --no-headers 2>/dev/null)
        if [ -n "$stats" ]; then
            local cpu=$(echo $stats | awk '{print $1}')
            local mem_kb=$(echo $stats | awk '{print $2}')
            local mem_mb=$(echo "scale=1; $mem_kb / 1024" | bc)
            local threads=$(echo $stats | awk '{print $3}')
            echo "$(date +%s),$cpu,$mem_mb,$threads,$label" >> "$outfile"
        fi
        sleep $SAMPLE_INTERVAL
        count=$((count + 1))
    done
}

# 启动 N 个 ffplay 客户端（静音模式，不显示视频窗口）
start_clients() {
    local n=$1
    for i in $(seq 1 $n); do
        ffplay -nodisp -nostats -autoexit -loglevel quiet "$RTSP_URL" &
        echo $! >> /tmp/rtsp_clients.pids
    done
    sleep 2  # 等待客户端连接
}

# 停止所有客户端
stop_clients() {
    if [ -f /tmp/rtsp_clients.pids ]; then
        while read pid; do
            kill $pid 2>/dev/null
        done < /tmp/rtsp_clients.pids
        rm -f /tmp/rtsp_clients.pids
    fi
    # 确保清理所有 ffplay
    pkill -f "ffplay.*$RTSP_URL" 2>/dev/null
    sleep 2
}

echo "========================================"
echo "RTSP 性能基准测试"
echo "URL: $RTSP_URL"
echo "每轮测试: ${DURATION}秒"
echo "========================================"

# 先采集基线（无客户端）
echo ""
echo "[基线] 采集无客户端时的资源占用..."
collect_stats "baseline"

# 依次测试不同客户端数量
for n in 1 3 5 10; do
    echo ""
    echo "[${n}客户端] 启动 $n 个 RTSP 客户端..."
    stop_clients
    start_clients $n
    echo "[${n}客户端] 采集数据中（${DURATION}秒）..."
    collect_stats "${n}clients"
    echo "[${n}客户端] 完成"
    stop_clients
    sleep 3  # 间隔
done

stop_clients

echo ""
echo "========================================"
echo "测试完成！结果保存在 $RESULT_DIR/"
echo "========================================"

# 汇总结果
echo ""
echo "=== 性能汇总 ==="
echo ""
printf "%-15s %-10s %-10s %-10s %-10s\n" "场景" "CPU(%)" "内存(MB)" "线程数" "客户端"
echo "--------------------------------------------------------------"

for label in baseline 1clients 3clients 5clients 10clients; do
    f="$RESULT_DIR/${label}.csv"
    if [ -f "$f" ]; then
        avg_cpu=$(tail -n +2 "$f" | awk -F, '{sum+=$2; count++} END {if(count>0) printf "%.1f", sum/count; else print "N/A"}')
        avg_mem=$(tail -n +2 "$f" | awk -F, '{sum+=$3; count++} END {if(count>0) printf "%.1f", sum/count; else print "N/A"}')
        avg_thr=$(tail -n +2 "$f" | awk -F, '{sum+=$4; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')
        display_name=$(echo $label | sed 's/clients/ 客户端/' | sed 's/baseline/基线(0客户端)/')
        printf "%-15s %-10s %-10s %-10s %-10s\n" "$display_name" "$avg_cpu" "$avg_mem" "$avg_thr" "$label"
    fi
done
