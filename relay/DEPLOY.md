# VoiceRelay 部署手册（DEPLOY）

本文是服务端**部署与运维**手册。协议、接口、配置项含义见同目录 [README.md](./README.md)。

## 架构总览

```
阅读器 ──ws://<server-ip>:8080/api/v1/voice/stream──▶ nginx (:8080, dev)
                                                        │
                                          uvicorn 127.0.0.1:8000 (单 worker)
                                                        │
                            ┌───────────────────────────┴──────────────────────┐
                            ▼                                                  ▼
                    DashScope 实时 ASR                                DashScope LLM / 生图
                  (paraformer-realtime-v2)                          (qwen-flash / wanx)
```

- 设备**只持有 token**，云厂商 API Key 只存在服务器上。
- 万年历与朗读走同一台 relay：`GET /api/v1/weather`（IP 定位 + open-meteo）与
  `WS /api/v1/tts`（edge-tts -> 16 kHz PCM），两者都不需要云厂商 key。
- 飞鸽传书走同一个 relay 的 HTTPS JSON（`/api/v1/pigeon/`，由 nginx 代理），
  只共享设备认证，不经过 DashScope。
- 进程由 systemd 托管，单进程（多 worker 会破坏全局限流计数，见 README "Multiple onepage devices"）。

## 服务器目录结构

| 路径 | 内容 |
|---|---|
| `/opt/reading-companion-relay/releases/<TS>/` | 每次安装生成的时间戳 release（`relay/`、`dict-data/`、`requirements.txt`、独立 `.venv`、`pip-freeze.txt`） |
| `/opt/reading-companion-relay/current` | 指向生效 release 的软链（回滚=改指旧 release） |
| `/opt/reading-companion-relay/logs/`、`run/` | 日志与可写运行时目录（含 `run/t2i/` 生图资产、`run/learning/`、`run/pomodoro/`、`run/pigeon/`、`run/devices/` 设备注册表、`run/library/` 书库与上传原文件；systemd 沙箱仅放行这两处写入） |
| `/etc/reading-companion/voice-relay.env` | **唯一存放密钥的文件**（root:voice-relay，640） |
| `/etc/systemd/system/reading-companion-relay.service` | systemd 单元（install.sh 安装） |
| `/etc/nginx/sites-available/reading-companion-relay.conf` | nginx 站点（dev=8080 / prod=443） |

查看当前生效版本：

```bash
readlink -f /opt/reading-companion-relay/current
```

## 前置条件

- Debian/Ubuntu 系服务器 + sudo（`install.sh` 以 root 运行，缺的 apt 包会自动补装：python3、python3-venv、nginx、curl、jq 等）。
- 朗读（TTS）与万年历天气**不需要系统额外的二进制**：`edge-tts` 与 `miniaudio` 都是 pip 包（`miniaudio` 自带 MP3 解码器），`requirements.txt` 已列出，`install.sh` 的 venv 步骤会自动装好。
- 出网要求（已按大陆主机实测）：`api.open-meteo.com` ✓、`ipinfo.io` 与 `api.ip.sb` ✓、`ip-api.com` ✗（不通，仅作第三顺位兜底）、`api.ipify.org` ✗（返回空，故出口 IP 改走 `api.ip.sb/ip`）、`speech.platform.bing.com`（edge-tts 上游）✓。若换到其它地区的主机，第一顺位的地理服务可以不改，`WEATHER_LAT`/`WEATHER_LON` 也能直接钉死位置。
- 或 Windows 10/11 x64（安装包自带 Python 运行时，**无需预装 Python**）。
- 云安全组放行 **TCP 8080**（dev 模式）；prod 模式需域名 + 443。
- 本机有仓库检出（打包用）与 `scp` 到服务器的权限。

## 0. 一键部署（推荐）

### Ubuntu / Debian（一条命令）

```bash
curl -fsSL https://raw.githubusercontent.com/zhou19830318/Reading_Companion_S3/main/relay/deploy/bootstrap.sh \
  | sudo bash -s -- --dashscope-key sk-...
```

- 不带 key = mock 模式（固定回复，用于联调）；之后重跑带 `--dashscope-key` 即可切换真实语音。
- 其余参数原样透传给 `install.sh`（`--mode prod --domain ...`、`--device-id`、`--public-url`、`--asr-model` 等）。
- 离线/无 GitHub 访问：`sudo bash bootstrap.sh --tarball /path/to/voice-relay-<TS>.tar.gz [参数]`。
- 脚本幂等，重复执行即升级；结束会打印设备三要素与安装摘要。
- 也可用环境变量传 key：`DASHSCOPE_API_KEY=sk-... sudo -E bash bootstrap.sh`。

