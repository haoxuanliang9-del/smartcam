#!/bin/bash
# WSL 端 RTSP 性能测试脚本
# 从 WSL 开客户端连接香橙派，同时在远端采集 CPU/内存

RTSP_URL="rtsp://100.111.77.78:8554/live"
ORANGEPI="orangepi@100.111.77.78"
DURATION=15
SAMPLE_INTERVAL=1
RESULT_DIR="/tmp/rtsp_bench_wsl"

mkdir -p "$RESULT_DIR"

# 在香橙派上采集 smartcam 进程的 CPU/内存
start_remote_collect() {
    local label=$1
    local outfile="$RESULT_DIR/${label}_server.csv"
    sshpass -p orangepi ssh -o StrictHostKeyChecking=no $ORANGEPI \
        "pid=\$(pgrep -x smartcam); \
         echo 'timestamp,cpu_pct,mem_mb,threads,label' > /tmp/rtsp_bench/${label}_server.csv; \
         for i in \$(seq 1 $DURATION); do \
           stats=\$(ps -p \$pid -o %cpu,rss,nlwp --no-headers 2>/dev/null); \
           if [ -n \"\$stats\" ]; then \
             cpu=\$(echo \$stats | awk '{print \$1}'); \
             mem_kb=\$(echo \$stats | awk '{print \$2}'); \
             mem_mb=\$(echo \"scale=1; \$mem_kb / 1024\" | bc); \
             thr=\$(echo \$stats | awk '{print \$3}'); \
             echo \"\$(date +%s),\$cpu,\$mem_mb,\$thr,$label\" >> /tmp/rtsp_bench/${label}_server.csv; \
           fi; \
           sleep $SAMPLE_INTERVAL; \
         done" &
    REMOTE_COLLECT_PID=$!
}

# 停止远端采集
stop_remote_collect() {
    wait $REMOTE_COLLECT_PID 2>/dev/null
}

# 拉取远端采集数据
fetch_remote_data() {
    local label=$1
    sshpass -p orangepi scp -o StrictHostKeyChecking=no \
        $ORANGEPI:/tmp/rtsp_bench/${label}_server.csv \
        "$RESULT_DIR/${label}_server.csv" 2>/dev/null
}

# 启动 N 个 WSL 端 ffplay 客户端
start_clients() {
    local n=$1
    for i in $(seq 1 $n); do
        ffplay -nodisp -nostats -autoexit -loglevel quiet "$RTSP_URL" &
        echo $! >> /tmp/wsl_rtsp_clients.pids
    done
    sleep 3
}

# 停止所有 WSL 客户端
stop_clients() {
    if [ -f /tmp/wsl_rtsp_clients.pids ]; then
        while read pid; do
            kill $pid 2>/dev/null
        done < /tmp/wsl_rtsp_clients.pids
        rm -f /tmp/wsl_rtsp_clients.pids
    fi
    pkill -f "ffplay.*$RTSP_URL" 2>/dev/null
    sleep 2
}

echo "========================================"
echo "WSL -> 香橙派 RTSP 性能基准测试"
echo "URL: $RTSP_URL"
echo "每轮测试: ${DURATION}秒"
echo "========================================"

# 基线
echo ""
echo "[基线] 采集无客户端时的资源占用..."
start_remote_collect "baseline"
stop_remote_collect
fetch_remote_data "baseline"

# 依次测试
for n in 1 3 5 10; do
    echo ""
    echo "[${n}客户端] 从 WSL 启动 $n 个 RTSP 客户端..."
    stop_clients
    start_clients $n
    echo "[${n}客户端] 采集数据中（${DURATION}秒）..."
    start_remote_collect "${n}clients"
    stop_remote_collect
    fetch_remote_data "${n}clients"
    echo "[${n}客户端] 完成"
    stop_clients
    sleep 3
done

stop_clients

echo ""
echo "========================================"
echo "测试完成！"
echo "========================================"

# 汇总
echo ""
echo "=== 香橙派服务端性能汇总（WSL 远程客户端）==="
echo ""
printf "%-15s %-10s %-10s %-10s\n" "场景" "CPU(%)" "内存(MB)" "线程数"
echo "------------------------------------------------------"

for label in baseline 1clients 3clients 5clients 10clients; do
    f="$RESULT_DIR/${label}_server.csv"
    if [ -f "$f" ]; then
        avg_cpu=$(tail -n +2 "$f" | awk -F, '{sum+=$2; count++} END {if(count>0) printf "%.1f", sum/count; else print "N/A"}')
        avg_mem=$(tail -n +2 "$f" | awk -F, '{sum+=$3; count++} END {if(count>0) printf "%.1f", sum/count; else print "N/A"}')
        avg_thr=$(tail -n +2 "$f" | awk -F, '{sum+=$4; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')
        display_name=$(echo $label | sed 's/clients/ 客户端/' | sed 's/baseline/基线(0)/')
        printf "%-15s %-10s %-10s %-10s\n" "$display_name" "$avg_cpu" "$avg_mem" "$avg_thr"
    fi
done
