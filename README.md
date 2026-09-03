# 聆行 VoxDrive — 分布式车载智能座舱系统（RK3588 + Jetson Orin 双板）

> 双 SoC 分布式车载智能座舱：**RK3588 作为行车记录媒体节点**（V4L2 摄像头采集 / RGA 图像处理 / MPP 硬件编码 / MP4 分段循环存储 / RTMP 推流），**Jetson Orin 作为端侧 AI 语音节点**（流式 ASR / LLM / RAG / TTS / Qt 座舱界面）。两板以太网互联，视频流走 RTMP、控制与状态走 ZeroMQ，实现"语音控制行车记录、画面预览、录像与存储状态查询"的跨节点完整闭环。

**状态：开发中（WIP）**——Jetson 侧七服务收编自上一代单体语音座舱项目并重构，RK3588 侧行车记录链路开发中。

## 系统架构

```
[UVC/MIPI 摄像头]
      │
      ▼
RK3588（行车记录媒体节点）
 V4L2 采集 ──► RGA 转换/缩放 ──► MPP H.264 硬编码(VBR)
                                     │
                 ┌───────────────────┴───────────────────┐
                 ▼                                       ▼
        Mp4SegmentSink                          RtmpSink（可替换）
     分段循环存储·水位监控                        RTMP 推流
     最旧覆盖·断链恢复                                │
                 │                                   │
                 └──► RK ZMQ 服务 ◄───────────────────┘
                      REQ/REP 状态应答 + PUB 异常事件
                              │ 以太网（控制面 ZMQ / 数据面 RTMP）
                              ▼
Jetson Orin（端侧 AI 语音节点）
  麦克风 ─► ASR ─► Intent Router ─┬► RAG（车辆知识库问答）
                                  ├► LLM（Qwen2.5 GGUF，llama.cpp 全离线）
                                  ├► Tool Bus ──► 跨板工具（录像/存储查询、抓拍、预览）
                                  └► TTS ─► 扬声器
  Qt Dashboard：座舱状态 + 行车记录画面预览 + 录像/存储面板
```

**任务划分原则**：数据密集型任务走专用加速器（RK3588 的 VPU/RGA/NPU），模型密集型任务走中心算力（Jetson GPU/统一内存）。

## Jetson 侧服务拓扑与 ZMQ 端口

| 服务 | 语言 | 端点 | 说明 |
|---|---|---|---|
| intent_router | C++ | REP `*:6666` / PUB `*:6671` | 意图路由：规则分类，分发到 RAG/LLM/Tool/TTS |
| rag | Python | REP `*:6667` / PUB→6671 | 车辆手册知识库：向量检索 + Top-K 召回 |
| llm | Python | REP `*:6668` / PUB→6671 | LLM 代理：对接 llama.cpp server（HTTP :8080），respond/tool_call 两类 JSON |
| tool_bus | C++ | REP `*:6669` / PUB `*:6670` | 工具总线：车控/传感工具执行，预留跨板工具类 |
| tts | C++ | REP `*:7777` `*:6677` / PUB `*:6678` | 语音合成（SummerTTS/VITS）+ 三端口握手防回灌 |
| asr | C++ | REQ→6666/6677 | 流式识别（sherpa-onnx zipformer 双语） |
| dashboard | Python | SUB 6670/6671 · REQ 6669 | PyQt5 数字座舱界面（面板插槽化，预留视频预览面板） |

统一消息信封：`{version, type, timestamp, source, payload}`——新增数据类型（GPS/IMU/检测事件…）只需扩展 `type`，不改协议。

## 仓库结构

```
jetson/               # Jetson 侧（七服务 + 公共模块 + 编排脚本）
  config/             # voxdrive.conf 统一配置（端口/路径/超时，集中管理）
  common/             # C++/Python 公共：配置读取·毫秒时间戳日志·JSON·消息信封
  services/           # asr / intent_router / rag / llm / tool_bus / tts
  dashboard/          # PyQt5 座舱界面
  scripts/            # 增量送板·板上编译·自测·启动编排·回归
rk/                   # RK3588 侧行车记录链路（IVideoSource/IVideoSink 接口化设计）
docs/                 # 工程文档（部署手册、架构说明、实测记录）
```

## 构建与部署

- Jetson 侧：`jetson/scripts/build_on_board.sh <service>`（板上原生编译），启动编排与健康检查见 `jetson/scripts/start_core.sh`。
- RK 侧：板端编译（细节随阶段 C 补充）。
- 模型文件不入库，按 `models_manifest.md`（本地维护）清单部署到板。

## 实测数据

> 以下表格在对应链路完成后以实测填入，无实测不填。