### Windows（安装包）

CI 构建：GitHub Actions `Relay Windows installer`（`.github/workflows/relay-windows.yml`），推送 `relay/**`、手动触发或发布 Release 均会产出 artifact `voice-relay-windows-installer`。

本地构建（需 Python 3.12 + Inno Setup 6）：

```powershell
./relay/deploy/windows/build.ps1 -Version v1.5.0
# 产物: build/windows/out/voice-relay-setup-v1.5.0.exe
```

双击安装（管理员）自动完成：解压运行时（无需 Python）→ 生成 `C:\ProgramData\ReadingCompanion\voice-relay.env`（随机 device/admin token）→ 注册并启动 Windows 服务 `ReadingCompanionVoiceRelay`（开机自启、崩溃自动重启）→ 防火墙放行 8080/tcp（仅专用网络）→ 等待 `/healthz` 就绪 → **自动打开管理页面**。

服务管理（管理员 PowerShell，目录为 `C:\Program Files\ReadingCompanionVoiceRelay`）：

```powershell
.\voice-relay-service.exe status|stop|start|restart
# 日志: .\logs\（按大小滚动）；配置: C:\ProgramData\ReadingCompanion\voice-relay.env
```

升级 = 直接运行新安装包（安装器会先停服再覆盖，env/token 保留）；卸载通过“应用和功能”或开始菜单快捷方式。

### 管理页面（两个平台通用）

- Ubuntu：<http://127.0.0.1:8000/admin>
- Windows：<http://127.0.0.1:8080/admin>

显示服务状态、ASR/LLM/字典/生图 provider、设备三要素（Relay URL / Device ID / Token，一键复制）、局域网 IP 与并发限制。页面仅允许本机访问（127.0.0.1/::1），nginx 不代理 `/admin`；无桌面的 Ubuntu 用 SSH 隧道：

```bash
ssh -L 8000:127.0.0.1:8000 <user>@<server-ip>
# 本地浏览器打开 http://127.0.0.1:8000/admin
```

设置了 `RELAY_ADMIN_TOKEN` 时，也可用 `?token=<admin-token>` 远程访问。`RELAY_PUBLIC_URL` 可覆盖页面上显示的 Relay URL（经域名/TLS 反代时使用）。

## 0.1 AI 原生阅读（书库 / 引用问答 / 笔记同步）

V1 服务端接口契约见仓库内 [docs/ai-native/api-v1.md](../docs/ai-native/api-v1.md)。部署后：

- **上传书籍**：`/admin` 的“阅读书库”卡片可上传 EPUB/TXT、查看索引状态与章节/段落数、软删除书籍；下方“笔记与引用”只读展示设备同步上来的记录。上传接口也开放给设备：`POST /api/v1/books`（设备 token）。
- **索引**：默认后台解析。状态依次为 `pending → parsing → indexing → ready`，失败为 `failed`；设备通过摘要或引用问答发现 `BOOK_NOT_FOUND` 后，自动上传当前 EPUB 并轮询 `GET /api/v1/books/{id}/processing`，就绪后重试原请求。上传与轮询总时限 10 分钟；失败或书籍不受支持时显示原因，引用问答只有重试后仍缺失或引用段落缺失时才明确说明并降级为无引用回答。
- **问答**：`POST /api/v1/reader/query` 需要真实 LLM provider（`LLM_PROVIDER=mock` 时只能联调协议，默认回复不是结构化 JSON，会被降级为 `[推断]`）。服务端强制校验每条引文必须是所送段落的子串，伪造引用会被丢弃并把 `book` 段降级为 `inference`。
- **同步**：`POST /api/v1/sync/push` / `GET /api/v1/sync/pull`，`op_id` 幂等；冲突不会静默覆盖。只同步笔记/引用/AI 摘要，阅读进度仍走原 KOReader 同步。

相关数据都在 `run/library/`：

```text
run/library/library.db      # books/chapters/passages/FTS/annotations/sync
run/library/books/<sha256>.<ext>   # 上传的原文件
```

备份时把整个 `run/library/` 一起带走；在线备份 SQLite 建议用 `sqlite3 run/library/library.db ".backup /path/library-backup.db"`，避免直接拷贝 WAL 状态。删除书籍是软删除（`deleted=1`），原文件与历史引用保留，便于设备笔记继续解析。

nginx 示例（dev/prod 两套配置已包含）：

