# 网页串口遥控（Web Remote）— 2026-11-17

## 目标

建立一个网页服务虚拟操控本工程电子书：网页实时显示墨水屏当前画面（串口截图），
网页按键与真机按键一一对应，通过串口命令操作设备；每次按键后页面立即回读截图，
并在界面延迟刷新（如进菜单先显示"连接 WiFi"中间态）时持续追帧，直到稳定画面。
**所有设备通讯均走串口**，网页不直连设备网络服务。

## 交付物

- `webremote/server.py` — 串口桥接 + HTTP/SSE 服务（纯 Python 标准库 + vendored pyserial，无 Pillow）
- `webremote/static/index.html` / `style.css` / `app.js` / `flows.js` — 前端控制台（无框架、无外部依赖）
- `webremote/static/testvoice.pcm` — 内置测试语音（16 kHz mono int16，181826 B ≈ 5.7 s）
- `dist/onepage-webremote.zip` — 跨平台便携包（见下文"打包"）
- 运行方式：`python3 webremote/server.py [--port /dev/ttyACM0] [--http 8090] [--host 127.0.0.1]`
  （`--host 0.0.0.0` 可供局域网访问）

## 架构

```
浏览器 ──HTTP/SSE──> server.py ──独占串口──> ESP32-S3 (src/main.cpp CMD: 协议)
   │  EventSource /api/events：log / status / screen / key 事件
   │  GET /api/screen.png?orient=port|land：当前帧 PNG（no-store）
   │  POST /api/key {key,holdMs,settleMs,shot,followMs}：按键+回读+追帧
   └─ POST /api/shot /api/info /api/reconnect；GET /api/state
```

- **单一串口所有者**：一个读线程解析行协议与截图二进制流；写锁串行化命令；
  截图锁串行化捕获；状态锁保护帧/日志。打开串口会复位 ESP32（DTR/RTS），
  故每次连接建立后 `_initial_capture` 延迟 2–3s 重试 4 次取首帧。
- **帧解码**：固件按面板扫描序 dump 800×480 位图；`Image.frombytes("1",(800,480))`
  后 `rotate(-90, expand=True)` 得可读竖屏 480×800（与 device_test.py 一致）。
  FNV-1a CRC（2166136261/16777619）与 `FBPTR … CRC:%08X` 校验，坏帧不发布并重试。
- **追帧（frame-follow）**：按键回读一帧后启动追帧线程，在 `followMs` 窗口内
  （默认 8000ms，间隔 0.9s）持续截图；仅当 CRC 变化才发布 `screen` 事件，
  generation 计数保证新按键取消旧追帧。解决"进菜单只看到连接 WiFi 中间态"问题。
- **日志安全**：`RELAY <url>|<token>` 行在上环前掩码为 `***`；环形缓冲 4000 行，
  SSE 新订阅补发最近 200 行 backlog。
- **CLOUD 信息**：设备主循环可能被 WiFi 管理阻塞数秒，`/api/info` 两次尝试、
  每次等待 6s，解析 `url/deviceId/tokenSet`。

## 串口协议（与 src/main.cpp 对应）

| 命令 | 作用 |
| --- | --- |
| `CMD:KEY <back\|ok\|left\|right\|up\|down\|power>` | 注入短按 |
| `CMD:KEYHOLD <key> <ms>` | 注入长按 |
| `CMD:SCREENSHOT` | `FBPTR/CRC` → `SCREENSHOT_START:n` → n 字节 → `SCREENSHOT_END` |
| `CMD:CLOUD` | `CLOUD url=… deviceId=… tokenSet=…` |

## 前端设计

外观依据 OSHWHub OnePage 项目渲染图（`/tmp/op_front.png` 等参考图）：
灰色摄影棚颗粒背景 + 漂移柔光、哑白 PET 面板、CNC 铝中框金属渐变、
三段式下巴导航（返回｜左/右分裂pill｜确认）、右侧 Wake/AI 短键 + 翻页摇杆、
底边 mic/USB-C/充电灯、挂绳孔；衬线中文标题 + 等宽遥测 chips。
配色取自硬件本身：铝灰阶 + 墨屏米白 + 朱砂印章红（主操作）+ moss 绿（连接/开关）
+ 琥珀（忙/警告）+  azure（信息），自定义滚动条同色系。

