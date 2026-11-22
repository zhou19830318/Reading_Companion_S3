/* 功能巡检流程表：串口按键序列 + 日志/画面断言。
   step 词汇：
   {cmd} 调试导航命令（GO_HOME/GO_VOICE/GO_CALENDAR/GO_SETTINGS）
   {k, n, gap, hold} 按键（n 次重复）   {wait} 毫秒等待
   {shot} 取帧作基准   {screen} 取帧并断言画面变化
   {log, ms} 断言窗口内出现匹配日志   {menu: act} .reader 菜单定位到 MenuAction
   {text} 注入文本回合   {audio} 注入内置测试语音   {macro:"reader"} 打开最近书籍 */
"use strict";

const READER_MACRO = [
  { cmd: "GO_HOME" }, { wait: 600 },
  { k: "down", n: 2, gap: 220 }, { k: "ok" }, { wait: 900 },
  { k: "ok" }, { wait: 2600 },
];

window.FLOWS = [
  {
    id: "home-grid", name: "首页九宫格导航", group: "首页",
    steps: [{ cmd: "GO_HOME" }, { wait: 700 }, { shot: true }, { k: "down", n: 9, gap: 200 }, { screen: true }],
  },
  {
    id: "file-browser", name: "浏览文件", group: "首页",
    steps: [{ cmd: "GO_HOME" }, { wait: 500 }, { k: "down", n: 1 }, { k: "ok" }, { wait: 1000 },
      { screen: true }, { k: "down", n: 2, gap: 250 }, { screen: true }, { k: "back", n: 2, gap: 350 }],
  },
  {
    id: "recents", name: "最近书籍", group: "首页",
    steps: [{ cmd: "GO_HOME" }, { wait: 500 }, { k: "down", n: 2 }, { k: "ok" }, { wait: 900 },
      { screen: true }, { k: "back", n: 2, gap: 350 }],
  },
  {
    id: "review-home", name: "今日复习（英文复习）", group: "首页",
    steps: [{ cmd: "GO_HOME" }, { wait: 500 }, { k: "down", n: 3 }, { k: "ok" }, { wait: 1500 },
      { log: "review queue:", ms: 9000 }, { screen: true }, { k: "back", n: 2, gap: 400 }],
  },
  {
    id: "calendar", name: "万年历", group: "首页",
    steps: [{ cmd: "GO_CALENDAR" }, { wait: 800 }, { screen: true }, { k: "right" }, { wait: 600 },
      { screen: true }, { k: "back", n: 2, gap: 350 }],
  },
  {
    id: "market", name: "市场行情", group: "首页",
    steps: [{ cmd: "GO_HOME" }, { wait: 500 }, { k: "down", n: 5 }, { k: "ok" }, { wait: 3000 },
      { screen: true }, { k: "back", n: 2, gap: 400 }],
  },
  {
    id: "ai-text", name: "AI助手 · 文本对话", group: "语音",
    steps: [{ cmd: "GO_VOICE" }, { wait: 1600 }, { text: "你好，请用一句话介绍你自己" },
      { log: "VCHAT\\] reply|relay chat reply", ms: 45000 },
      { screen: true }, { k: "back", n: 2, gap: 400 }],
  },
  {
    id: "ai-audio", name: "AI助手 · 音频注入", group: "语音",
    steps: [{ cmd: "GO_VOICE" }, { wait: 1600 }, { audio: true },
      { log: "external source finished", ms: 20000 },
      { wait: 9000 }, { screen: true }, { k: "back", n: 2, gap: 400 }],
  },
  {
    id: "transfer", name: "文件传输", group: "首页",
    steps: [{ cmd: "GO_HOME" }, { wait: 500 }, { k: "down", n: 7 }, { k: "ok" }, { wait: 900 },
      { k: "ok" }, { wait: 2500 }, { screen: true }, { k: "back" }, { wait: 1500 },
      { screen: true }, { wait: 4000 }],
  },
  {
    id: "pigeon", name: "飞鸽传书", group: "首页",
    steps: [{ cmd: "GO_HOME" }, { wait: 500 }, { k: "down", n: 8 }, { k: "ok" }, { wait: 1200 },
      { screen: true }, { k: "back", n: 2, gap: 400 }],
  },
  {
    id: "settings", name: "系统设置", group: "首页",
    steps: [{ cmd: "GO_SETTINGS" }, { wait: 900 }, { screen: true }, { k: "down", n: 3, gap: 250 },
      { screen: true }, { k: "back", n: 2, gap: 350 }],
  },
  {
    id: "reader-open", name: "打开书籍（阅读器）", group: "阅读",
    steps: [...READER_MACRO, { screen: true }, { k: "back", n: 2, gap: 400 }],
  },
  {
    id: "reader-turn", name: "阅读器左右翻页", group: "阅读",
    steps: [...READER_MACRO, { shot: true }, { k: "right" }, { wait: 800 }, { screen: true },
      { k: "left" }, { wait: 800 }, { screen: true }, { k: "back", n: 2, gap: 400 }],
  },
  {
    id: "read-aloud", name: "朗读模式（逐页 TTS）", group: "阅读",
    steps: [...READER_MACRO, { shot: true }, { k: "ok" }, { wait: 600 }, { menu: 16 }, { k: "ok" },
      { log: "read-aloud start", ms: 9000 }, { wait: 4000 }, { k: "back" },
      { log: "read-aloud stopped", ms: 9000 }, { k: "back", n: 2, gap: 400 }],
  },
  {
    id: "ask-ai", name: "AI书童 · 预设提问", group: "阅读",
    steps: [...READER_MACRO, { shot: true }, { k: "ok" }, { wait: 600 }, { menu: 15 }, { k: "ok" },
      { wait: 1600 }, { k: "ok" }, { log: "\\[ASK\\] relay reply", ms: 40000 },
      { screen: true }, { k: "back", n: 3, gap: 400 }],
  },
  {
    id: "dictionary", name: "查字典 · 字卡", group: "阅读",
    steps: [...READER_MACRO, { shot: true }, { k: "ok" }, { wait: 600 }, { menu: 17 }, { k: "ok" },
      { wait: 1300 }, { k: "ok" }, { log: "\\[DICT\\] reply", ms: 25000 },
      { screen: true }, { k: "back", n: 3, gap: 400 }],
  },
  {
    id: "review-reader", name: "英文复习 · 菜单入口", group: "阅读",
    steps: [...READER_MACRO, { shot: true }, { k: "ok" }, { wait: 600 }, { menu: 18 }, { k: "ok" },
      { wait: 1600 }, { log: "review queue:", ms: 9000 }, { screen: true }, { k: "back", n: 3, gap: 400 }],
  },
  {
    id: "voice-note", name: "语音书签 · 笔记模式", group: "阅读",
    steps: [...READER_MACRO, { shot: true }, { k: "ok" }, { wait: 600 }, { menu: 7 }, { k: "ok" },
      { wait: 1300 }, { screen: true }, { k: "back", n: 3, gap: 400 }],
  },
  {
    id: "bookmark", name: "书签切换", group: "阅读",
    steps: [...READER_MACRO, { shot: true }, { k: "ok" }, { wait: 600 }, { menu: 6 }, { k: "ok" },
      { wait: 900 }, { screen: true }, { k: "back", n: 2, gap: 400 }],
  },
];