# VoxDrive 车机座舱 UI 设计规范（Deep-Space Cockpit）

> **文档性质**：Jetson 侧 Qt dashboard 界面重构的唯一设计基线。任何会话实施 UI 改动前**先读完本文档**，按第 11 节分期执行；设计决策变更先改本文档再动代码（与 AGENTS.md 同规则）。
> **版本**：v1.0（2026-09-04 定稿）；**v1.0 实施状态（2026-09-05）**：P1+P2 代码完成（commit d37990b，PC offscreen 端到端验证 PASS），P3 动效打磨与 CPU 实测、板上实测（selftest/语音链路/回归）待板可达后补。
> **设计输入**：UI-UX-Pro-Max skill 检索结论（第 2 节）+ 代码现状调查（第 1.3 节）+ 用户决策（VNC 键鼠 / 深空驾驶舱风 / 全工具可交互 / 深色为主+昼间模式 / 多页+插件化 / 语音打开页面 / 克制不复杂）。
> **本版本只做设计，不含实现代码**；附录 E 的插件契约为规格定义，非实现。

---

## 1. 设计目标与约束

### 1.1 目标

1. **车机 HMI 观感**：深空驾驶舱——深海军蓝底、玻璃拟态卡片、品牌青强调、等宽数字读数。观感对标新势力车机夜间模式，不是开发者监控面板。
2. **流畅度**：动效克制且顺滑；UI 进程 CPU 预算 = 空闲 <5% 单核、预览+语音活动 <15% 单核（Orin Nano，实测口径回填 AGENTS.md）。
3. **插件化**：核心=宿主，功能=插件。**新增功能=新增一个插件文件，不改宿主代码**；主页磁贴、底部导航、页面路由全部由注册表自动生成。
4. **多页可跳转**：主页桌面 + 功能页，底部 dock 导航；语音可直接"打开 XX 页面"。
5. **语音可见**：ASR 识别文本、TTS 播报文本、聆听/播报状态全部上屏（现状：语音只出声不上屏）。
6. **全工具可交互**：tool_bus 七工具全部有屏上控件（现状仅 dashcam 3 动作），全部真功能、零占位。

### 1.2 运行环境约束

| 项 | 值 | 依据 |
|---|---|---|
| 板/系统 | Jetson Orin Nano 8GB，Ubuntu 22.04，L4T R36.4.7 | AGENTS.md |
| 显示 | X11 (xcb)，**VNC 远程桌面 + 键鼠操作**（无触屏） | 用户决策 2026-09-04 |
| 基准分辨率 | **1920×1080**（VNC 与本机 :0 同屏）；1366×768 为降级档 | 1.4 节 |
| Qt | PyQt5 ≥5.15（apt 系统 python3），Widgets 路线 | jetson/requirements.txt:8 |
| 图标渲染 | QtSvg——板上一次性依赖 `apt install python3-pyqt5.qtsvg`（全仓库现状无 QtSvg，需新增） | 调查 2026-09-04 |
| 字体 | 系统有 Noto Sans CJK SC；读数等宽字体**仓库捆绑 ttf**（离线板不联网下字体） | AGENTS.md 资料复制规则 |
| 简洁红线 | 不上 QML / QGraphicsView / QOpenGLWidget（全仓库零先例，引入=复杂度失控）；Widgets+QSS+QPropertyAnimation 到顶 | 用户决策"不要过于复杂" |

### 1.3 现状基线（重构起点）

`jetson/dashboard/dashboard_ui.py`（723 行，单文件）：深底青色开发者面板；1400×780 硬编码；emoji 图标；语音不可见；按钮仅 dashcam 3 动作；传感器卡是死元素；无动画；已有可复用资产——`ZmqSub`/`DashcamPoller`/`StreamPlayer` 三线程、ffmpeg→QImage 预览管线（960×540 BGRA）、`VOX_DASH_SNAPSHOT` offscreen 自测钩子（保留）、`BarGauge` 自绘（将被 ArcGauge 升级替换）。

### 1.4 分辨率与降级

| 档 | 规则 |
|---|---|
| 1920×1080（基准） | 全尺寸 token 直接生效；默认最大化窗口（`showMaximized`），不再 1400×780 |
| 1366×768（降级） | 全局缩放因子 0.75：磁贴 4→3 列，右栏并单人字排，字号阶梯整体 -1 档；实现=QFontMetrics 换算 + 布局 stretch，不做多套 QSS |
| 窗口化（VNC 默认态） | 布局用 3 个横向 stretch 区（左主区/右栏/底栏），任意窗口尺寸不破版；最小可用 1152×720，低于此出滚动条 |

---

## 2. 设计智能来源（UI-UX-Pro-Max 检索记录）

供后续会话复检（skill 位于 `.agents/skills/ui-ux-pro-max/`，检索脚本 `scripts/search.py`）：