常驻操作台（≥7 个实时控件）：长按模式开关、长按时长 200–3000ms、
落屏等待 100–1500ms、**追帧窗口 0–15000ms**、按键后自动截图开关、
日志自动滚动开关、屏幕视图（竖屏 480×800 / 原生 800×480）、
立即截图 / 重连串口 / 云信息 三个动作键。
串口日志面板：过滤、暂停/继续、清空、行数徽章、终端式等宽预格式化行。
键盘直操：↑↓←→ / Enter=ok / Esc、Backspace=back / P=power。
状态栏与顶栏 LED 实时反映连接、帧序号、CRC、捕获耗时、最近按键。
无声音设计（遥控工具页，声音不属于该体验）。

## 验证（真机 /dev/ttyACM0 + 浏览器实测）

- 首帧：服务启动后自动取帧，SEQ 1、CRC OK、约 500–700ms。
- 按键回读：7 键逐一点击均命中（elementFromPoint 全对）并推进帧序号；
  读书页翻页 SEQ 连续推进；键盘 Enter/Esc 同样生效。
- 追帧：进 AI助手触发 WiFi 连接中间态后，窗口内持续回读直至稳定菜单帧；
  相同 CRC 不重复刷图（前端亦按 crc 去重）。
- 操作台：长按徽章/时长、落屏等待、追帧窗口、自动截图、自动滚动、
  竖/横视图切换（480×800 ↔ 800×480）、过滤、暂停、清空、云信息
  （url/deviceId/tokenSet，token 掩码显示）、立即截图、重连串口 全部实测生效。
- 视口：1440×900 与 390×844 实测截图（/tmp/shot_desktop.png、/tmp/shot_mobile.png），
  无横向溢出（bodyW==vw）、无遮挡/裁切、侧键与下巴键全部可达；
  控制台 0 报错；重复按键/重连无重复线程或监听器累积。
- reduced-motion 路径：动画全禁时内容仍完整可见（入场动画仅 keyframes 设 opacity）。

## 过程中修复的缺陷

1. SSE 死锁：`_sse` 持 state_lock 内再调 `state()` 二次加锁 → 移出锁外。
2. 帧解码方向错误（花屏）→ 800×480 扫描序 + rotate(-90)。
3. 开串口复位导致首帧丢失 → 连接建立后重试捕获。
4. WiFi 中间态停留（用户报告）→ 追帧线程 + 仅 CRC 变化发布 + followMs 可调。
5. 上下左右键无反应（用户报告）：容器 ::after 扩大命中区盖住子按钮 →
   子按钮 z-index:2、容器伪元素 pointer-events:none；单列网格被 pre 日志行
   撑宽致侧键溢出屏幕 → minmax(0,1fr) + min-width:0。
6. `no CLOUD reply`：设备主循环阻塞 >3s → 2 次 × 6s 等待。
7. 日志折行断字 → white-space:pre + 面板内横向滚动。

## 输入台（文本 / 麦克风音频注入）

- 文本：`CMD:VOICE_TX <text>` → `activityManager.injectVoiceTranscript()`，
  走与真人语音相同的 relay 回合（ASR 跳过，直接 LLM+TTS）。回复 `VOICE_TX accepted/rejected`。
- 音频：网页麦克风（MediaRecorder → 解码重采样 16 kHz mono int16）或内置 testvoice.pcm，
  `CMD:VOICE_RX <samples>` 武装 → 原始二进制流 → `CMD:VOICE_RX_END`。
  固件侧 `VoiceActivity` 外部 PCM 源上限 20 s（`MAX_EXTERNAL_RECORD_SECONDS`， mic 8 s 不够 paced 上传）。
