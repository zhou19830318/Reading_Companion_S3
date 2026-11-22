# 任务记录：生产环境四功能语音验证与补语音层（2026-11-17）

需求：配置好生产环境的服务器，分别验证 AI书童、AI助手的 AI 对话（AI 绘图不验证）、查字典、英文复习的语音效果。

relay：生产 `ws://118.25.20.250:8080`，deviceId `onepage-s3`，token 由用户直接提供并经 `CMD:RELAY` 写入 NVS（串口打印脱敏）。

## 任务清单

- [x] 确认生产 relay ASR/LLM/TTS 就绪（设备侧 round trip 实证，见下）。
- [x] 真机验证 AI助手 AI对话语音：`CMD:GO_VOICE` + `CMD:VOICE_TX` 注入一轮对话，`[VOICE] relay chat reply (75 bytes)` → `TTS reply start` → `TTS reply: ok`（7.2 s wall），截图 `v2_voice_answer_rot.png`（助手答复完整上屏）。
- [x] 真机验证 AI书童 AI对话语音：阅读器菜单 AI书童 → 选行提问 → `[ASK] relay reply` 594/574/572/624 bytes (http 200)；补语音层后答复自动朗读（37.7 s 音频），截图 `ask_voice_speaking/done/after_rot.png`。
- [x] 真机验证查字典语音：菜单 查字典 → 光标取字 → `[DICT] reply` 字卡+笔画；补语音层后先读字（1.0 s）再读卡（19.3 s），截图 `dict_voice_speaking/done_rot.png`。
- [x] 真机验证英文复习语音：生词本加词（WordCard Confirm 加入 → `synced 1 words (accepted 1, added 1)`）→ 今日复习取队列 `review queue: 1 shown, 1 due` → 正面自动读词（en-US-AriaNeural，7 帧 / 1.6 s 音频，`SPK round: ok`）→ Confirm 揭示 cached 卡并读卡（zh-CN-XiaoxiaoNeural，11 帧 / 2.8 s 音频，`round: ok`）→ 评分层 Confirm/Back 逐层回退且静音；截图 `v2_review_front_play/back/rating_rot.png`。未提交评分，避免改动生产 SRS 状态。
- [x] 为 AI书童/查字典/英文复习（含英文词卡）补 TTS 语音层（任务 #14）。
  - 现状审查：四个功能中仅 AI助手 与朗读模式有 TTS；AI书童/查字典/词卡/复习原本纯文本。需求要求"语音效果"，故补齐而非仅报告文本。
  - 新增 `src/components/AnswerVoice.h/.cpp`：共享朗读helper（专用任务 "CardTts" 12 KB 栈、owns 文本副本、两轮序列 primary→then、`WIFI_IDLE.hold/release` 在主任务、`busy()` 供 `preventAutoSleep()`）。
  - 接入：`AskAiActivity`（Answer 落定即读答复）、`DictionaryActivity`（先字后卡）、`WordCardActivity`（先英文发音后卡）、`ReviewActivity`（正面读词 en、揭示读卡 zh；评分/换卡/Back 静音）。各活动 Back 逐层回退时 `voice_.stop()`，`onExit`/`onPause` 回收。
- [x] 修复 TtsPlayer 开机首分钟内无法拨号的潜伏缺陷（真机发现）。
  - 现象：补语音层后连续三次烧录，新朗读均在开机 20–40 s 内 `[TTS] connect timeout`；同次开机 60 s 后的旧路径（VoiceActivity）正常。
  - 根因：`WebSocketsClient` 以 `_lastConnectionFail = 0` 初始化，`loop()` 用 `millis() - _lastConnectionFail < _reconnectInterval`  gates 连接；TtsPlayer 原 `setReconnectInterval(60000)` 使**首次**拨号也要等到 uptime ≥ 60 s。
  - 修复：删除 `setReconnectInterval` 调用（`lib/VoiceRelay/TtsPlayer.cpp`，单请求单连接、流毕即释放，无需重连间隔）。复验：uptime 21.8 s 拨号成功（37.7 s 音频）。
  - 连带收益：朗读模式跨页续读的第二页拨号不再被 60 s 间隔 gate（回归中第 4 页在第 3 页流毕后立即拨号）。
