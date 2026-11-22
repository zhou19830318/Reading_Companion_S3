# 任务记录：朗读本页 → 朗读模式（2026-11-17）

需求：将阅读器“朗读本页”功能改为“朗读模式”——语音逐页朗读电子书，自动翻页续读，直至用户打断退出为止。

## 任务清单

- [x] 审查现有朗读功能的逐页续读与打断逻辑
  - 结论：`EpubReaderActivity` 已实现连续朗读状态机（`readAloudTick` → TTS 完成 → `pageTurn(true)` → 等待渲染任务发布新页标识 `readAloudPageKey_` → 朗读下一页），跨章节经 `pendingSpineDelta` 衔接，书末自动结束。
  - 打断路径：短按 Back 停止并留在本页；手动翻页请求停止模式；打开菜单等子活动经 `onPause()` 停止；退出书籍经 `onExit()` 停止并回收 TTS 任务。朗读期间 `preventAutoSleep()` 阻止自动休眠。
- [x] 将“朗读本页”改名为“朗读模式”并更新 i18n
  - `lib/I18n/translations/chinese_simplified.yaml`：STR_READ_ALOUD "朗读本页" → "朗读模式"
  - `lib/I18n/translations/english.yaml`：STR_READ_ALOUD "Read this page aloud" → "Read aloud mode"
  - 运行 `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/` 重新生成字符串表（其余语言按既有约定回退英文）。
  - 菜单项与朗读中的状态栏标题共用该字符串，均随之显示“朗读模式”。
- [x] 移除连续空页即停的限制，保证朗读持续到用户打断
  - 删除 `READ_ALOUD_MAX_EMPTY_PAGES`（原为 3）及 `readAloudEmptyPages_`、`readAloudSkippedPage_`。
  - 纯图片/无文字页现在直接跳过并继续翻页，不再提前终止模式。
  - 保留的终止条件：用户打断、书末、TTS 失败、翻页超时（30 s）、页面提取失败等不可恢复错误。
- [x] 编译验证并写入任务文件
  - `pio run -e onepage_s3`：SUCCESS（RAM 36.9%，Flash 83.5%）。
  - 本目录非 git 仓库，跳过本地 commit。
- [x] 修复朗读期间被自身扬声器振动触发的倾斜手势自打断（真机验证中发现）
  - 现象：开启倾斜翻页的设备上，朗读出声后扬声器振动被 IMU 判为 tilt 手势（`[TILT] gesture back`），按"手动翻页即打断"的设计使朗读模式自行停止。
  - 修复：`ReaderUtils::detectPageTurn` 增加 `allowTilt` 参数（抑制时仍消费手势标志，避免模式结束瞬间积压手势触发翻页）；`EpubReaderActivity` 朗读期间传 `false`，按键翻页/Back 打断保留。
  - 复验：朗读播放中出现 `gesture back (28 deg)` 不再停止模式。
- [x] 真机烧录与功能验证（含截图）
  - 烧录：`pio run -e onepage_s3 -t upload` SUCCESS，Hash of data verified。
  - 验证驱动：`scripts/device_test.py`（串口合成按键 + CRC 校验截图）。
  - relay：生产 relay（118.25.20.250:8080）/readyz 正常，但设备 NVS 无 token；改用仓库同源 relay 本地实例（自生成一次性 token）完成端到端验证，relay 日志确认设备 `onepage-s3` 的 TTS 会话与推流。
  - 结果：WiFi 自动连接保存网络（Prefoco）→ `read-aloud start` → 整页朗读出声（206 帧 / 52.3 s 音频，I2S MAX98357A 功放启动）→ `read-aloud TTS: ok` → 自动翻页且跨章节（spine 66 3/3 → spine 67 1/15）继续朗读 → 短按 Back `read-aloud stopped`，状态栏恢复章节标题。
  - 截图（`docs/screenshots/read-aloud/`，`*_rot.png` 为旋转后可视版本）：
    - `menu2_rot.png`：阅读器菜单含"朗读模式"条目。
    - `aloud_active_rot.png`：朗读中状态栏显示"朗读模式"（3/3 54%）。
    - `aloud_page2_rot.png`：跨章节自动翻页后仍显示"朗读模式"并续读（1/15 54%）。
    - `aloud_stopped_rot.png`：Back 打断后状态栏恢复章节名"15. 智能指针"。
    - `home3_rot.png` / `page_before_rot.png`：验证前主页与书籍页面。

## 变更文件

- `src/activities/reader/EpubReaderActivity.h`
- `src/activities/reader/EpubReaderActivity.cpp`
- `src/activities/reader/ReaderUtils.h`（detectPageTurn allowTilt）
- `lib/I18n/translations/chinese_simplified.yaml`
- `lib/I18n/translations/english.yaml`
- `lib/I18n/I18nKeys.h` / `I18nStrings.h` / `I18nStrings.cpp`（构建时自动生成）
- `scripts/device_test.py`（真机验证驱动）

## 设备端验证结论