- `/api/v1/books`：`client_max_body_size 70m`（默认上传上限 64 MiB + multipart 开销）；
- `/api/v1/reader/`：关闭 buffering，读超时 300s；
- `/api/v1/sync/`：4 MiB JSON 批次上限。

`/admin` 仍只允许本机访问或 `?token=`，反代不会暴露它；远程管理请用 SSH 隧道。生产环境务必给设备入口配 TLS，否则 token 与书稿内容都是明文。

## 1. 打包

在仓库的 `relay/` 目录执行：

```bash
bash tools/make-package.sh
# 产物: dist/voice-relay-<TS>.tar.gz
# 内容: relay/ + dict-data/ + deploy/ + tools/ + requirements.txt + README.md + DEPLOY.md
```

`dict-data/`（离线字典数据）是打包**必需**项；缺失时脚本报错退出。

## 2. 上传并安装

```bash
scp dist/voice-relay-<TS>.tar.gz <user>@<server-ip>:/tmp/
ssh <user>@<server-ip>
tar xzf /tmp/voice-relay-<TS>.tar.gz -C /tmp
sudo bash /tmp/voice-relay-<TS>/deploy/install.sh \
    --source /tmp/voice-relay-<TS> \
    --mode dev \
    --dashscope-key sk-...        # 可省略；省略且在终端时会交互提示（回车=mock 模式）
```

常用参数：`--device-id`（设备 ID）、`--asr-model` / `--llm-model`、`--dashscope-region`、`--public-url`（手动生成设备连接地址）、`--mode prod --domain voice.example.com`。

`install.sh` 是**幂等**的，一次执行完成：

1. 校验 `--source` 含 `relay/` 与 `requirements.txt`；
2. 建 `voice-relay` 系统用户与目录；
3. 复制出新 release（去 `__pycache__`），建 `.venv` 并 `pip install -r requirements.txt`（写入 `pip-freeze.txt`）；
4. env 文件：首次生成（随机 device/admin token），已存在则保留并**回填**新增配置键、更新 `RELAY_VERSION`；
5. `current` 软链切到新 release，安装/重启 systemd 服务；
6. 安装 nginx 站点（dev/prod）并 reload；
7. 冒烟测试：`127.0.0.1:8000/healthz`、`/readyz`（校验 `dict=local`），dev 模式再测 `:8080`；
8. 清理旧 release（保留最近 5 个）；
9. 打印阅读器需要的 **Relay URL / Device ID / Token**。

## 3. 配置阅读器

1. 阅读器联网，打开其 Web UI（`http://<reader-ip>/settings`）；
2. **Voice Relay** 卡片粘贴安装脚本输出的三项（也可从管理页面一键复制）：

   ```
   Relay URL:  ws://<server-ip>:8080/api/v1/voice/stream
   Device ID:  reading-companion-001
   Token:      <RELAY_DEVICE_TOKEN>
   ```

   多台设备共用同一个 relay 时必须使用**不同的 Device ID**（例如
   `reading-companion-001` / `reading-companion-002`）；飞鸽传书会按这些
   ID 互相发现和寻址。

3. 保存（设备侧 Voice Relay 开关默认开启）。设备端无需任何额外密钥——relay 是唯一的云端后端。

## 4. 密钥与配置变更

所有配置都在 `/etc/reading-companion/voice-relay.env`（改完必须重启）：

```bash
sudoedit /etc/reading-companion/voice-relay.env
sudo systemctl restart reading-companion-relay
```

部署后最常见的三件事（完整键表见 `deploy/voice-relay.env.example` 与 README）：

| 目的 | 改动 |
|---|---|
| 从 mock 切到真实语音/LLM | `DASHSCOPE_API_KEY=sk-...`、`ASR_PROVIDER=paraformer`、`LLM_PROVIDER=dashscope-openai-compatible`；或直接重跑 `install.sh --dashscope-key sk-...` |
| 开通生图 | `T2I_PROVIDER=dashscope`（或 `openai` 配 `T2I_BASE_URL`/`T2I_API_KEY`；`mock`=本地占位图零成本） |
| 轮换设备 token | `RELAY_DEVICE_TOKEN=...`（或 `RELAY_DEVICE_TOKENS=device:token,...` 按设备区分） |
| 词库学习数据位置 | `RELAY_LEARNING_DIR`（默认 `run/learning`；生产钉在 `$RELAY_ROOT/run/learning`，跨 release 持久，无需改 systemd unit） |
| 番茄钟会话数据位置 | `RELAY_POMODORO_DIR`（默认 `run/pomodoro`；生产钉在 `$RELAY_ROOT/run/pomodoro`，同词库规则） |
| 飞鸽传书消息库 | `RELAY_PIGEON_DIR`（默认 `run/pigeon`）；`PIGEON_MESSAGE_TTL_DAYS`（7）、`PIGEON_MAX_MESSAGES_PER_RECIPIENT`（100）、`PIGEON_MAX_TEXT_BYTES`（512）、`PIGEON_RATE_LIMIT_PER_MINUTE`（30）、`PIGEON_PRESENCE_TTL_SECONDS`（45） |
| 服务端设备注册表 | `RELAY_DEVICE_DIR`（默认 `run/devices`；`devices.json` 存 Relay URL / Token，0600）。新增设备后注册表即成为设备鉴权权威 |
| 行情超时/缓存 | `MARKET_TIMEOUT_S`（10）、`MARKET_QUOTE_TTL_S`（60）、`MARKET_CHART_TTL_S`（900）、`MARKET_STALE_S`（86400）。行情只读代理腾讯/Yahoo/CNBC；美股指数 1d 优先 CNBC，东方财富（WAF 会掐断云主机 IP）、腾讯、新浪依次兜底。无密钥；nginx 已代理 `/api/v1/markets/` |

