#!/bin/bash
# 在香橙派上生成 720p@25fps 1分钟测试视频
# 通过 SSH 远程执行

ORANGEPI="orangepi@100.111.77.78"
VIDEO_PATH="/root/video_720p_25fps.mp4"

echo "=== 生成 720p@25fps 测试视频 ==="

sshpass -p orangepi ssh -o StrictHostKeyChecking=no $ORANGEPI bash -s <<'REMOTE_SCRIPT'
set -e

VIDEO_PATH="/root/video_720p_25fps.mp4"

if [ -f "$VIDEO_PATH" ]; then
    echo "测试视频已存在: $VIDEO_PATH"
    echo "文件大小: $(du -h $VIDEO_PATH | cut -f1)"
    exit 0
fi

echo "正在生成 720p@25fps 1分钟测试视频..."

ffmpeg -y \
    -f lavfi -i "testsrc2=size=1280x720:rate=25:duration=60" \
    -vf "drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf:\
text='%{localtime\:%H\\\\\:%M\\\\\:%S}':fontsize=36:fontcolor=white:borderw=2:bordercolor=black:x=(w-text_w)/2:y=h-60" \
    -c:v libx264 -preset ultrafast -tune zerolatency \
    -g 50 -bf 0 -keyint_min 25 \
    -b:v 1000k -maxrate 1000k -bufsize 40k \
    -pix_fmt yuv420p \
    "$VIDEO_PATH" 2>&1 | tail -5

echo "视频生成完成: $VIDEO_PATH"
echo "文件大小: $(du -h $VIDEO_PATH | cut -f1)"
echo "视频信息:"
ffprobe -v quiet -show_format -show_streams "$VIDEO_PATH" 2>&1 | \
    grep -E "width|height|r_frame_rate|duration|bit_rate|codec_name"

REMOTE_SCRIPT

echo "=== 测试视频准备完成 ==="