- [x] 打开 EPUB → 菜单选择"朗读模式"：逐页朗读并自动翻页，跨章节继续（真机日志 + 截图）。
- [x] 短按 Back 停止朗读；停止后状态栏恢复（真机日志 + 截图）。
- [x] 朗读期间倾斜手势不再自打断；按键手动翻页仍打断（代码路径保留，真机复验倾斜抑制）。
- [x] 串口日志可见 `read-aloud start/page/TTS: ok/stopped`。
- [ ] 连续图片页不中断、书末自动停止：代码路径已审查（空页直接标记完成继续翻页；`readAloud: end of book`），未在本轮真机流程中专门构造场景。
- [x] 生产 relay 切换与复验（2026-11-17，token 由用户直接提供）
  - `CMD:RELAY ws://118.25.20.250:8080|<token>` 写入成功（deviceId 沿用 `onepage-s3`，生产共享 token 认证通过）。
  - 真机复验：整页朗读出声（236 帧 / 59.5 s 音频）→ `read-aloud TTS: ok` → 自动翻页 spine 67 2/15 → 3/15 续读 → 短按 Back `read-aloud stopped`。
  - 截图：`prod_menu_rot.png`（含脚注行时的菜单）、`prod_active_rot.png`（生产朗读中状态栏"朗读模式"）、`prod_stopped_rot.png`（打断后恢复章节标题）。
  - 本地验证 relay 已停止，一次性 token 与运行目录已清理；`scripts/device_test.py` 对 RELAY 命令打印脱敏。

## 2026-11-18 间歇性无声排查与修复

用户报告：朗读模式有时候没有语音。

### 排查结论

- 历史日志中所有 `no audio` 事件均为 `stopped by caller`（巡检流程在首帧音频前打断），不是故障。
- 4 分钟真机复现（生产 relay）：4 页全部 `TTS: ok`（每页 ~63–67 s），当时服务商健康，无法按需复现。
- 代码审查发现两个真实脆弱点，任一命中即整段无声并退出朗读模式：
  1. **设备帧间隔超时 6 s < relay 每段合成预算 15 s**（`relay/relay/config.py TTS_TIMEOUT_S`）。
     edge-tts 冷拨号或服务商限流时首帧可迟于 6 s，设备误判 `stream stalled after 0 frames` 中止。
  2. **任何一次 TTS 失败立即终止整个朗读模式**，瞬时故障（unavailable）没有重试。

### 修复

- `lib/VoiceRelay/TtsPlayer.cpp`：`FRAME_GAP_TIMEOUT_MS` 6000→20000，覆盖 relay 15 s 段预算；
  真实失败仍由 `tts.error` 立即上报，加长只影响假死服务器的兜底时延。
- `src/activities/reader/EpubReaderActivity.{h,cpp}`：瞬时失败（`Result::Unavailable`）同页自动重试
  最多 2 次。重试决策放在 `readAloudTick`（loop 任务），与 `stopReadAloud` 串行化——不能放 TTS
  工作任务：`speak()` 入口会清 `stopRequested_`，任务侧重试会与用户停止竞态（停止丢失→整页播完→
  5 s 等待超时→强删运行中任务）。成功翻页/新页/会话启动/停止均清零重试计数。
- `src/main.cpp`：`CMD:RELAY` / `CMD:DEVICEID` 处理器保存前先 `CLOUD_CFG.load()`。
  原实现在新开机（单例未加载）时直接 save 会把 NVS 中其它字段（deviceId）清空——本轮验证中
  实际触发了一次，已修复并恢复 `deviceId=onepage-s3`。

### 真机验证（2026-11-18，固件已烧录）

- [x] 坏地址注入（`CMD:RELAY ws://118.25.20.250:9999/...`）：朗读启动 → 3 次拨号各 ~5 s 失败 →
  日志依次 `retry 1/2`、`retry 2/2` → `read-aloud: TTS failed, stopping` 干净停止（无强删任务）。
- [x] 恢复生产地址后 4 分钟回归：spine 276 第 4/5/6 页整页出声（252–273 帧，63.9–68.9 s 音频/页），
  第 7 页朗读 23.7 s 时短按 Back → `stopped by caller` → `read-aloud stopped`，全程 0 次重试。
- [x] `CMD:CLOUD` 确认 url/deviceId/token 三项齐全；便携包固件已同步更新
  （`dist/onepage-webremote/firmware/`，sha256 与 .pio 产物一致，zip 重建 88 文件 ≈15.2 MB）。
- 用户的 8090 便携包服务已恢复（`bash run.sh`），8091 验证服务已停止。

## 2026-11-18 系统音量调节（0-100，步进 10）

需求：系统设置-系统 增加音量项，上/下键每按一次 ±10。

### 实现

- `src/CrossPointSettings.h`：新增 `uint8_t volume = 20`（0-100）。默认 20 是把原硬编码
  25% 就近落到 10 步进网格、并保留"安静房间不吵"的调音取向。
