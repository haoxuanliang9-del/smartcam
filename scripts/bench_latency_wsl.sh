#!/bin/bash
# smartcam 延迟基准测试 - WSL 端编排脚本
# 测量：端到端 P99 延迟、服务器处理 P99 延迟、CPU 占用
#
# 前提：
#   1. smartcam 已在香橙派上编译并安装（含延迟插桩）
#   2. 测试视频已生成（/root/video_720p_25fps.mp4）
#   3. WSL 和香橙派时钟已 NTP 同步
#
# 用法：./bench_latency_wsl.sh

set -e

RTSP_URL="rtsp://100.111.77.78:8554/live"
ORANGEPI="orangepi@100.111.77.78"
SSH_CMD="sshpass -p orangepi ssh -o StrictHostKeyChecking=no $ORANGEPI"
SCP_CMD="sshpass -p orangepi scp -o StrictHostKeyChecking=no"
DURATION=65
RESULT_DIR="/tmp/latency_bench"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$RESULT_DIR"

echo "============================================"
echo "  smartcam 延迟基准测试"
echo "  服务器: $ORANGEPI"
echo "  RTSP:   $RTSP_URL"
echo "  时长:   ${DURATION}s"
echo "============================================"

# ---- Step 0: NTP 同步检查 ----
echo ""
echo "[Step 0] 检查 NTP 时钟同步..."
LOCAL_TIME=$(date +%s)
REMOTE_TIME=$($SSH_CMD date +%s)
OFFSET=$((LOCAL_TIME - REMOTE_TIME))
echo "  WSL 时间:     $(date '+%Y-%m-%d %H:%M:%S')"
echo "  香橙派时间:   $($SSH_CMD date '+%Y-%m-%d %H:%M:%S')"
echo "  时钟偏差:     ${OFFSET}s"
if [ $((OFFSET < 0 ? -OFFSET : OFFSET)) -gt 2 ]; then
    echo "  ⚠️  时钟偏差超过 2 秒，建议同步 NTP！"
    echo "  WSL: sudo ntpdate pool.ntp.org"
    echo "  香橙派: sudo ntpdate pool.ntp.org"
fi

# ---- Step 1: 停止旧 smartcam，清理旧数据 ----
echo ""
echo "[Step 1] 停止旧 smartcam 进程..."
$SSH_CMD "pkill -x smartcam 2>/dev/null; sleep 1; rm -f /tmp/smartcam_latency.csv"
sleep 2

# ---- Step 2: 启动 smartcam ----
echo ""
echo "[Step 2] 启动 smartcam..."
$SSH_CMD "nohup /usr/local/bin/smartcam -c /etc/smartcam/config.yaml > /tmp/smartcam.log 2>&1 &"
sleep 3

# 检查是否启动成功
SMARTCAM_PID=$($SSH_CMD "pgrep -x smartcam")
if [ -z "$SMARTCAM_PID" ]; then
    echo "ERROR: smartcam 启动失败！"
    echo "日志："
    $SSH_CMD "cat /tmp/smartcam.log"
    exit 1
fi
echo "  smartcam PID: $SMARTCAM_PID"

# ---- Step 3: 启动远端 CPU 采集 ----
echo ""
echo "[Step 3] 启动远端 CPU 采集..."
$SSH_CMD bash -s <<REMOTE_CPU_START &
#!/bin/bash
pid=\$(pgrep -x smartcam)
echo 'timestamp,cpu_pct,mem_mb,threads' > /tmp/latency_bench_cpu.csv
for i in \$(seq 1 $DURATION); do
    stats=\$(ps -p \$pid -o %cpu,rss,nlwp --no-headers 2>/dev/null)
    if [ -n "\$stats" ]; then
        cpu=\$(echo \$stats | awk '{print \$1}')
        mem_kb=\$(echo \$stats | awk '{print \$2}')
        mem_mb=\$(echo "scale=1; \$mem_kb / 1024" | bc)
        thr=\$(echo \$stats | awk '{print \$3}')
        echo "\$(date +%s%N),\$cpu,\$mem_mb,\$thr" >> /tmp/latency_bench_cpu.csv
    fi
    sleep 1
done
REMOTE_CPU_START
CPU_COLLECT_PID=$!

# ---- Step 4: 运行端到端延迟测量 ----
echo ""
echo "[Step 4] 运行端到端延迟测量（${DURATION}s）..."
python3 "$SCRIPT_DIR/measure_e2e_latency.py" "$RTSP_URL" \
    --duration "$DURATION" \
    --output "$RESULT_DIR/e2e_latency.csv"
echo "  端到端延迟数据已保存"

# ---- Step 5: 等待 CPU 采集完成 ----
echo ""
echo "[Step 5] 等待 CPU 采集完成..."
wait $CPU_COLLECT_PID 2>/dev/null

# ---- Step 6: 拉取远端数据 ----
echo ""
echo "[Step 6] 拉取远端数据..."
$SCP_CMD $ORANGEPI:/tmp/smartcam_latency.csv "$RESULT_DIR/server_latency.csv" 2>/dev/null
$SCP_CMD $ORANGEPI:/tmp/latency_bench_cpu.csv "$RESULT_DIR/cpu_usage.csv" 2>/dev/null
echo "  服务器延迟数据: $RESULT_DIR/server_latency.csv"
echo "  CPU 占用数据:   $RESULT_DIR/cpu_usage.csv"

# ---- Step 7: 停止 smartcam ----
echo ""
echo "[Step 7] 停止 smartcam..."
$SSH_CMD "pkill -x smartcam 2>/dev/null"

# ---- Step 8: 生成汇总报告 ----
echo ""
echo "[Step 8] 生成汇总报告..."