| 指标 | 数值 | 测量方法 | 状态 |
|---|---|---|---|
| LLM 生成速度（Qwen2.5-1.5B Q4_K_M, Jetson Orin Nano Super, -ngl 28 全层 offload） | 22.8 token/s | llama-server 非流式单请求 usage 分解计时（`selftest_llm.sh`，2026-09-03） | 已实测 |
| RAG 检索延迟（CPU 嵌入，45 文档库） | 稳态 ~210 ms/查（首查 591 ms 含预热） | `selftest_rag.sh` REQ 计时，2026-09-03 | 已实测 |
| 语音链路查询延迟（文本→最终答复，含 2 次 LLM 调用+工具执行） | 2.0-5.1 s/条 | `run_regression.sh` 4 条 canned 查询，2026-09-03 | 已实测（不含 ASR/TTS 播报） |
| TTS 播报端到端（合成+ALSA 播放） | 2.7 s（"启动自检测试"短句） | `selftest_tts.sh` 三端口握手计时，2026-09-03 | 已实测 |
| 1080p 采集帧率（RK, IMX415→rkisp NV12） | 30.0 fps（120 帧，max 帧间距 33ms） | `test_capture` 驱动时间戳统计，2026-09-03 | 已实测 |
| 1080p 采集→MPP H.264 硬编（RK） | 30.0 fps；编码延迟 avg 4.8ms / max 8.5ms；全链路 CPU ~2.8%（单核）；静态场景 VBR 实际 0.55Mbps（目标 4Mbps，GOP 2s 节奏 5/5 I 帧）；ffprobe/ffmpeg 全量解码零错误 | `test_encode` 300 帧 + 板上 ffmpeg 校验，2026-09-03 | 已实测 |
| MP4 分段循环录像（RK） | 10s→3 段（测试段长 3s），段边界严格 I 帧；每段 ffmpeg 全量解码零错误；水位触发按最旧序删段且当前段幸免 | `test_record` 分段+水位双相位，2026-09-03 | 已实测 |
| RK ZMQ 服务（REQ 状态查询 / PUB 事件上行） | 状态查询含 recording/pipeline_fps/存储水位（used 52% 实测）；录像开关 off/on 应答正确；慢加入者 SUB 收到 `segment_closed` 事件信封；16s 试跑 4 段全部 ffprobe 有效 | `test_recorder_client` REQ+SUB 双通道自测，2026-09-03 | 已实测 |
| RTMP 推流（RK 回环验证） | 推流 408 帧与录像完全一致（双 sink 扇出无丢帧）；拉流 h264 1080p、ffmpeg 解码零错误；实测码率 0.52Mbps（静态场景 VBR 下探，目标 4Mbps）；服务端中途断开→写失败即时检测→I 帧+2s 冷却重连，录像管线不受影响 | `test_rtmp` 两相位 + `recorder_service --rtmp-url` 集成，ffmpeg `-listen 1` 作回环接收端，2026-09-03 | 已实测（跨板拉流待 mediamtx 部署） |
| 语音端到端延迟（ASR→TTS 播报结束） | 待实测 | 毫秒日志打点对账 | 未开始 |
| 跨板查询往返延迟 | 待实测 | 毫秒日志打点对账 | 未开始 |

## Roadmap

- [x] zmq-comm-kit 通信库上 Jetson 编译验证（REQ/REP + PUB/SUB 回环）
- [x] Jetson 七服务收编 + 标准重构（统一配置/公共 JSON/毫秒日志/死代码清理；七服务全部板上自测 PASS，`start_core.sh` 一键全栈启动 + `run_regression.sh` 4/4 PASS）
- [ ] mediamtx RTMP 服务上 Jetson（部署被网络阻塞，见 models_manifest）
- [x] RK3588 硬件链路验证 + 采集层（野火 LubanCat-4/RK3588S + IMX415 MIPI；`IVideoSource`/`IVideoSink` 接口抽象 + `V4L2Capture` mmap/DMA-BUF 实现，NV12 1920x1080 实测 30.0fps 板上自测 PASS）
- [x] RK3588 MPP H.264 硬编（NV12 直入 VBR，30fps/编码延迟 4.8ms/全链路 CPU 2.8%，ffmpeg 全量解码校验通过；RGA 留作子码流缩放）
- [x] RK3588 MP4 分段循环录像（libavformat 封装，I 帧边界滚动分段/水位最旧覆盖/断链恢复，逐段解码校验）
- [x] RK ZMQ 服务（`recorder_service`：V4L2→MPP→MP4 管线线程 + REP 状态/录像开关 + PUB 事件上行，统一消息信封；`test_recorder_client` 双通道自测 PASS）
- [x] RK3588 RTMP 推流（`RtmpSink` flv over rtmp：avcC/AVCC 转换与 MP4 共用、I 帧+冷却断链重连；ffmpeg `-listen 1` 回环两相位自测 PASS + 服务级双扇出集成验证；跨板 mediamtx 部署待网络）
- [ ] 跨板工具 + dashboard 预览/状态面板
- [ ] 跨板闭环联调 + 端到端延迟分解实测
- [ ] 语义双路意图路由、RKNN 事件锁录（规划中）

## License

MIT