- `src/SettingsList.h`：System 分类新增 `SettingInfo::Value(STR_VOLUME, &volume, {0,100,10},
  "volume", STR_CAT_SYSTEM)`。通用 VALUE 序列化自动存/读 settings.json 并钳制范围。
- `src/activities/settings/SettingsActivity.{h,cpp}`：音量项 Confirm 不再循环 VALUE，而是
  `openVolumePicker()` 打开与休眠时间同款的 `IntervalSelectionActivity`（0-100、small/large
  步进均 10，`ignoreInitialConfirmRelease=true`）。回调非取消时写 `SETTINGS.volume` 并
  `saveToFile()`。列表 VALUE 分支用 `STR_VOLUME_VALUE_FORMAT` 渲染百分比（如 20%）；
  音量与休眠时间项的 Confirm 提示改为"选择"（opensPicker）。
- `src/util/UiSound.cpp`：`atVolume()` 改读 `SETTINGS.volume`（按键音效随音量）。
- `lib/VoiceRelay/TtsPlayer.{h,cpp}`：lib 层不依赖 src，新增 `setVolumeSource(getter)` 静态
  注册点 + `gainPercent()`；`playPcm()` 用其缩放 PCM。无注册时回退 `DEFAULT_GAIN_PERCENT=25`。
- `src/main.cpp`：`SETTINGS.loadFromFile()` 后注册 `[]{ return SETTINGS.volume; }`，所有
  TtsPlayer（朗读/语音/字典/复习/SPEAK 调试）实时读取当前音量。
- i18n：`STR_VOLUME` = 音量 / Volume；`STR_VOLUME_STEP_HINT` = 左/右：10 上/下：10；
  `STR_VOLUME_VALUE_FORMAT` = `%u%%`（简/繁/英，其余语言回退英文）。

### 验证（v1  inline 上/下）

- 编译通过并已烧录真机；设置界面可达"系统"分类（截图确认分类表头与列表渲染正常）。
- 逐项自动化导航因 e-ink 刷新滞后不稳定，**应用户要求改由用户真机自测**上/下 ±10、
  边界钳制、重启保留与听感。
- 便携包固件已同步（sha256 与 .pio 一致）、BUILD_INFO 与 zip 已更新（88 文件）。

## 2026-11-18 音量界面改为选择器（参考休眠时间）

需求：音量调节界面设计参考休眠时间的调节界面设计（即 `IntervalSelectionActivity`）。

### 实现

- 移除 v1 的列表内联上/下拦截；音量项 Confirm 改为 `openVolumePicker()`，复用
  `IntervalSelectionActivity`（标题=STR_VOLUME，大数值+进度条+步进提示+按键提示），
  左/右、上/下 均 ±10，Confirm 保存、Back 取消，与休眠时间选择器完全同构。
- 列表 VALUE 分支渲染百分比（`STR_VOLUME_VALUE_FORMAT`），音量/休眠时间项 Confirm
  提示为"选择"。新增 `STR_VOLUME_STEP_HINT`、`STR_VOLUME_VALUE_FORMAT`。

### 真机验证（固件已烧录，截图 `docs/screenshots/volume/`）

- [x] 列表"音量"行显示百分比（20%），Confirm 提示"选择"（`list_20.png`）。
- [x] Confirm 打开选择器：居中标题"音量"+大号"20%"+进度条+提示"左/右：10 上/下：10"+
      按键提示 返回/-/+/选择，与休眠时间选择器同构（`picker_20.png`）。
- [x] 上键 ×2 → 40%，进度条随动（`picker_40.png`）；Confirm 保存回列表显示 40%
      （`list_40_saved.png`）。
- [x] Back 取消不保存：选择器 30% 时 Back，回列表仍 20%（`picker_30_unsaved.png` /
      `list_20_after_cancel.png`）。
- [x] 验证后音量恢复默认 20；便携包固件/BUILD_INFO/zip 已同步（88 文件），8090 服务已恢复。

## 2026-11-18 打包固件 + 网页遥控（便携包）

- 产物：`dist/onepage-webremote.zip`（88 文件，15,217,579 B ≈14.5 MiB）。
  sha256 `3b485768e654da427e4270674ae7d2ef10168b9862c1b611f4aa180cf1d7b2fa`。
- 内含：`firmware/{bootloader,partitions,firmware}.bin`（选择器音量版，firmware sha256
  `780e2080…60cb1c`，与 .pio 产物一致）+ `BUILD_INFO.txt`（16:12）；网页遥控
  `server.py` + `static/`（与 `webremote/` 源逐字节一致）；跨平台启动/烧录
  `run.sh/run.bat/flash.sh/flash.bat`、`README.md`、内置 `python-win/` 与 `vendor/serial`。
- 打包前清理 `__pycache__`、排除 `*.pyc`；解包回读 firmware.bin 校验 sha 一致。
- 用法：解压后 Ubuntu `bash run.sh` / Windows `run.bat`（默认 8090，`--open` 开浏览器），
  页面内选择串口连接即可遥控；烧录用 `flash.sh/bat`（偏移 0x0/0x8000/0x10000，保留 NVS）。