| 查询 | 命中与采纳 |
|---|---|
| `--design-system "automotive in-vehicle infotainment cockpit"` | 风格 `data-dense-dashboard`（骨架：4-8px 节奏/小字号阶梯/卡片化）+ `voice-first-multimodal`（语音层：波形/聆听脉冲/播报态/大触控目标）+ `bento-box-grid`（磁贴网格）+ `glassmorphism`（表面） |
| `--domain color "automotive dashboard dark cockpit"` | 采纳 Smart Home/IoT Dashboard 深色系为基底（#0F172A 族），品牌强调延续项目既有 #00C8FF 青 |
| `--domain typography "automotive digital display dashboard"` | 双轨制：数字读数=等宽（JetBrains Mono，防数字跳动抖宽），界面文案=sans（Noto Sans CJK SC） |
| `--domain ux`（touch/motion/dark-mode） | 触控目标 ≥44px、相邻间距 ≥8px、正文对比 ≥4.5:1（3:1 仅大字/图形）、按压反馈 80-150ms、每屏动画元素 ≤2、无限循环动画仅限"活动指示"类（本设计=语音波形）、必须支持 reduced-motion 降级、动效时长用共享 token 不许散落魔数 |
| `references/pro-rules.md` | 禁 emoji 作结构性图标（改 SVG 矢量）；图标尺寸 token 化（20/24/32），stroke 统一 1.5px；同层只用 outline 一种风格；有含义图标对相邻背景对比 ≥3:1；disabled=降强调+无动作 |
| `--domain chart "gauge real-time status"` | 单 KPI 仪表=Gauge（数字必须文字并排呈现，不能只靠色）；存储水位→ArcGauge；fps 历史→Streaming 思路但降级为 60s 迷你趋势条（CPU 考量，见 8.3） |

**风格定式**：`data-dense-dashboard` 骨架 × `glassmorphism` 表面 × `voice-first-multimodal` 语音层，品牌青贯穿。

---

## 3. Design Tokens

### 3.1 色板（语义 token，双主题）

对比度全部**实测**（WCAG 2.1 相对亮度公式，2026-09-04 脚本验算，非估算）。规则：正文 ≥4.5:1；大字（≥24px 或 ≥18.66px bold）与图形/UI 部件 ≥3:1。

| Token | 深色（默认） | 昼间 | 深色对比实测 | 昼间对比实测 | 用途 |
|---|---|---|---|---|---|
| `bg` | #0F172A | #EEF2F7 | — | — | 窗口底 |
| `bg-deep` | #020617（渐变终点/凹陷区） | #E2E8F0 | — | — | 状态栏底、语音区底 |
| `card` | rgba(255,255,255,0.06)（合成≈#1B2231） | #FFFFFF | 见下方护栏 | fg 17.85:1 | 卡片 |
| `card-border` | rgba(255,255,255,0.12) | #D3DCE6 | ≥3:1 ✓ | — | 卡片描边 1px |
| `card-inner` | #141B2E（实底，用于嵌套面板） | #F1F5F9 | fg 16.37:1 | fg 15.88:1* | 嵌套面板/输入区 |
| `fg` | #F8FAFC | #0F172A | 17.06:1 AAA | 15.88:1 AAA | 主文本 |
| `fg-muted` | #94A3B8 | #475569 | 6.68:1 AA | 7.58:1 AAA | 次级文本/标签 |
| `accent` | #00C8FF | #00739B | 8.73:1 AAA | 5.35:1 AA | 品牌青：选中态/链接/焦点/主按钮底 |
| `on-accent` | #062A36 | #FFFFFF | 7.69:1 AAA | 5.35:1 AA | accent 底上的文字 |
| `success` | #22C55E | #15803D | 7.52:1 AAA | 5.02:1 AA | 在线/成功/录音正常 |
| `warn` | #FFB020 | #B45309 | 9.37:1 AAA | 5.02:1 AA | 重连中/水位警告 |
| `danger` | #FF5C5C | #DC2626 | 5.66:1 AA | 4.83:1 AA | 错误/断流 |
| `rec` | #FF3B47 | #DC2626 | 4.87:1 AA | 4.83:1 AA | REC 红点/录像中（语义独立于 danger：一个是"正在工作"一个是"出错了"） |
| `on-success` | #052E12 | #FFFFFF | 6.55:1 AA | — | success 底上文字（pill） |

\* card-inner 昼间 #F1F5F9 对 fg 实测 15.88:1 系对 bg 的数值，白卡对 #0F172A 为 17.85:1，均 AAA。

**护栏**：深色玻璃卡合成色亮度须落在 #121A2B–#1C2436 区间（此区间内 fg-muted ≥6:1 已验算）；若调整 bg 或白色叠加比例超出区间，**必须重跑对比度验算**再合入。

### 3.2 字体