python3 - "$RESULT_DIR" <<'PYTHON_REPORT'
import sys
import csv
import os

result_dir = sys.argv[1]

def percentile(sorted_data, p):
    if not sorted_data:
        return 0
    idx = int(len(sorted_data) * p / 100)
    idx = min(idx, len(sorted_data) - 1)
    return sorted_data[idx]

report_lines = []
report_lines.append("=" * 70)
report_lines.append("  smartcam 延迟基准测试汇总报告")
report_lines.append("=" * 70)

# --- 服务器处理延迟 ---
server_csv = os.path.join(result_dir, "server_latency.csv")
if os.path.exists(server_csv):
    encode_us = []
    queue_us = []
    server_us = []
    with open(server_csv) as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                e = int(row['encode_us'])
                q = int(row['queue_us'])
                s = int(row['server_us'])
                if e > 0 and s > 0:
                    encode_us.append(e)
                    queue_us.append(q)
                    server_us.append(s)
            except (ValueError, KeyError):
                continue

    if server_us:
        report_lines.append("")
        report_lines.append("--- 服务器处理延迟 ({} 帧) ---".format(len(server_us)))
        report_lines.append("{:<20s} {:>10s} {:>10s} {:>10s} {:>10s} {:>10s}".format(
            "指标", "Avg(ms)", "P50(ms)", "P90(ms)", "P95(ms)", "P99(ms)"))
        report_lines.append("-" * 70)

        for name, data in [("编码延迟", encode_us), ("队列等待", queue_us), ("服务器总处理", server_us)]:
            if not data:
                continue
            s = sorted(data)
            avg = sum(s) / len(s)
            report_lines.append("{:<20s} {:>10.1f} {:>10.1f} {:>10.1f} {:>10.1f} {:>10.1f}".format(
                name, avg/1000, percentile(s,50)/1000, percentile(s,90)/1000,
                percentile(s,95)/1000, percentile(s,99)/1000))
else:
    report_lines.append("")
    report_lines.append("⚠️  服务器延迟数据未找到")

# --- 端到端延迟 ---
e2e_csv = os.path.join(result_dir, "e2e_latency.csv")
if os.path.exists(e2e_csv):
    e2e_data = []
    with open(e2e_csv) as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                e2e_data.append(int(row['latency_us']))
            except (ValueError, KeyError):
                continue

    if e2e_data:
        s = sorted(e2e_data)
        report_lines.append("")
        report_lines.append("--- 端到端延迟 ({} 帧) ---".format(len(s)))
        report_lines.append("{:<20s} {:>10s}".format("指标", "延迟(ms)"))
        report_lines.append("-" * 35)
        report_lines.append("{:<20s} {:>10.1f}".format("Min", s[0]/1000))
        report_lines.append("{:<20s} {:>10.1f}".format("Avg", sum(s)/len(s)/1000))
        report_lines.append("{:<20s} {:>10.1f}".format("P50", percentile(s,50)/1000))
        report_lines.append("{:<20s} {:>10.1f}".format("P90", percentile(s,90)/1000))
        report_lines.append("{:<20s} {:>10.1f}".format("P95", percentile(s,95)/1000))
        report_lines.append("{:<20s} {:>10.1f}".format("P99", percentile(s,99)/1000))
        report_lines.append("{:<20s} {:>10.1f}".format("Max", s[-1]/1000))
else:
    report_lines.append("")
    report_lines.append("⚠️  端到端延迟数据未找到")

# --- CPU 占用 ---
cpu_csv = os.path.join(result_dir, "cpu_usage.csv")
if os.path.exists(cpu_csv):
    cpu_data = []
    mem_data = []
    with open(cpu_csv) as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                cpu_data.append(float(row['cpu_pct']))
                mem_data.append(float(row['mem_mb']))
            except (ValueError, KeyError):
                continue

    if cpu_data:
        s_cpu = sorted(cpu_data)
        s_mem = sorted(mem_data)
        report_lines.append("")
        report_lines.append("--- CPU / 内存占用 ({} 采样) ---".format(len(s_cpu)))
        report_lines.append("{:<20s} {:>10s} {:>10s}".format("指标", "CPU(%)", "内存(MB)"))
        report_lines.append("-" * 45)
        report_lines.append("{:<20s} {:>10.1f} {:>10.1f}".format(
            "Avg", sum(s_cpu)/len(s_cpu), sum(s_mem)/len(s_mem)))
        report_lines.append("{:<20s} {:>10.1f} {:>10.1f}".format(
            "P95", percentile(s_cpu,95), percentile(s_mem,95)))
        report_lines.append("{:<20s} {:>10.1f} {:>10.1f}".format(
            "P99", percentile(s_cpu,99), percentile(s_mem,99)))
        report_lines.append("{:<20s} {:>10.1f} {:>10.1f}".format(
            "Max", s_cpu[-1], s_mem[-1]))
else:
    report_lines.append("")
    report_lines.append("⚠️  CPU 数据未找到")

report_lines.append("")
report_lines.append("=" * 70)
report_lines.append("数据文件:")
report_lines.append("  服务器延迟: {}/server_latency.csv".format(result_dir))
report_lines.append("  端到端延迟: {}/e2e_latency.csv".format(result_dir))
report_lines.append("  CPU 占用:   {}/cpu_usage.csv".format(result_dir))
report_lines.append("=" * 70)

report_text = "\n".join(report_lines)
print(report_text)

report_path = os.path.join(result_dir, "latency_report.txt")
with open(report_path, 'w') as f:
    f.write(report_text + "\n")
print(f"\n报告已保存到 {report_path}")
PYTHON_REPORT

echo ""
echo "============================================"
echo "  基准测试完成！"
echo "  结果目录: $RESULT_DIR"
echo "============================================"
