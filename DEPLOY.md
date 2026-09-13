# 部署与发布

讲的是**把它发出去、装到别的机器上跑**这件事。开发流程见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

---

## 1. 部署前必须处理的一件事：许可

随包的 FFmpeg 来自 gyan.dev 的 `full_build-shared`，**该构建是 GPL v3**。
因此 `dist/` 是 GPL v3 的衍生作品，分发前必须：附许可全文、标明改动、
提供对应源码的获取方式、不得对下游附加更严格的条款。

完整的逐组件清单、证据与复核命令见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
**在把这批文件发给第三方之前先读那一节。** 只有本机自用、不对外分发时不触发这些义务。

如果你需要非 GPL 的分发（例如闭源再发布），唯一路径是自建一份 LGPL 配置的 FFmpeg
（`--disable-gpl --disable-version3` 并去掉 GPL-only 外部库）再替换 `dist/` 里的 DLL ——
大版本号一致才能直接替换（见 README「可选的升级路径」）。当前随包的这份**不支持**这样做。

---

## 2. 发布载荷

```bat
F:\Python314\python.exe build.py --release
```

`--release` = 干净重建（先删 `build/` 与 `dist/`）+ 生成插件完整性清单 +
清理运行期产物 + 生成 `.bat` 启动器。实测口径：

```
[OK] 已生成 plugins/manifest.sha256（6 个插件）
[release] 可发布文件 17 个，合计 127.0 MB
```

| | 内容 | 说明 |
|---|---|---|
| 内核 | `pluginplayer.exe`（约 509 KB） | 依赖下面这些 DLL |
| FFmpeg 运行时 | `avcodec-63` / `avformat-63` / `avutil-61` / `swscale-10` / `swresample-7` | 93 + 19 + 3 + 2.3 + 0.5 MB，**大头在 avcodec** |
| 图形/音频 | `SDL3.dll` | 5.2 MB |
| MinGW 运行时 | `libwinpthread-1` / `libgcc_s_seh-1` / `libstdc++-6` | 内核与 `video_effects` 需要 |
| 插件 | `plugins/*.dll`（8 个）+ `plugins/manifest.sha256` | 清单存在即强制校验 |
| 配置 | `config/pluginplayer.ini` | 只在不存在时生成，**不覆盖用户已有设置** |

`avfilter` / `avdevice` **不随包**（省约 110 MB）；需要它们的是 `ffmpeg.exe` 命令行工具，
在 `vendor/ffmpeg/bin/` 里，不属于发布内容。

**不带 `--release` 的 `dist/` 会累积**到 300 MB 量级（含 `logs/`、`selftest_out*/`、
`test_assets/`），并且可能残留上一轮手工放进去的 DLL。发之前一定用 `--release`。

`tools/verify.py` 的 `release-payload` 门禁会核对数量与体积与 README 记录一致
（数量必须精确相等，体积容差 ±3 MB）。

---

## 3. 部署到别的机器

**整个 `dist/` 目录一起拷**，不要只拷 exe —— 它依赖同目录的 DLL。

要求：