| Token | 值 | 说明 |
|---|---|---|
| `font-sans` | "Noto Sans CJK SC"（系统已有） | 正文、标签、按钮 |
| `font-mono` | "JetBrains Mono"（仓库捆绑 `jetson/dashboard/assets/fonts/JetBrainsMono-Regular.ttf` + `-Medium.ttf`，OFL 许可）→ QFontDatabase.addApplicationFont 注册 | **所有动态数字读数**：时钟、fps、温度、GB、百分比。等宽保证数字刷新不抖宽 |
| `font-mono-fallback` | DejaVu Sans Mono（系统） | 捆绑加载失败时 |
| 字号阶梯 | 12 / 14 / 16 / 20 / 28 / 42 px（`text-xs`→`text-display`） | 42 仅空调温度与钟面；行高 1.5（大字 1.2） |
| 字重 | Regular 400 / Medium 500 / Bold 700 | 标签用 Medium 不用 Bold（车机屏渐晕下 Bold 发糊） |

### 3.3 几何 token

| Token | 值 |
|---|---|
| `space` | 4 / 8 / 12 / 16 / 24 / 32 px（只用这 6 档，4-8px 节奏） |
| `radius` | 卡 16px / 磁贴 20px / 按钮-pill 10px / toast 12px |
| `border` | 1px（卡片、分隔线）；焦点环 2px `accent` |
| `icon-size` | 20 / 24 / 32 px 三档 token，禁止其它值 |
| `touch-target` | **≥44×44px**（含内边距热区），相邻目标间距 ≥8px——键鼠形态仍按此设计（车机基因 + VNC 鼠标更易点中） |

### 3.4 动效 token

| Token | 值 | 用途 |
|---|---|---|
| `dur-fast` | 120ms | 按压反馈、pill 状态色变 |
| `dur-base` | 200ms | 页面切换、卡片 crossfade、dock 选中位移 |
| `dur-slow` | 320ms | toast 滑入滑出 |
| `easing` | QEasingCurve.OutCubic（出场 InCubic） | 全部动画 |
| 硬规则 | 每屏同时动画元素 ≤2；无限循环动画仅限语音波形/聆听脉冲（属"活动指示"，规则允许）；所有动画可被 `VOX_DASH_REDUCED_MOTION=1` 一键降级为直切（offscreen 自测也走此模式） | 来源见第 2 节 UX 准则 |

### 3.5 图标

- **禁 emoji**。全部换 SVG 矢量：Phosphor Icons outline 风格（MIT），**复制不软链**入 `jetson/dashboard/assets/icons/*.svg`，viewBox 24、stroke 1.5px、`fill=none` 统一；渲染用 QtSvg（一次性依赖见 1.2）。
- 首批清单（23 枚）：`home, cam-video, car, activity, gear, mic, speaker, rec-dot, camera, eye, thermometer, fan, window, sun, moon, seat, wifi, wifi-off, hard-drive, warning, close, chevron-right, check`。
- 尺寸只用 3.3 节三档；颜色取 `currentColor` 语义（随所在文本色），状态色由外层 QSS 给。

---

## 4. 信息架构：多页导航

### 4.1 页面清单与路由

路由 = `QStackedWidget`，页面深度 ≤1（页内禁二级导航，保持简单）：

| 路由 id | 页面 | 内容 | 来源 |
|---|---|---|---|
| `home` | 主页（桌面） | 功能磁贴网格（插件自动注册）+ RK 快览条 | 宿主内置 |
| `dashcam` | 行车记录 | 预览大画面 + 控制条 + 状态卡 + 事件日志 | 插件 |
| `vehicle` | 车控 | 空调卡 + 车身俯视图（车窗/天窗/座椅） | 插件 |
| `status` | 状态 | 传感器卡（sensor_read）+ 服务健康 + RK 存储详情 | 插件 |
| `settings` | 设置 | 主题切换、轮询周期、预览地址、关于（版本/模型口径） | 插件 |

跳转通道：底部 dock 点击、主页磁贴点击、`ctx.navigate(id)`（程序内）、语音导航事件（第 7 节）。`home` 磁贴数量随插件增长，网格满了纵向滚动（页内唯一可滚动区）。

### 4.2 持久层（所有页面共有）

```
1920×1080 基准（px）
┌──────────────────────────────────────────────────────────────┐ h=64 顶栏
│ 14:32  09-04 周四    ●router ●rag ●llm ●tts ●tool    [RK●] [◐] │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│                     内容区（QStackedWidget）                   │ h≈856
│                                                              │
├──────────────────────────────────────────────────────────────┤ h=128 底栏
│ [home][cam][car][act][gear]   ◉语音区： 状态徽章+字幕+波形      │
└──────────────────────────────────────────────────────────────┘
toast 层：顶部居中悬浮（顶栏下方 16px），跨页面
```

