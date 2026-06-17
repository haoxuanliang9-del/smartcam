# SmartCam 音频增强 & 视频画质增强 设计规格

> 日期: 2026-06-17
> 状态: 设计审批

## 1. 概述

为 SmartCam 新增两个处理模块，面向工业生产监控场景：

| 模块 | 功能 | 核心算法 |
|---|---|---|
| **AudioProcessor** | 远场人声降噪增强 | AGC 自动增益控制 + RNNoise 神经网络降噪 + 下采样 |
| **VideoProcessor** | 低光照画质增强 + 去噪 | CLAHE 自适应直方图均衡 + 非局部均值去噪 |

## 2. 目标场景与约束

- **目标硬件**: 香橙派 Zero 3 (ARM Cortex-A53, 四核, 1-1.5GB RAM)
- **开发平台**: x86 Linux 虚拟机
- **音频场景**: 远场人声拾取（工业厂房噪声环境, 人声距离 > 3 米）
- **视频场景**: 夜间低照度监控 + 弱光雾霾退化
- **功能控制**: 通过 `config.yaml` 静态开关和参数调节
- **兼容性**: 功能关闭时行为与当前版本完全一致

## 3. 架构

### 3.1 整体管道

```
Audio pipeline:
ALSA (48kHz) ──→ AGC ──→ RNNoise ──→ ↓6:1 resample ──→ Noise Gate ──→ PCMU ──→ RTP
                   └────────── AudioProcessor ──────────┘    (8kHz)

Video pipeline:
V4L2 ──→ swscale ──→ CLAHE ──→ fastNlMeansDenoise ──→ OSD filter ──→ H.264 ──→ RTP
              (YUV420P)    └─────────── VideoProcessor ───────────┘
```

### 3.2 源文件结构

```
src/modules/audio/audio_processor.h       # AudioProcessor 接口
src/modules/audio/audio_processor.cpp     # RNNoise 封装 + AGC + 重采样
src/modules/video/video_processor.h       # VideoProcessor 接口
src/modules/video/video_processor.cpp     # OpenCV CLAHE + 降噪
third_party/rnnoise/                      # RNNoise 源码（纯 C, 约 10 文件）
```

### 3.3 模块职责

- **AudioProcessor**: 入参 48kHz PCM int16 帧, 出参 8kHz PCM int16 帧; 无 ALSA/编码依赖
- **VideoProcessor**: 入参 YUV420P 裸数据, 出参增强后的 YUV420P 裸数据; 无 V4L2/编码依赖
- 两个 processor 均可独立实例化、独立启用/禁用

## 4. AudioProcessor 详细设计

### 4.1 管道

```
ALSA 48kHz S16_LE (480 samples/10ms)
    │
    ├── AGC (基于 RMS 包络)
    │     窗长: 480 samples (10ms)
    │     目标 RMS: -20dBFS
    │     增益范围: 0 ~ +30dB
    │     起效: 5ms / 释放: 100ms
    │     软限幅防过载
    │
    ├── RNNoise
    │     输入: 48kHz 16-bit mono, 480 samples/frame
    │     输出: 48kHz 16-bit mono, 同帧长
    │     内部管理 RnnoiseState*
    │     denoise_level: 0.0=bypass, 1.0=max suppression
    │     支持自定义模型路径
    │
    ├── 下采样 48kHz → 8kHz (×6 抽取)
    │     两级 FIR: 48k → 24k → 8k
    │     避免单级高倍抽取混叠
    │
    └── 输出: 8kHz S16_LE (80 samples/10ms)
           → 原有 noise gate → PCMU
```

### 4.2 接口

```cpp
class AudioProcessor {
public:
    bool init(const AudioEnhanceConfig& cfg);
    bool process(const int16_t* input, size_t input_samples,
                 int16_t* output, size_t& output_samples);
    void set_denoise_level(float level);
    void set_agc_target(float level);
    bool is_enabled() const;
};
```

### 4.3 降级策略

| 故障 | 行为 |
|---|---|
| RNNoise 模型加载失败 | WARN, 旁路降噪, AGC + 下采样继续 |
| ALSA 48kHz 不可用 | 回退原有 8kHz 采集 + noise gate |

## 5. VideoProcessor 详细设计

### 5.1 管道

```
YUV420P (来自 swscale)
    │
    ├── CLAHE (仅 Y 通道)
    │     clip_limit: 2.0
    │     tile_grid: 8×8
    │     OpenCV: cv::createCLAHE()
    │
    ├── fastNlMeansDenoising (YUV 三通道)
    │     h: 10.0 (亮度滤波强度)
    │     templateWindowSize: 7
    │     searchWindowSize: 21
    │     跳帧: 每 N 帧处理一次（默认 N=3）
    │     超时保护: >30ms 跳过当前帧
    │     OpenCV: cv::fastNlMeansDenoisingColored()
    │
    └── 增强后的 YUV420P → OSD filter
```