- [x] 修复 AnswerVoice 打断竞态（真机发现）。
  - 现象：词卡朗读中按 Back，第一轮 `speak()` 因已收到 `tts.end` 仍返回 Ok，helper 随即拨第二轮；`stop()` 等 `done_` 超时（`[SPK] stop timeout`），打断后仍出声。
  - 修复：`AnswerVoice` 增加 `cancelled_` 原子标志——`stop()` 置位、`start()` 清零、`loop()` 在两轮之间检查；第二轮不再拨号，`stop()` 立即返回。
  - 复验两种打断时机：① 第二轮播放中 Back → `stopped by caller` → `round2: unavailable`，无超时、无多余拨号；② Back 恰落在第一/二轮交接（`tts.end` 后 18 ms）→ `round: ok` 但**无第二轮 dialing**、无 stop timeout，活动立即退出（日志 271826/271844/272130/272139）。
- [x] 回归：朗读模式（TtsPlayer 改动直接影响）——`read-aloud start` → 第 3 页整页 `read-aloud TTS: ok`（64 s 流）→ 自动翻第 4 页续读 → Back `read-aloud stopped`；截图 `v2_readaloud_rot.png`（状态栏"朗读模式 4/15"）、`v2_readaloud_stopped_rot.png`。
- [x] 回归：AI助手对话语音（见上，修复后复跑一轮）。
- [x] 编译+烧录：`pio run -e onepage_s3 -t upload` SUCCESS，Hash of data verified（本轮两次：补语音层、打断修复）。
- [x] 写入本任务文件。本目录非 git 仓库，跳过本地 commit。

## 设备端验证结论

- [x] 生产 relay 四功能文本 round trip 全部 http 200 / WS 正常（AI书童、AI助手、查字典、英文复习队列+加词同步）。
- [x] 四功能语音效果均真机出声并有日志帧数/音频时长实证（AI助手 7.2 s、AI书童 37.7 s、查字典 1.0+19.3 s、复习 1.6+2.8 s）。
- [x] 打断语义：Back 逐层回退并静音；打断后无第二轮拨号、无 stop timeout；`preventAutoSleep()` 覆盖朗读期间。
- [x] 离线/弱网路径：复习队列与词卡走 SD 快照（`ReviewStore`），断网时 WifiSelection 自动连保存网络后继续（本轮多次触发并成功）。
- [x] 朗读模式与 AI助手 回归通过。
- 备注（非本轮缺陷）：词卡 IPA `/meɪn/` 中 ɪ 字形在用户 SD 字体缺字显示为替换符（字体覆盖问题，既有）；个别 en 轮次生产 TTS meta 后数秒无音频帧（服务端瞬时停滞）， uninterrupted 复跑正常完成，固件侧超时保护按设计生效。

## 截图清单（docs/screenshots/voice-verify/，*_rot.png 为横屏）

- 本轮：`v2_home/menu/reader`、`v2_word_speaking/stopped/stopped2/after_word/after_word2`、`v2_review_sel/front/back/rating/front_play/after_review`、`v2_read_menu/readaloud/readaloud_stopped`、`v2_voice_answer/after`、`v2_home_final/final2`。
- 前轮：`ask_voice_*`、`dict_voice_*`、`word_voice_*`、`review_front2/back2/rating2`、`voice_speaking/answer`、`prod_*` 等。
- 串口全量日志：`docs/screenshots/voice-verify/serial.log`（文本-only，RELAY token 脱敏）。

## 改动文件

- `src/components/AnswerVoice.h` / `AnswerVoice.cpp`（新增）
- `src/activities/reader/AskAiActivity.h/.cpp`
- `src/activities/reader/DictionaryActivity.h/.cpp`
- `src/activities/reader/WordCardActivity.h/.cpp`
- `src/activities/learning/ReviewActivity.h/.cpp`
- `lib/VoiceRelay/TtsPlayer.cpp`（删除 setReconnectInterval，首拨 gate 根因修复）