- **顶栏**：左=时钟（mono 28px `fg`）+日期（12px `fg-muted`）；中=五服务 pill（h28，语义：online=`success` 20% 透明底+`success` 字，down=`danger`，未知=`fg-muted`）；右=RK 在线徽章（`success`/`danger`）+主题切换按钮 44×44（`sun`/`moon` 图标）。
- **底栏**：左=dock 图标组（每枚 56×56，间距 16，图标 24px；选中态=`accent` 18% 透明底 + 底部 3px `accent` 指示条，切页动画=指示条 `dur-base` 滑移）；右=语音区（宽约 1000px，规格见第 7 节）。
- **toast 层**：分 `info/warn/danger` 三级（`accent`/`warn`/`danger` 左侧 4px 色条），宽 ≤720，8s 自动收回，同屏最多 2 条（多余的合并为计数"+N"），滑入 `dur-slow`。

### 4.3 各页线框

**home 主页**（磁贴网格 4 列，磁贴 h≈200，gap 16）：
```
┌─ RK 快览条（h64 卡片：录像态·fps·存储%·预览态，点击→dashcam 页）─┐
├──────────┬──────────┬──────────┬──────────┤
│ 磁贴 2×宽 │  磁贴    │  磁贴    │  磁贴    │
│ 行车记录  │  车控    │  状态    │  设置    │
│ ●录像中   │ 24°C·全关 │ RK:52%  │ 深色主题 │
└──────────┴──────────┴──────────┴──────────┘
```
磁贴规格：图标 32px `accent` + 名称 20px Medium + 摘要行 14px `fg-muted`（**摘要必须是活数据**，来自插件订阅）；按压 scale 0.98 `dur-fast`；新插件注册后磁贴自动出现，无需改宿主。

**dashcam 页**：
```
┌────────────────────────────┬───────────────┤
│                            │ 状态卡         │
│   预览 16:9（LIVE徽章·REC   │ ●录像中 30.0fps│
│   红点·左上角叠时间戳）       │ 段13 · 14.5GB  │
│   底边渐隐 scrim：           │ [ArcGauge 52%] │
│   [●REC][预览][抓拍] 48px高  ├───────────────┤
│                            │ 事件日志        │
│  （右栏宽 ~560）             │ segment_closed │
│                            │ watermark_del  │
└────────────────────────────┴───────────────┤
```
预览卡=玻璃卡内嵌视频，视频与卡片间 8px 内衬；控制条按钮 ≥48px 高、图标+文字；事件日志=最近 20 条 `:6701` 事件（时间 mono 12px + 类型 + detail），新条目顶部插入 + `dur-fast` crossfade。

**vehicle 车控页**：
```
┌──────────────────┬──────────────────────┤
│ 空调卡            │ 车身俯视图卡（SVG）      │
│   24°C (mono 42) │   ┌────[天窗]────┐    │
│ [电源][△][▽]     │   FL窗      FR窗    │
│ 模式: [制冷][制热][除雾] │   │    车体     │    │
│ 风量: [1][2][3]  │   RL窗      RR窗    │
│                  │  [主驾座椅][副驾座椅]   │
└──────────────────┴──────────────────────┘
```
每个控件都是**可点真控件**（映射见 6.2），点击后立即乐观更新 UI 态，`state_change` 广播（:6670）回来后校正；超时 5s 无回读则回滚 + toast 提示。

**status 页**：三卡纵向——传感器卡（sensor_read 轮询喂活，终结现状死元素）、服务健康卡（五服务延迟/上次状态时间）、RK 存储详情卡（segments_total/segments_deleted/bytes_written/current_file/rtmp 连接与失败计数）。

**settings 页**：主题（深/昼 单选）、`dashboard.sensor_poll_s` 展示与调整、预览地址只读展示、关于卡（VoxDrive 版本、模型口径、本机 IP）——全部读 conf，落盘仅主题切换（写 `dashboard.theme`）。

### 4.4 内容区伸缩规则

内容区横向 = `stretch(内容)`；dashcam 页左右栏 62:38；磁贴网格 `QGridLayout` 4 列等宽。窗口 <1366 时磁贴 3 列；<1152 出 `QScrollArea`。

---

## 5. 插件化架构（核心）

### 5.1 原则

1. **宿主只做五件事**：加载插件、路由页面、渲染持久层（顶/底栏/toast）、分发事件、提供主题与共享服务。宿主不含任何具体业务（行车记录/空调等全部在插件里）。
2. **新增功能 = 新增 `plugins/<id>.py`**：磁贴、dock 图标、页面、语音导航词全部由 MANIFEST 自动生效，**宿主零修改**。
3. **克制清单**（违反任何一条即打回）：插件间禁止互相 import；插件禁止自建 ZMQ socket / QThread / QTimer（一律走 ctx，见 5.3）；页面深度 ≤1；无弹窗（告警走 toast）；禁 emoji、禁主题外硬编码色值。