- **Windows 10 或更新**（FFmpeg 那份构建需要 UCRT；Win7/8 需自行安装 UCRT）。
- **目录可写**：播放器会在 `<exeDir>/logs/` 写日志、在 `<exeDir>/config/` 写令牌与配置。
  放在 `C:\Program Files\` 下会因权限失败 —— 放用户目录里，或预先给目录写权限。
- 路径**别带特殊字符**（`%`、`&` 之类），批处理垫片对它们敏感。
- 不需要安装任何东西，也不写注册表。

运行时行为要点：

| 项 | 默认 |
|---|---|
| 控制通道 | 监听 `127.0.0.1:7911`，**强制 256-bit 令牌**，令牌写 `config/rpc-token.txt`，每次启动重新生成 |
| 插件写文件 | 只允许写进 `dist/`；用 `--write-dir 目录` 或 ini 的 `security.write_dirs` 追加 |
| 网络输入 | `http/rtsp/file/UNC` 等**默认拒绝**，需 `--allow-network-input` |
| 插件完整性 | `plugins/manifest.sha256` 存在即强制校验；**新增/替换插件后必须重新生成**，否则会被拒绝加载 |
| DLL 搜索路径 | 启动时 `SetDefaultDllDirectories`，把当前工作目录移出解析顺序 |

### 无人值守部署

```bat
dist\pluginplayer.exe --hidden --rpc-port 7911 --size 1280x720 D:\media\loop.mp4
```

配合 `pluginplayer` 命令远程控制（见 [docs/CLI.md](docs/CLI.md)）。

两个部署期的坑：

1. **令牌每次启动都会重新生成**。所以固定脚本里不要硬编码令牌 —— 让客户端每次去读
   `dist\config\rpc-token.txt`，或用 `--rpc-token <64 位 hex>` 在启动时钉住。
2. **端口被占时进程仍会继续运行**，但**不会写令牌文件**（刻意如此：否则第二个实例会
   用新令牌覆盖掉正在运行实例的令牌，把它所有客户端一起搞坏）。所以启动脚本里请检查
   标准输出里的 `控制通道: 127.0.0.1:7911` 那一行，别看进程在就算起好了。

不看窗口、又不控制任何东西的场景，最省心的加固是 `--rpc-port 0`（完全不启动控制通道）。

---

## 4. 依赖升级

```bat
F:\Python314\python.exe tools\fetch_deps.py --check-update      :: 只发 HEAD 请求，告诉你上游有没有更新
F:\Python314\python.exe tools\fetch_deps.py --refresh           :: 强制重新抓取
```

- 抓取/校验流程：解压**前**比对 SHA-256 与 `DEPS_SHA256`；不命中会告警并打印实测哈希；
  解压前还用 `7z l -slt` 扫条目，拒绝含 `..` 或绝对路径的归档条目（Zip Slip）；
  下载是"先写 `.part`、成功且长度匹配才改名"。
- `DEPS_SHA256` 是 **TOFU 式**钉值（取自本机曾成功构建过的那份）：能发现"内容被换过"，
  **不能证明来源可信**。来源可信性由 HTTPS + 官方站点保证（SDL 官方站、gyan.dev）。
- FFmpeg 那条 URL 是 gyan.dev 的滚动构建，内容会随上游更新变化，所以硬钉必然过期 ——
  这是"未命中只告警不阻断"的原因。

**升级后必须重跑**：

```bat
F:\Python314\python.exe build.py --release
F:\Python314\python.exe tools\gen_third_party_notices.py      :: 版本号/许可证据变了，重生成
F:\Python314\python.exe tools\verify.py                       :: 文档里的版本号也要同步
pluginplayer selftest
```

版本号变了要同步 README 的「依赖与来源」表 —— `versions-align` 门禁会核对。

---

## 5. 清场与卸载

全部是"删目录即卸载"：没有安装程序、没有注册表项、没有系统目录残留。

```bat
:: 只留发布载荷，清掉运行期产物（日志/自检证据/测试素材/令牌）
F:\Python314\python.exe build.py --release

:: 连构建缓存一起去掉
F:\Python314\python.exe build.py --clean

:: 把 CLI 命令也从 PATH 里移除（只删带自身标记的垫片）
F:\Python314\python.exe tools\pp_cli.py uninstall-cli
```

要交接给别人时，拷贝 `dist/` 之前先确认里面**没有** `config/rpc-token.txt`、
`logs/`、`selftest_out*/`、`test_assets/` —— 令牌文件绝不应该跟着发布物走
（`.gitignore` 也单独排除了它）。

---

## 6. 故障排查

| 现象 | 原因 | 修法 |
|---|---|---|
| 启动报 `0xC0000135` | 找不到 DLL | 确认整个 `dist/` 一起拷了；从别的目录拷 exe 会缺 DLL |
| 窗口起来了但没声音 | 默认音频设备不可用（例如未连接的蓝牙耳机） | 时钟会自动退回墙钟继续播放；换设备或 `--no-audio` |
| 4K 高码率卡顿 | 没做硬件加速（未接 `d3d11va`/`dxva2`） | 已知限制，见 README |
| 日志/截图写不出去 | 目标目录不在写白名单 | `--write-dir <目录>` 或 ini 的 `security.write_dirs` |
| 插件被拒绝加载 | 清单存在但该 DLL 未登记，或哈希不符 | `pluginplayer manifest` 重新生成清单 |
| 标准输出没有「控制通道: ...」 | 端口被占（bind 返回 10048），进程仍在跑但没写令牌 | 换端口，或先结束占用进程 |
| 打开网络地址被拒 | 默认拒绝 `scheme://` 与 UNC | 确需时加 `--allow-network-input`（注意 SSRF/SMB 面） |
| 控制通道连不上 | 用了 `--rpc-port 0`，或端口不符 | 对齐 `--port`；`pluginplayer doctor` 会报端口状态 |

