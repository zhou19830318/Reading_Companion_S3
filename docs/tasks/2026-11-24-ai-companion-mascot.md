# AI 界面吉祥物："书灵"（2026-11-24）

范围：AI 助手首页、AI 对话/语音、语音书签、生图、AI 书童（Ask AI）、查字典与单词卡。

## 问题

这些屏幕在空闲、等待和失败状态下只有几行文字：AI 助手首页两个列表项下面整屏空白，生图菜单、字典查询中、生图等待等同样如此。缺少角色与空状态引导，看起来像内容没加载出来。

## 调研结论（e-ink 约束）

- 大面积反白/整块黑色是残影最重的区域，浅色描边插画比实心剪影更干净（<https://github.com/balazsbajorics/epaper-playground>）。
- 1-bit 面板上灰阶会被抖动，细线容易消失；应使用纯黑白、粗描边（<https://vp0.com/blogs/e-ink-display-optimized-ui-kit-swiftui>）。
- 空状态应说明这里将来会有什么、下一步做什么，而不是只放一句“暂无”（<https://www.carbondesignsystem.com/building-blocks/core/patterns/empty-states>）。
- 本项目渲染层只有 1-bpp 帧缓冲与 `drawIcon`，动画/彩色不可用；因此采用静态位图 + 一句说明文案。

## 方案

一个统一的卡通角色（纸白色身体 + 粗黑描边的小书灵），按场景换姿势：

| Pose | 用途 |
|---|---|
| `Read` | AI 助手首页主视觉 |
| `Chat` | 对话（备用姿势） |
| `Listen` | AI 对话空闲、语音书签 |
| `Lookup` | 查字典 / 单词卡加载中 |
| `Paint` | 生图菜单与空图库 |
| `Think` | 等 relay 回复 |
| `Sad` | 失败/空状态 |

实现：

- `scripts/generate_mascot_icons.py` 用 Pillow 以 4 倍超采样绘制，阈值化后输出 128×128、1-bpp 打包的头文件；与 `scripts/convert_icon.py` 一样预先逆时针旋转 90°，由 `drawIcon()` 还原。预览图写到 `build/mascot-preview/`（gitignored）。
- 头文件 `src/components/icons/mascot_<pose>128.h` 已提交（固件构建不依赖 Pillow），每张 2 KB，共 7 张 ≈ 14 KB Flash（`.rodata`），**无堆分配**。
- `src/components/CompanionMascot.{h,cpp}` 提供 `draw` / `drawCentered` / `drawBlock`。`drawBlock` 只在盒子放得下“128 px + 一行说明”时绘制，横屏空间不足时自动跳过，不会压住文字或按键提示；说明文案按整字符截断。
- 图标按 8 px 对齐绘制（面板 `drawImageTransparent` 按字节列寻址）。

## 各屏放置

| 屏幕 | 状态 | 位置 |
|---|---|---|
| `AiHubActivity` | 始终 | 吉祥物 + “你的读书小伙伴”与两个入口整组垂直居中；横屏放不下时只保留列表 |
| `VoiceActivity` | Idle | 说明文字下方居中（`Listen`），Listening/Answer 等状态保持原样 |
| `AiImageActivity` | Menu | 两行菜单下方的空白区（`Paint` + “描述画面”） |
| `AiImageActivity` | Working / Failed / 空图库 | 等待（`Think`）、失败（`Sad`）、空图库（`Paint` + 暂无记录） |
| `DictionaryActivity` | Loading / Failed | 查询中（`Lookup`）、失败（`Sad`） |
| `WordCardActivity` | Loading / Failed | 同上 |
| `AskAiActivity` | Asking / Failed | 等待 relay（`Think`）、失败（`Sad`）；预设问题列表保持满屏 |

新增文案：`STR_AI_HUB_TAGLINE`（英/简/繁），其余复用现有 `STR_VOICE_HINT`、`STR_AI_IMAGE_PROMPT`、`STR_VOICE_HISTORY_EMPTY` 等键。

## 验证

- `pio run -e onepage_s3`：成功（Flash 85.1%，其中吉祥物约 14 KB）。
- 图集逐姿势检查（`build/mascot-preview/*.png`），并按 `drawIcon()` 的旋转关系还原验证面板朝向（头部/书本朝上，非倒置）。
- **真机截图**（OnePage S3，`scripts/device_test.py`，抓图为 800×480 帧缓冲，顺时针旋转 90° 即竖屏）：
  - AI 助手首页：书灵 + “你的读书小伙伴” + 两个入口整组居中，图标/文字间距正常。
  - AI 对话空闲：书灵位于说明文字正下方；首版把它居中在剩余空间里会与上方文字脱节，已改为紧贴文字。
  - 生图菜单：书灵固定在「生成新图 / 历史」下方（首版垂直居中显得悬空，已改为锚定）。
  - 生图空图库：书灵 + “暂无历史记录”居中。
  - AI 书童：问题列表与回答页信息密度不变，未插入吉祥物。
- 方向：AI 相关界面只在竖屏渲染（阅读器退出时会恢复竖屏），因此横屏保护只是防御性代码，真机无需验证横屏。
- 未能在真机截到：AI 书童/查字典的“等待中”和失败态——relay 阻塞调用占住主循环，`CMD:SCREENSHOT` 无法在调用期间执行，而本机 relay 在截图前已返回。需在断网或慢 relay 下复测这两个状态。
- 仍需确认：大块白色插画在 FAST_REFRESH 下的残影（本次截图未见异常）。