### 5.2 目标目录结构与加载流程

```
jetson/dashboard/
├── dashboard_ui.py          # 宿主入口（保留文件名，内部重构）
├── core/
│   ├── theme.py             # 第 3 节全部 token（双主题 dict）+ QSS 生成器
│   ├── ctx.py               # PluginContext 实现与共享服务
│   └── loader.py            # 插件扫描/校验/注册
├── plugins/
│   ├── dashcam.py  ├── vehicle.py  ├── status.py  └── settings.py
└── assets/
    ├── fonts/JetBrainsMono-{Regular,Medium}.ttf
    └── icons/*.svg          # 第 3.5 节清单
```

加载流程（启动一次，~ms 级）：`loader.scan()` 按文件名序 import → 校验 MANIFEST 必填字段（缺失=跳过并 stderr 告警，**不让单个坏插件拖死整窗**）→ 按 `order` 排序注册 → 宿主据注册表生成：磁贴网格、dock（仅 `page=True` 的插件 + home）、路由表、语音导航词表。

### 5.3 插件契约（规格）

```python
# plugins/<id>.py —— 模块级约定，无基类、无注册调用，宿主反射加载
MANIFEST = {
    "id": "dashcam",              # 必填，全局唯一，=路由 id
    "name": "行车记录",            # 必填，磁贴/dock 中文名
    "icon": "cam-video",          # 必填，assets/icons/ 下的 svg 名
    "order": 10,                  # 必填，排序（10/20/30…留插入空隙）
    "tile_span": 2,               # 选填，主页磁贴占格 1|2（默认 1）
    "page": True,                 # 选填，是否注册整页进 dock（默认 False=仅磁贴）
    "nav_words": ["行车记录", "记录仪"],  # 选填，语音打开页面的触发词
}

def create_card(ctx): ...   # 必填 -> QWidget（主页磁贴内容，尺寸由宿主网格给）
def create_page(ctx): ...   # page=True 时必填 -> QWidget（整页，背景透明）
```

**ctx（PluginContext）能力清单**——插件与系统打交道的唯一通道：

| 成员 | 类型 | 说明 |
|---|---|---|
| `ctx.conf` | vox_config 只读 | `get/get_int/get_float` |
| `ctx.theme` | token dict + `themeChanged` 信号 | 昼夜切换时插件 QSS 自动重刷 |
| `ctx.bus` | 中央信号枢纽 | `state_changed(dict)`（:6670 车控合并态）、`rk_event(event,detail)`（:6701）、`service_status(dict)`（:6671）、`asr_text(str)`/`tts_text(str)`（新增契约，第 7 节）、`nav(str)`（语音导航）、`play_end()`（:6678）、`rk_online(bool)` |
| `ctx.tool(tool, action, payload=None, on_done=None)` | 异步封装 | tool_bus :6669 REQ；内部 2s 超时重试 1 次口径对齐现状，失败回调 `on_done(ok, reply)`；插件不碰 socket |
| `ctx.poll(interval_ms, fn)` | 主线程 QTimer 注册 | 返回句柄可 `stop()`；轮询统一走此通道（单定时器合并唤醒，见 8.3） |
| `ctx.run_async(fn, on_done)` | QThreadPool | 一次性后台任务（快照落盘等）；**长任务**（视频拉流循环）用 `ctx.attach_thread(qthread)` 由宿主统一回收 |
| `ctx.toast(text, level)` | | 三级 toast（4.2） |
| `ctx.navigate(plugin_id)` | | 页面跳转（磁贴点击用） |
| `ctx.store` | 共享状态仓 | 如 `store.rk_status`：DashcamPoller 的最新 status 快照——dashcam 页与 status 页**共用同一次轮询**，插件只读 |

> 移植映射：现有 `ZmqSub`/`DashcamPoller`/`StreamPlayer` 三线程全部收编进宿主/ctx（bus 数据源与 `store` 实现在 core），插件层不再出现线程代码。

### 5.4 首批插件与动作全集（七工具全部上屏）

| 插件 | 工具 | 动作（→ :6669 REQ `{"tool","action"}`） | 屏上控件 | 状态回读 |
|---|---|---|---|---|
| dashcam | `dashcam` | `record_on/record_off`、`preview_on/preview_off`、`snapshot` | REC/预览/抓拍按钮 | :6700 轮询 + :6670 `dashcam_recording/preview` 广播 |
| vehicle | `climate_control` | `on/off`、`set_temp`（±0.5 步进）、`set_mode`（cool/heat/defrost）、`set_fan`（1/2/3） | 空调卡全控件 | `ac_on/ac_temp/ac_mode/ac_fan` |
| vehicle | `window_control` | 四窗 `open/close`、`open_all/close_all` | 车身俯视图四窗点击 | `window_fl/fr/rl/rr` |
| vehicle | `sunroof_control` | `open/close/tilt` | 天窗三态点击 | `sunroof_state` |
| vehicle | `seat_heater` | `driver_on/off`、`passenger_on/off` | 两座椅按钮 | `seat_driver/passenger` |
| dashcam | `camera_capture` | `front`（现阶段=抓拍入口） | 与 dashcam.snapshot 合并按钮 | — |
| status | `sensor_read` | 读全量 | 传感器卡 | `ctx.poll(dashboard.sensor_poll_s)` |
| settings | — | 无工具调用，纯 conf+关于 | 表单 | — |