字典数据随 release 发布（`DICT_SOURCE=local`），升级 release 即升级词库；改成 `DICT_SOURCE=llm` 可回退到模型生成卡片（可能有事实错误，不推荐）。

词库学习数据（生词本、复习记录，SQLite）存放在 `RELAY_LEARNING_DIR`，位于 release 目录之外的 `run/` 树中：升级与回滚都不会动它；备份即拷走该目录下的 `learning.db*` 文件。

番茄钟会话数据（专注/休息段记录，SQLite）存放在 `RELAY_POMODORO_DIR`，规则与词库完全一致；备份即拷走该目录下的 `pomodoro.db*` 文件。

设备注册表同样位于 release 之外的 `run/` 树：

```bash
sudo cp -a /opt/reading-companion-relay/run/devices/devices.json /path/to/backup/
```

`devices.json` 含各设备 Token，按密钥文件管理；恢复时停服后
放回原路径再启动。注册表一旦非空，未登记的 Device ID 会被拒绝，迁移前先把
所有在用的设备加入 `/admin/devices`。

### 4.1 飞鸽传书（设备间消息）

飞鸽传书消息存放在 `RELAY_PIGEON_DIR`（生产为
`$RELAY_ROOT/run/pigeon/pigeon.db`，WAL 模式会同时有 `-wal` / `-shm`），
和词库/番茄钟一样位于 release 目录之外，升级或回滚都不影响消息。备份时
先停服或使用 SQLite 在线备份，再拷走 `pigeon.db*`；恢复时停服后放回原路径
再启动：

```bash
sudo systemctl stop reading-companion-relay
sudo cp -a /opt/reading-companion-relay/run/pigeon/pigeon.db* /path/to/backup/
# 恢复：停服后把备份文件放回 run/pigeon/，再 start
sudo systemctl start reading-companion-relay
```

- **设备注册**：每台设备通过 `POST /api/v1/pigeon/heartbeat` 注册，已注册设备
  离线时仍可作为收件人；presence 在线状态只存在进程内，relay 重启后由下一次
  heartbeat 自动恢复。管理员可在 `ALLOWED_DEVICE_IDS` 或
  `RELAY_DEVICE_TOKENS` 中预置设备 ID，这样设备首次上线前就能被寻址。
- **保留与清理**：发送时自动删除超过 `PIGEON_MESSAGE_TTL_DAYS` 的消息，
  并只保留每个接收方最近 `PIGEON_MAX_MESSAGES_PER_RECIPIENT` 条。需要立即
  清空某段会话时，在设备端进入会话按“清空聊天”（会删除双方消息），或调用：

  ```bash
  TOKEN=...   # 设备 token
  curl -s -X POST \
    -H "Authorization: Bearer $TOKEN" -H "X-Device-Id: reading-companion-001" \
    "localhost:8080/api/v1/pigeon/clear?peer=reading-companion-002" | jq .
  # {"ok":true,"deleted":N}
  ```

  这是**双向硬删除**，没有“只删本机”选项，也不会留下墓碑；操作前确认
  对方也同意删除。
- **限额**：正文最大 512 UTF-8 字节、每发送方 30 条/分钟、每接收方保留
  100 条；超出时 `send` 返回 `text_too_long` / `rate_limited`，设备会显示
  发送失败。调大上限前先确认设备端输入与显示能接受。
- **监控**：`journalctl -u reading-companion-relay | grep pigeon` 可看到
  heartbeat、send、auth rejected 与 rate limited 日志；`pigeon.db` 只含
  消息元数据和正文，备份文件按敏感数据保管。