- 网页输入台控件：文本框+字数、发送、麦克风开始/停止、电平表、注入状态机
  （就绪/录音中/上传中/设备处理中/画面已更新/被拒绝/失败）。

## 功能巡检面板（19 项流程）

`flows.js` 声明式步骤词汇：`{cmd}` 导航、`{k,n,gap,hold}` 按键、`{wait}`、`{shot}` 基准帧、
`{screen}` 取帧并断言变化、`{log,ms}` 日志断言、`{menu}` 阅读器菜单定位、
`{text}`/`{audio}` 注入回合（失败自动重试一次）、`{macro:"reader"}` 打开最近书籍。
覆盖：首页九宫格、文件浏览、最近书籍、今日复习、万年历、市场行情、AI助手文本/音频、
文件传输、AI书童、查字典、英文复习、设置、阅读器开书/翻页/菜单/书签、朗读模式、返回主页。
逐项通过/失败徽章 + 耗时 + 总进度条；全部通过才算巡检完成。

## 串口 / USB 可靠性修复（本轮真机调试）

1. **VOICE_RX 批量上传丢字节**（多层根因，逐一修复）：
   - HWCDC 默认 256 B RX 队列在 ISR 延迟尖峰时丢字节 → `setRxBufferSize(16384)`；
   - loop 任务消费节奏受限 → 专用 `vrxDrainTask`（prio 3）+ 16 KB 环形缓冲，
     环满即停读（背压经 CDC NAK 传回主机内核缓冲）；
   - USB-Serial-JTAG 64 B 硬件 FIFO 在全速 bulk 下仍丢（ISR 只有 0.14 ms 余量）
     → 主机 paced 上传 512 B/25 ms（≈20 KB/s，3.2 ms 余量），181826 B 全量无损。
   - 行解析器仅在 `expect==0 && ring 空` 时运行，END 命令不会被当作音频吃掉；
     30 s 主机消失超时结算半截捕获。
2. **截图帧短帧/失步**：HWCDC `flush()` 在全局 1 ms TX 超时会整环丢弃 →
   dump 期间 `setTxTimeoutMs(1000)`，结束恢复 1；块放弃时零填充到声明长度，
   CRC 不符让主机带完整帧重试。
3. **并发日志混入二进制帧**：所有任务日志经 `Logging.cpp logPrintf()` 单一出口，
   新增 `loggingBeginSerialDump()/loggingEndSerialDump()` 互斥门——非 dump 任务
   等待至多 5 s 后丢弃串口副本（RTC 环缓冲仍保留）。
4. **截图时机（用户报告：截到未加载完的半页）**：`SCREENSHOT` 先等
   `activityManager.isRendering()` 空闲（≤10 s，含 30 ms 排队渲染调度窗口），
   再持 `RenderLock` 导出整帧——渲染任务（含 EPD 驱动直写 `Serial.printf` 的日志）
   在 dump 期间被阻塞，既保证帧完整，又消除最后一种 TX 交织垃圾。
   实测：开书冷渲染 settleMs=100 时首帧等待 2385 ms 取帧，与 3 s 后稳定帧 CRC 一致。
5. **服务端帧泵健壮性**：二进制接收 3 s 无数据 → 放弃帧 + resync（丢弃含 U+FFFD
   的垃圾行直到干净行），每次捕获重置 crc_ok；短帧不再吞噬后续 CMD 回复。
6. **WiFi 空闲关机**：60 s 静默后射频 park，`VOICE_RX` 武装被拒 "not idle voice screen"
   → 服务端 audio() 自动 `GO_VOICE`+6 s 重试至多 4 次。
7. **relay URL 必须含路径**：设备 NVS relayUrl 需为
   `ws://host:8080/api/v1/voice/stream`（裸 URL 会被服务端 50 ms 内关闭升级，
   表现为 TransportError::ConnectTimeout=4）。已用 `CMD:RELAY <url>|<token>` 重新配置。
8. **被拒原因可见**：END 被拒多因 relay 会话中途失败（如 ASR_QUOTA_EXHAUSTED），
   服务端从最近日志提取 `relay error / [VOICE] failed:` 原因随响应返回，
   输入台与巡检失败详情直接显示设备端原因。