---

## 6. 组件规范（公共组件规格）

| 组件 | 规格 | 数据/交互 |
|---|---|---|
| `Card` | 玻璃卡：`card` 底 + `card-border` 1px + `radius-card` + 内边距 24；嵌套面板用 `card-inner` 实底 | 纯容器 |
| `Pill` | h28 r10，语义色 20% 透明底 + 语义色文字；状态切换 `dur-fast` 色变 | 服务状态/RK 在线 |
| `Tile`（磁贴） | `tile_span` 格，r20，内边距 24；图标 32 `accent`、名 20 Medium、摘要 14 muted；hover 边框提亮至 0.2、按压 scale 0.98 | 点击 `ctx.navigate` |
| `RecButton` | 48px 高，REC 态=`rec` 底白点脉动（仅此一处允许脉动，属"活动指示"） | record_on/off |
| `ArcGauge` | 120px 弧 270°，底线 `card-inner`，值弧 `success`→`warn`(75%)→`danger`(90%) 渐变，中心 mono 28 数值 + "已用" 12px 标签（数字文字并排，不靠色——第 2 节 chart 准则） | `storage.used_percent` |
| `CarMap` | 俯视车身 SVG（自绘 assets/icons 内新增 `car-top.svg`）：四窗/天窗/座椅为独立可点区域，开=天蓝 `accent` 25% 填充，关=描边；点击区域 44px 等效热区 | :6670 车控键 |
| `WaveBar` | 语音波形：40px 高，12 根 4px 圆角竖条，QTimer 30fps 上限随机相位正弦包络；idle=静止最低高度 `fg-muted` 25%，listening=`accent`，speaking=`success` | 第 7 节状态机 |
| `StateBadge` | 语音状态徽章：h28 pill，四态文案 聆听中/识别完成/思考中/播报中（第 7 节） | |
| `Toast` | 见 4.2 | `ctx.toast` |
| `EventLog` | 行内列表 20 条上限，新条目顶部插入 + `dur-fast` crossfade；时间 mono 12px | :6701 |

---

## 7. 语音可视化与语音导航

### 7.1 语音状态机

```
idle ──6671 asr_text──► listening ──同一事件落定──► thinking ──6671 tts_text──► speaking ──6678 play_end──► idle
（无信号）               （WaveBar accent 动）        （WaveBar 静默+徽章"思考中"）   （WaveBar success+气泡）   （8s 后字幕淡出）
```

### 7.2 数据通道契约（需 intent_router 最小扩展，**不动 TTS 半双工 REQ 配对协议**——AGENTS.md 红线）

现状：ASR final 文本只 REQ→intent_router（裸文本，无 PUB）；TTS 6678 只发裸串 `"play_end"`；intent_router 自有 PUB 6671 只发 `{"service":"router","status":...}`。

**扩展提案（改一处：intent_router）**——router 在既有 6671 PUB 上增加三类事件（多加字段对旧消费者向后兼容，dashboard 现有 `_onx` 不受影响）：

```json
{"service": "router", "status": "asr_final", "asr_text": "还剩多少存储", "ts": 1757000000000}
{"service": "router", "status": "tts_say",   "tts_text": "存储剩余14.5GB…", "ts": 1757000000500}
{"service": "router", "status": "nav",       "target": "dashcam",          "ts": 1757000000300}
```

- `asr_final`：router 收到用户文本时发（router 手里有原文）。→ dashboard 驱动 listening→thinking。
- `tts_say`：router 把应答文本发给 TTS 7777 的同时发（router 手里有播报文本）。→ 气泡文本 + speaking。
- `nav`：语音打开页面（见 7.3）。
- 结束信号：dashboard **新订阅 6678**（现有裸串 `play_end`，无需改 tts 服务），驱动 speaking→idle。半双工门平衡不受影响（dashboard 只订阅不发送）。
- 局限如实标注：ASR **partial 中间文本**目前只打 stderr 不上网，实时_partial 字幕需改 ASR C++ 服务（新增 PUB），列为可选增强不进 P2 范围。

### 7.3 语音打开 app（页面）

- intent_router 关键词直通表（与其 dashcam 域确定性直通同模式，**不进 LLM**）：
  `("打开"|"显示"|"进入") + nav_words` → `nav` 事件 + TTS 应答"好的，已打开行车记录"；`("回到"|"返回") + ("主页"|"首页"|"桌面")` → `nav home`。