## 5. 升级

重新 `make-package.sh` → 上传 → 重跑 `install.sh --source ...`。环境文件、token、密钥全部保留；旧 release 自动清理（留 5 个）。

- 一键脚本：重跑同一条 `bootstrap.sh` 命令即可。
- Windows：直接运行新版 `voice-relay-setup-<ver>.exe`（先停服再覆盖，env/token 保留）。

## 6. 回滚

```bash
ln -sfn /opt/reading-companion-relay/releases/<old-TS> \
        /opt/reading-companion-relay/current
sudo systemctl restart reading-companion-relay
```

`<old-TS>` 从 `ls /opt/reading-companion-relay/releases/` 里挑（按时间倒序）。

## 7. 验证与排障

```bash
systemctl status reading-companion-relay --no-pager
journalctl -u reading-companion-relay -f              # 结构化 JSON 一行一事件
journalctl -u reading-companion-relay --since "5 min ago" | grep contracts   # 上游契约门

curl -s localhost:8000/healthz | jq .                 # 存活
curl -s localhost:8000/readyz  | jq .                 # asr/llm/dict/t2i 配置状态
curl -s localhost:8080/healthz | jq .                 # 经 nginx（dev）
# 飞鸽传书能力键（需设备 token + X-Device-Id）
curl -s -H "Authorization: Bearer $TOKEN" -H "X-Device-Id: reading-companion-001" \
     localhost:8080/api/v1/device/config | jq '.features.pigeon'   # true
```

- `/readyz` 里 `"dict": "local"` 才说明离线字典生效；`local_missing_data` 表示 release 缺 `dict-data/`。
- 飞鸽传书联系人看不到对方：确认两台设备填了不同 Device ID、指向同一个 relay，
  且都已成功 heartbeat；可直接查注册表：
  `sudo python3 -c "import sqlite3;print(sqlite3.connect('/opt/reading-companion-relay/run/pigeon/pigeon.db').execute('select device_id,name,last_seen from devices').fetchall())"`。
  看不到 `features.pigeon` 说明设备连到了旧 relay，重新部署即可。
- Windows 日志：安装目录 `logs\voice-relay-service.out.log` / `.err.log`；管理页打不开时先 `.\voice-relay-service.exe status`。
- 无硬件联调：`python tools/test_client.py --pcm speech.wav`（协议）；`--realtime` 走真实上游；4 设备压测 `tools/load_test.py --clients 4 --rounds 5`。
- 设备连不上：先确认安全组 8080、nginx 站点已 enable、token 与 env 一致（`sudo grep RELAY_DEVICE_TOKEN /etc/reading-companion/voice-relay.env`）。
- 日志脱敏过滤器保证 API key 不会进日志；`/metrics` 仅内网可访问。

## 8. dev 与 prod 模式

| | dev（当前现网） | prod |
|---|---|---|
| 入口 | nginx `:8080`，`ws://` | nginx `:443`，`wss://`（需域名 + 证书） |
| 安装 | `--mode dev` | `--mode prod --domain voice.example.com` |
| 云安全组 | 开 TCP 8080 | 开 TCP 443 |

## 9. 安全基线

- 长期云密钥在 `voice-relay.env`（root:voice-relay 640）；设备 Token 在 `run/devices/devices.json`（0600）。它们永不入库、永不下发到设备（设备只拿到自己的 Token 和 relay 配置），也不回显到 Web UI。
- 管理页面 `/admin` **仅本机可访问**（127.0.0.1/::1），nginx 不代理；需要远程时用 SSH 隧道或 `RELAY_ADMIN_TOKEN`。
- Windows 防火墙规则只对“专用网络”放行 8080/tcp；公网使用请加 Caddy/nginx 反代 wss://。
- systemd 沙箱：`ProtectSystem=strict` + 仅放行 `logs/`、`run/` 两个写路径；`NoNewPrivileges`、`PrivateTmp`。
- 设备认证靠 `Authorization: Bearer` + 可选 `ALLOWED_DEVICE_IDS` 白名单；空 token 配置拒绝启动。注册表非空时改用 `/admin/devices` 中的设备与 Token（旧 Token 轮换后保留 15 分钟）。
- 飞鸽传书正文以明文存放在 `run/pigeon/pigeon.db`（仅服务器磁盘，不经云端）；
  备份、快照和磁盘镜像同样包含这些内容，按敏感数据管理。清空接口只删除
  消息，不删除设备注册表。
- 结构化日志带脱敏过滤，API key 不可能被打印。