## 验证（真机 /dev/ttyACM0 + 无头浏览器实测）

- 全功能巡检 **19/19 通过**（run8，含截图时机修复固件）：
  麦克风注入"画面已更新"、文本注入"画面已更新"、控制台 0 报错、
  390×844 移动端无横向溢出、7 键全部可达。
- 日志流纯二进制垃圾行 **0**（run5 有 8 行 → 互斥门+渲染锁后清零；
  仅存合法朗读多行文本的跨行 UTF-8 碎片）。
- 音频端到端：181826 B paced 上传 → `external source finished: 90913 samples` →
  `VOICE_RX_END accepted` → relay `session.done`（asrFinal≈300–440，llmFirst≈334）。
- 截图时机专项：6 连键 settleMs=100 全部 CRC OK；开书/翻页首帧即完整帧。
- 环境性失败备注：生产 ASR 偶发 `ASR_QUOTA_EXHAUSTED retryable=0`（供应商瞬时限流），
  会话在 audio.start 后 ~200 ms 失败导致其后 END 被拒；重试即成功，非固件/服务端缺陷。

## 打包（跨平台便携包）

- `dist/onepage-webremote.zip`（88 文件，≈15.2 MB）：
  `server.py` + `static/`（含 testvoice.pcm）+ `vendor/serial`（pyserial，含 LICENSE）
  + `python-win/`（Windows 嵌入版 Python 3.12.7，binascii/zlib 内置，依赖核验 MISSING=[]）
  + `firmware/`（bootloader.bin / partitions.bin / firmware.bin，与 .pio 构建产物逐字节一致，
  附 BUILD_INFO.txt）+ `flash.sh` / `flash.bat`（esptool 烧录器，缺 esptool 时给出安装提示）
  + `run.sh`（Ubuntu：python3→python 回退）+ `run.bat`（Windows：嵌入 Python→系统 Python 回退链）
  + `README.md`（双平台使用 + 固件烧录说明）。
- 固件烧录偏移 0x0 / 0x8000 / 0x10000；NVS 在 0x9000 不在烧录范围，Wi-Fi/relay 配置保留。
- 页面内串口选择：`GET /api/ports`（USB 适配器优先排序）+ `POST /api/connect`，
  无需命令行参数即可在网页换口/重连。
- 复验（解包到 /tmp/bundle-test，`bash run.sh --http 8091`）：
  自动连接 ttyACM0、首帧 CRC OK 645 ms、端口列表、connect API、截图、
  按键推进帧（5 连测全 CRC OK）、巡检面板/flows.js/testvoice.pcm 均在位。
- 使用：解压 → Ubuntu `bash run.sh` / Windows 双击 `run.bat` → 浏览器打开页面 →
  选择串口连接 → 即可网页模拟控制电子书（固件需已烧录本工程 onepage_s3）。

## 中间修复：物理按键与回读冲突（2026-11-19，已被下节取代）

> 这是中间方案：物理按键单独触发短追帧。最终已改为“屏幕变化驱动”，
> 不再区分输入来源，见下一节。

**症状**：网页虚拟按键正常；设备上的真实物理按键在网页遥控运行时经常不生效，
网页与设备混合操作时画面/按键状态容易不同步。

**根因（两处叠加）**：

1. **服务端只认识自己注入的按键**：`server.py` 只在 `/api/key` 注入后截图，
   固件日志里的 `[IN] press edge` 没有消费，所以设备物理按键不会触发网页回读。
2. **截图导出阻塞主循环**：`SCREENSHOT` 先等待渲染空闲（可达数秒），再持有
   `RenderLock` 分段导出 48 KB 帧；整个过程中 `gpio.update()` 不运行，而按键
   没有 ISR/队列，短按完全落在导出窗口内就被丢掉了。每次网页按键后还有默认
   8 s 的追帧窗口，物理按键被吞的概率因此很高。

**修复**：