- nav_words 来自插件 MANIFEST 汇总（5.3），**新插件注册触发词即自动生效**，router 侧只需一张由宿主导出的词表（实现期把词表放进 conf 或 router 读 dashboard 插件清单，P2 定稿，默认先 conf 静态表）。
- dashboard 收 `ctx.bus.nav(id)` → 切页（dock 指示条同步滑移）；目标不存在 → toast "该功能未安装"。

### 7.4 语音区布局（底栏右段，宽 ~1000）

`[StateBadge 28] [字幕/气泡区：一行 ASR 原文（14px muted，前缀"你：")或 TTS 应答气泡（16px fg，前缀"聆行："）] [WaveBar 40px]`。字幕双缓冲：新事件覆盖旧文本，8s 无新事件淡出至 idle 徽章"聆行待命"。

---

## 8. 动效与流畅度

### 8.1 动效清单（全量，此外禁止加动画）

| 动效 | 规格 |
|---|---|
| 页面切换 | crossfade + 8px 横向滑移，`dur-base` OutCubic |
| dock 指示条 | 滑移 `dur-base` |
| 磁贴/按钮按压 | scale 0.98 + 透明度 0.9，`dur-fast`（≈120ms，符合 80-150ms 反馈窗口） |
| toast | 底部滑入/滑出 `dur-slow` |
| pill/状态色变 | 直接色变 `dur-fast`（QSS 无过渡，用 QVariantAnimation 驱动，或接受直切——P3 决定，不阻塞） |
| WaveBar / REC 脉动 | 仅语音活动与录音中，30fps 上限 |
| ArcGauge 值变化 | 弧长过渡 `dur-base`，数值直接换字（不做数字滚动，克制） |

### 8.2 渲染规约（防卡顿红线）

1. 玻璃拟态**不逐帧算模糊**：QWidget 无 backdrop-filter；卡片"玻璃感"用半透底色+1px 描边+顶部 1px 高光实现（零成本）；真实模糊只用于 toast 底和视频 scrim——**启动时一次性预渲染** QPixmap 缓存，禁止运行时 QGraphicsBlurEffect 每帧重绘。
2. 重绘最小化：动态区域单独 widget、`update(QRect)` 局部刷新；静态底图 `QPixmap.cache`；时钟 1s 一跳不改整栏。
3. 预览管线保持 ffmpeg→QImage（现状 960×540 BGRA）；1080p 基准下升级 1280×720 需实测 CPU 后再定（P3 实测项）。视频 widget 改 paintEvent 自绘省一次 setPixmap 拷贝（P3）。
4. 线程纪律：UI 线程零阻塞调用——所有 ZMQ REQ/轮询/快照落盘走 ctx（5.3）；`pyzmq ctx.destroy()` 退出口径沿用现状。
5. 全部动画可降级：`VOX_DASH_REDUCED_MOTION=1` → 所有 `QPropertyAnimation` duration=0（offscreen 自测同走此模式，截图可复现）。

### 8.3 CPU 预算与实测项（P3 回填）

| 项 | 预算 | 实测法 |
|---|---|---|
| UI 空闲（无预览无语音） | <5% 单核 | `pidstat -u -p $(pgrep -f dashboard_ui)` 60s 均值 |
| 预览+WaveBar 活动 | <15% 单核 | 同上，播放中 |
| 页面切换 | 无可感顿挫（>30fps 等效） | VOX_DASH_SNAPSHOT 前后时标 + 目测 |
| 定时器合并 | 全 UI ≤6 个活跃 QTimer | 代码审查项：ctx.poll 合并注册表 |

---

## 9. 可访问性与交付 checklist（每期验收必过）

- [ ] 对比度：第 3.1 节表已验算；**新增色值必须补验算并更新该表**
- [ ] 昼间主题单独核查（不沿用深色结论）：卡片阴影可见性、边框 #D3DCE6 与 bg 区分、accent 按钮白字 5.35:1
- [ ] 触控目标 ≥44px（全按钮/磁贴/dock/车控热区），相邻间距 ≥8px
- [ ] 焦点可见：键盘 Tab 焦点环 2px `accent`（VNC 键盘操作可用）；焦点顺序=视觉顺序
- [ ] 颜色不是唯一状态指示：LIVE/REC/在线等必带文字或图标形状差异（色盲可用）
- [ ] disabled 态：40% 透明 + 无动作，hover 无反馈
- [ ] 图标：无 emoji；SVG stroke 1.5 统一；尺寸只用 20/24/32
- [ ] 动画：每屏 ≤2；reduced-motion 全降级验证
- [ ] 单插件崩溃不拖死整窗（loader 隔离 + 插件内 try/except 兜底，异常进 toast）
- [ ] `VOX_DASH_SNAPSHOT` offscreen 截图五页各一张归档（重构前后对照）
- [ ] `run_regression.sh` 4/4 PASS 不回归