部署后自查：

```bat
dist\pluginplayer.exe --list-plugins        :: 插件全部加载成功才算好
pluginplayer doctor
pluginplayer --json doctor > doctor.json
```

---

## 7. 发布流程（Gitee）

把"发一个版本"从手工点网页变成可复现的命令。工具在 `tools/gitee_release.py`，
每个子命令都是幂等的（已存在就跳过），中断了直接重跑即可。

```bat
:: 0) 令牌：绝不写进仓库。用环境变量或凭据文件指定
set GITEE_TOKEN=<你的令牌>
::   或： --token-file "G:\私人令牌.txt"

:: 1) 全量回归必须先绿（发布构建会清空 dist/，先跑完测试）
ci.bat
F:\Python314\python.exe build.py --release
F:\Python314\python.exe tools\verify.py

:: 2) 打标签并推送（tag 要落在"全量回归通过的那个提交"上）
F:\Python314\python.exe tools\gitee_release.py release --name pluginplayer ^
    --tag v1.0.0 --title "PluginPlayer v1.0.0" --notes-file release-notes.md

:: 3) 上传发行版附件
F:\Python314\python.exe tools\gitee_release.py attach --name pluginplayer ^
    --tag v1.0.0 --file pluginplayer-v1.0.0-win64.zip --file SHA256SUMS.txt
```

### 为什么发**一个 zip**而不是 dist/ 里的 17 个文件

| 事实 | 数字 |
|---|---|
| 发布载荷（解压后） | 17 个文件 / **127 MB** |
| 压成一个 zip | **47.9 MB**（2.65x） |
| Gitee 单附件上限（社区版） | **100 MB** |
| Gitee 仓库总附件上限 | 1 GB |

127MB 的散文件虽然每个都低于 100MB 能传，但**不能用目录路径命名附件**
（`plugins/osd_stats.dll` 会变成 `plugins__osd_stats.dll`，用户还得手动摆目录）。
压缩后一个文件 47.9MB 就够了，用户体验和配额都更好。

### 平台坑（都实测撞过，别再撞第二遍）

- `PATCH /repos/{owner}/{repo}` **必须带 `name` 字段**，否则 `400 name is missing`
  —— 转公开时会踩到。
- `GET /releases/tags/{tag}` 在 tag 不存在时返回 **200 + `null`**，不是 404。
  只判状态码会走进"已存在"分支然后对 `None` 取属性。
- 建仓要用 `auto_init: false`，否则会多一个无关的初始提交，默认分支也不再是你推的那个。
- 发行版响应里的 `html_url` 可能为 `null`；网页地址按
  `https://gitee.com/{owner}/{repo}/releases/{tag}` 构造。
- **发布页上 Gitee 自动附的源码包链接不是直链**：匿名 HTTP 客户端拿到的是一段 HTML。
  GPL 义务要求的源码指向写 `git clone` 地址，不要依赖那两个链接。
- 上传 47.9MB 实测约 20 秒；单附件超过 100MB 会被拒。

### 发布前逐条打勾

- [ ] `ci.bat` 全绿，且 `build.py --release` 的载荷口径与 README 一致（`release-payload` 门禁）
- [ ] `tools/verify.py` 18/18（含 `no-credentials`：**确认没有把令牌提交进去**）
- [ ] `THIRD_PARTY_NOTICES.md` 已重新生成（依赖或许可证据有变时）
- [ ] tag 指向全量回归通过的那个提交
- [ ] 发行说明里有：系统要求、安装步骤、**GPL v3 义务与对应源码指向**、实测数据、已知限制
- [ ] 附件的 SHA-256 写进说明或 `SHA256SUMS.txt`
- [ ] 上传后**回下载一次比对哈希**（别只看"上传成功"）