- `InputManager::pollWhileBlocked()` / `HalGPIO::pollWhileBlocked()`：在截图
  等长阻塞路径中采样硬件并锁存新的按下沿，`update()` 在下一帧合并为正常
  press 事件；短按即使已松开也会在导出结束后被消费一次（同一按键多次敲击
  合并为一次，避免误连翻多页）。
- `main.cpp` 的 `SCREENSHOT` 等待/分段写/零填充循环内调用上述轮询。
- `server.py`：跟踪固件 `KEY ... injected` 回显，区分注入按键与设备物理按键；
  检测到物理 `[IN] press edge` 后合并 120 ms 内的连按，再启动一次短追帧
  （2.5 s），让网页画面跟随设备真实按键刷新。
- `app.js`：收到 `source=device` 的 key 事件时在状态栏显示「物理按键」。

**构建与复验**：

- `pio run -e onepage_s3`：SUCCESS，Flash 83.6%（5481907 / 6553600 B），
  RAM 42.6%（139488 / 327680 B）；仅保留既有 `main.cpp:713 vrxTail++`
  volatile 警告。
- `python3 -m py_compile webremote/server.py` 通过；用模拟 `_line` 输入验证：
  `KEY ... injected` 不触发物理追帧，`[IN] press edge` 触发一次合并追帧。
- `dist/onepage-webremote/firmware/` 已更新：firmware.bin 与 `.pio/build/onepage_s3`
  产物 SHA-256 一致；bootloader/partitions 逐字节未变；BUILD_INFO.txt 已同步。
- 待真机验证（需烧录新固件并人工按键）：物理按键翻页、网页追帧期间的连按、
  网页与物理混合操作。

## 屏幕变化驱动回读（2026-11-19 最终逻辑）

**需求**：只要屏幕内容发生变化，就应自动截屏并刷新网页，不区分操作来源——
网页虚拟按键、设备物理按键、三轴加速度翻页、自动翻页、菜单切换都一样。

**实现**：

- 固件 `ActivityManager::renderTaskLoop()`：每个渲染任务完成后（面板推送已结束）
  输出 `[DISP] frame <seq> '<Activity>'`。日志在持有 `RenderLock` 时写出，避免与
  截图二进制帧在串口上交错。所有翻页入口最终都汇聚到渲染任务，因此物理按键、
  `CMD:KEY` 注入、三轴手势、自动翻页共用同一个“屏幕已更新”信号。
- `SCREENSHOT` 阻塞路径中除 `gpio.pollWhileBlocked()` 外，还调用
  `TILT.update(...)`，让导出期间的三轴手势继续采样并锁存，导出结束后仍能翻页。
- `server.py` 监听 `[DISP]`：180 ms 尾随合并（generation 递增，新帧取消旧定时器）
  后调用 `capture()`；若已有截图在途则跳过，因为 `CMD:SCREENSHOT` 本身会等待
  渲染任务空闲，必然包含这次更新。截图 CRC 不变时不推送 SSE。
- 物理按键的 `[IN] press edge` 仍用于状态栏「物理按键」提示，但不再单独触发
  轮询追帧；三轴翻页没有按键日志，完全由 `[DISP]` 覆盖。
- `/api/key` 的 `followMs` 默认改为 0（额外轮询仅作旧固件/特殊场景的可选回退）。

**构建与复验**：

- `pio run -e onepage_s3`：SUCCESS，Flash 83.6%（5481999 / 6553600 B），
  RAM 42.6%（139488 / 327680 B）；仅保留既有 `main.cpp:713 vrxTail++`
  volatile 警告。
- `python3 -m py_compile webremote/server.py` 通过；模拟 `_line` 验证：
  注入按键只触发屏幕刷新、物理按键额外触发设备状态、纯 `[DISP]`（三轴翻页）
  同样触发刷新。
- 待真机验证：物理按键翻页、三轴翻页、自动翻页及三者在连续截图期间的响应。

## 备注

- 本目录非 git 仓库，按约定不做提交。
- 生产 relay 部署状态另见 `2026-11-17-production-voice-verification.md`。