---

## 10. conf 新增键（jetson/config/voxdrive.conf，dashboard.* 风格）

```ini
dashboard.theme = dark              # dark|light（settings 页可写回）
dashboard.sensor_poll_s = 10        # status 页传感器轮询周期
dashboard.clock_24h = true
dashboard.tile_order = dashcam,vehicle,status,settings   # 主页磁贴顺序覆盖（缺省按 MANIFEST order）
```

---

## 11. 实施分期建议（给实施会话的执行顺序）

| 期 | 范围 | 验收 |
|---|---|---|
| **P1 宿主与视觉骨架** | 建 core/（theme/loader/ctx）+ 目录结构；token/双主题 QSS；顶栏底栏 dock 路由 toast；dashcam 插件平移现有三线程与预览管线；现有 723 行拆解入库 | 五页 offscreen 截图；回归 4/4；功能与现状等价（dashcam 3 动作+状态卡+横幅） |
| **P2 插件齐全 + 语音上屏** | vehicle/status/settings 插件（七工具控件全量 + sensor_read 喂活）；intent_router 6671 扩展（asr_final/tts_say/nav，直通关键词表）；dashboard 订 6678；语音状态机与字幕/气泡 | 语音"打开车控"切页实测；每控件点击→:6670 回读一致；离线兜底 toast；回归 4/4 |
| **P3 动效与流畅度实测** | 8.1 动效全量落地；ArcGauge/CarMap SVG；1280×720 预览取舍实测；CPU 预算表回填 AGENTS.md；昼夜切换实机验证 | 8.3 表全部实测数字；checklist 全过 |

**实施会话开工顺序**：读本文件 → 第 5 节搭宿主 → 第 3 节抄 token → 按 P1→P2→P3；任何与本文档冲突的现实（板依赖缺失等）先改本文档再改代码。

---

## 附录 A：token → QSS 对照（生成器输入，实施时直接抄）

```css
/* 深色主题 core 值（昼间由 theme.py 同构生成） */
QMainWindow, QStackedWidget { background: qlineargradient(y1:0,y2:1, stop:0 #0F172A, stop:1 #020617); }
QFrame#card  { background: rgba(255,255,255,0.06); border: 1px solid rgba(255,255,255,0.12); border-radius: 16px; }
QFrame#tile  { background: rgba(255,255,255,0.06); border: 1px solid rgba(255,255,255,0.12); border-radius: 20px; }
QFrame#tile:hover { border-color: rgba(255,255,255,0.22); }
QPushButton  { background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.14);
               border-radius: 10px; color: #F8FAFC; font-size: 16px; min-height: 48px; }
QPushButton:pressed { background: rgba(255,255,255,0.14); }
QPushButton#primary  { background: #00C8FF; color: #062A36; font-weight: bold; }
QPushButton#rec      { background: #FF3B47; color: #FFFFFF; }
QPushButton:disabled { opacity: 0.4; }   /* Qt 无 opacity——实现为 rgba(248,250,252,0.4) 前景 + 无 hover */
QLabel#muted { color: #94A3B8; font-size: 14px; }
QLabel[themeFont="mono"] { font-family: "JetBrains Mono"; }
```

## 附录 B：字体与图标落地

- **字体**：下载 JetBrainsMono-Regular/Medium.ttf（OFL）→ `assets/fonts/`，启动时 `QFontDatabase.addApplicationFont`；失败 fallback "DejaVu Sans Mono"。复制不软链。
- **图标**：Phosphor（MIT）下载对应 SVG → `assets/icons/`（清单见 3.5）；板上新依赖 `sudo apt install python3-pyqt5.qtsvg`（一次性，写入 QUICKSTART）。
- **car-top.svg**：仓库自绘（车身俯视轮廓 + 4 窗 + 天窗 + 2 座椅分区，path 分区 id 对应 :6670 键名）。

## 附录 C：现状代码 → 目标结构移植映射

| 现状（dashboard_ui.py） | 去向 |
|---|---|
| Design System 常量 :44-110 | core/theme.py（扩充为双主题 token） |
| ZmqSub :122 / DashcamPoller :166 / StreamPlayer :214 | core/ctx.py（bus 数据源 / store / attach_thread） |
| BarGauge :270 | plugins/dashcam.py 的 ArcGauge（替换） |
| 主窗口布局 _ui :314-525 | 宿主持久层 + plugins/* 各页 |
| _cmd :676 / _onx :618 / _on_dashcam :553 / 告警 :630 | ctx.tool / 宿主 bus / dashcam 插件 / 宿主 toast |
| VOX_DASH_SNAPSHOT :710 / AUTOPREVIEW :548 | 原样保留（回归资产） |