### 5.2 性能保护

- **跳帧处理**: `denoise_skip_frames = 2` 表示每 3 帧做一次去噪, 其余仅 CLAHE
- **超时跳过**: 连续 10 次去噪超时则自动增加 skip
- **可配置强度**: `denoise_h = 0` 完全旁路去噪
- 在 Cortex-A53 上预估: CLAHE < 3ms, 去噪 ~50-100ms（跳帧后均摊 ~20ms）

### 5.3 接口

```cpp
class VideoProcessor {
public:
    bool init(const VideoEnhanceConfig& cfg);
    bool process(uint8_t* y, uint8_t* u, uint8_t* v,
                 int width, int height, int y_stride, int uv_stride);
    void set_clahe_clip(float limit);
    void set_denoise_strength(float h);
    bool is_enabled() const;
};
```

### 5.4 降级策略

| 故障 | 行为 |
|---|---|
| OpenCV 不可用 (CMake 未找到) | VideoProcessor 编译为空实现, 旁路 |
| CLAHE 创建失败 | WARN, 跳过 CLAHE, 仅去噪 |
| 两者都失败 | 旁路整个 VideoProcessor |

## 6. 配置设计

### 6.1 config.yaml 新增 section

```yaml
audio_enhance:
  enabled: true
  agc_target_rms: 0.1
  agc_max_gain: 30.0
  agc_attack_ms: 5.0
  agc_release_ms: 100.0
  denoise_level: 1.0
  rnnoise_model: ""

video_enhance:
  enabled: true
  clahe_clip_limit: 2.0
  clahe_tile_size: 8
  denoise_h: 10.0
  denoise_skip_frames: 2
```

### 6.2 Config struct (config.h)

```cpp
struct AudioEnhanceConfig {
    bool enabled = false;
    float agc_target_rms = 0.1f;
    float agc_max_gain = 30.0f;
    float agc_attack_ms = 5.0f;
    float agc_release_ms = 100.0f;
    float denoise_level = 1.0f;
    std::string rnnoise_model;
};

struct VideoEnhanceConfig {
    bool enabled = false;
    float clahe_clip_limit = 2.0f;
    int clahe_tile_size = 8;
    float denoise_h = 10.0f;
    int denoise_skip_frames = 2;
};
```

### 6.3 MainService 集成

- `init()`: 读取 `audio_enhance` / `video_enhance` 配置
- `setup_modules()`: enabled 时创建 processor 并注入 capture 模块
- 关闭时: `AudioProcessor` / `VideoProcessor` 不创建, `AudioCapture` / `CameraCapture` 走原有通路

## 7. 依赖与构建

### 7.1 新增依赖

| 库 | 用途 | 类型 |
|---|---|---|
| RNNoise | 神经网络降噪 | 纯 C 源码, 编入 third_party/rnnoise/ |
| OpenCV (core + imgproc + photo) | CLAHE + 非局部均值去噪 | `find_package(OpenCV)`, 可选 |

### 7.2 CMake 变更

- `ENABLE_VIDEO_ENHANCE` option (默认 ON)
- `find_package(OpenCV QUIET)`, 未找到时 `HAS_OPENCV` 宏不定义, VideoProcessor 编译为空实现
- RNNoise 作为 static library 始终编译

## 8. 向后兼容

- `audio_enhance.enabled = false` → ALSA 8kHz 采集, 原有 noise gate + PCMU 管道
- `video_enhance.enabled = false` → VideoProcessor 不创建, swscale 直出送 OSD
- 默认配置均关闭, 与 v1.0.0 行为无差异

## 9. 测试

| 层级 | 内容 | 通过标准 |
|---|---|---|
| **AudioProcessor 单元** | 白噪声输入 → AGC RMS 收敛; 正弦波 + 噪声 → RNNoise 输出 SNR 提升 | SNR 提升 > 3dB |
| **VideoProcessor 单元** | 合成暗帧 + 高斯噪声 → CLAHE 输出直方图熵增加; 去噪 PSNR 提升 | PSNR 提升 > 1dB |
| **集成测试** | 启动 smartcam → FFmpeg 拉流 → 人工 + MOS/BRISQUE 评分 | 主观可感知改善 |
| **性能测试** | Zero 3 上 30 分钟连续运行 | CPU < 70%, 内存 < 50MB 增量 |
| **回归测试** | 功能关闭时 | 与 v1.0.0 行为一致 |
| **交叉编译** | Zero 3 上完整构建 + 运行 | 无链接错误 |
