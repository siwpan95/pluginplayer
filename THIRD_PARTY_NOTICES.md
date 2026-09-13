<!-- 由 tools/gen_third_party_notices.py 生成，请勿手工编辑。
     重新生成: F:\Python314\python.exe tools\gen_third_party_notices.py
     校验最新: F:\Python314\python.exe tools\gen_third_party_notices.py --check
     tools/verify.py 的 third-party-notices 门禁会在提交前跑 --check。 -->

# 第三方组件声明 / Third-Party Notices

PluginPlayer 自身代码以 **`GPL-3.0-or-later`** 分发，见 [LICENSE](LICENSE)（GPLv3 逐字原文）。这与随包 FFmpeg 的 GPL v3 一致 —— 下面这些义务已经由`LICENSE` 的存在与本文件共同满足。本文件列出**随发布物分发**的第三方组件、它们的许可与来源；每个组件仍受其各自许可约束，本文件不改变那些条款。

本文件由 `tools/gen_third_party_notices.py` 从仓库内的原始文件生成（版本号、依赖清单、`dist/` 文件列表都是**读出来的**，不是写死的）。更改依赖后请重新生成。

## 先说结论：发布物整体受 GPL v3 约束

随包的 FFmpeg 来自 gyan.dev 的 **`full_build-shared`**，该构建是 **GPL v3**（不是 LGPL）。因此 `dist/` 这个发布物是 GPL v3 的衍生作品，分发时必须满足：

1. **附上许可全文**（GPL v3 全文，或指向 <https://www.gnu.org/licenses/gpl-3.0.html> 的明确指引）；
2. **标明改动**（若你对上述组件做过修改，必须声明）；
3. **提供对应源码**（Corresponding Source）—— 二进制分发须给出获取方式；
4. **不得附加限制**（不能对下游再分发施加更严格的条款）。

**本项目自身代码的许可（`GPL-3.0-or-later`）已经满足这条约束** —— 见 [LICENSE](LICENSE)。

唯一能摆脱 GPL 的路径是**自建一份 LGPL 配置的 FFmpeg**（`--disable-gpl --disable-version3`，并去掉所有 GPL-only 外部库），再替换 `dist/` 里的 DLL —— 注意大版本号一致才能直接替换（见 README「可选的升级路径」）。当前随包的这份**不支持**这样做。

## 随发布物分发的组件（`dist/`）

| 组件 | 版本 | 许可 | 上游 |
|---|---|---|---|
| FFmpeg | 9.0.1-full_build-www.gyan.dev | **GPL v3** | https://ffmpeg.org/ |
| SDL3 | 3.4.16 | zlib | https://www.libsdl.org/ |
| Cordis / @cordisjs/plugin-loader | @cordisjs/plugin-loader 1.0.0-rc.6, cordis 4.0.0-rc.9 | MIT | https://www.npmjs.com/package/cordis |
| MinGW-w64 GCC 运行时 | GCC 16.1.0 | GPL v3 + GCC Runtime Library Exception | https://www.mingw-w64.org/ |

插件 ABI 版本：`PP_ABI_VERSION = 3`（`src/sdk/player_plugin.h`）。

### FFmpeg —— 随包文件与来源

构建：gyan.dev release-full-shared（64-bit shared, UCRT64 构建）

| 文件 |
|---|
| `avcodec-63.dll` |
| `avformat-63.dll` |
| `avutil-61.dll` |
| `swresample-7.dll` |
| `swscale-10.dll` |

**对应源码（GPL v3 义务 3 的指向）**：本构建的二进制由 FFmpeg 提交 `bf1b838f2a` 构建（随包 README.txt 给出），源码可从 <https://github.com/FFmpeg/FFmpeg/tree/bf1b838f2a> 获取；构建者提供的源码入口见 <https://www.gyan.dev/ffmpeg/builds/>。

> `avfilter` / `avdevice` **不随 `dist/` 分发**（`build.py` 只拷 5 个 FFmpeg DLL，省约 110 MB）。`vendor/ffmpeg/bin` 里那两个 DLL 属于本地开发包，不随发布物走。

判定依据（逐条可复核）：
- 随包档案自带的 README.txt：`License: GPL v3`（原文，非推断）
- 随包 LICENSE 的 SHA-256 与 FFmpeg 源码树里的 COPYING.GPLv3 完全一致 （8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903）
- gyan.dev 构建页原文："All builds are 64-bit, static and licensed as GPLv3"

### SDL3 —— 随包文件

| 文件 |
|---|
| `SDL3.dll` |

依据：随包 LICENSE.txt 原文（"Permission is granted to anyone to use this software for any purpose" + 三条限制）—— 即 zlib 许可的标准措辞

### MinGW-w64 GCC 运行时 —— 随包文件

| 文件 |
|---|
| `libgcc_s_seh-1.dll` |
| `libstdc++-6.dll` |
| `libwinpthread-1.dll` |

依据：GCC 的运行时库受 GPL v3 第 3 节 + Runtime Library Exception 约束；该例外**明确允许**把 libgcc/libstdc++ 随二进制一起分发而不触发传染

### 本项目产物：插件与插件目录清单（与 PluginPlayer 自身同许可，不是第三方）

| 文件 |
|---|
| `plugins/audio_device.dll` |
| `plugins/audio_viz.dll` |
| `plugins/hotkey_ext.dll` |
| `plugins/manifest.sha256` |
| `plugins/osd_stats.dll` |
| `plugins/radio_bridge.dll` |
| `plugins/snapshot.dll` |
| `plugins/subtitles.dll` |
| `plugins/track_selector.dll` |
| `plugins/video_effects.dll` |
| `plugins/video_quality.dll` |
| `plugins/video_tonemap.dll` |

### 其它随包文件（本项目产物或配置）

| 文件 |
|---|
| `assets/SOURCES.md` |
| `assets/fonts/ZCOOLKuaiLe-Regular.ttf` |
| `config/pluginplayer.ini` |
| `libasprintf-0.dll` |
| `libass-9.dll` |
| `libbrotlicommon.dll` |
| `libbrotlidec.dll` |
| `libbrotlienc.dll` |
| `libbz2-1.dll` |
| `libcharset-1.dll` |
| `libexpat-1.dll` |
| `libfontconfig-1.dll` |
| `libfreetype-6.dll` |
| `libfribidi-0.dll` |
| `libgio-2.0-0.dll` |
| `libgirepository-2.0-0.dll` |
| `libglib-2.0-0.dll` |
| `libgmodule-2.0-0.dll` |
| `libgobject-2.0-0.dll` |
| `libgraphite2.dll` |
| `libgthread-2.0-0.dll` |
| `libharfbuzz-0.dll` |
| `libharfbuzz-gobject-0.dll` |
| `libharfbuzz-raster-0.dll` |
| `libharfbuzz-subset-0.dll` |
| `libharfbuzz-vector-0.dll` |
| `libiconv-2.dll` |
| `libintl-8.dll` |
| `libpcre2-16-0.dll` |
| `libpcre2-32-0.dll` |
| `libpcre2-8-0.dll` |
| `libpcre2-posix-3.dll` |
| `libpng16-16.dll` |
| `libunibreak-7.dll` |
| `pluginplayer.exe` |
| `zlib1.dll` |

### `dist/` 载荷

发布载荷共 **57 个文件**（口径同 `build.py --release`，即已剔除运行期产物）。数量与体积的具体口径写在 README 的「发布构建」一节，由 `tools/verify.py` 的 `release-payload` 门禁核对 —— 这里不重复那些数字，避免同一事实两处维护。

## 开发期使用、但不随发布物分发

| 材料 | 许可 | 用途 |
|---|---|---|
| potplayer-master（itisyang/playerdemo） | GPL v3 | 解码/渲染思路的**架构参考**（README「与原 playerdemo 的差异」一节）。内核为按 FFmpeg 9.x 新 API 重写，该目录不随发布物分发。 |
| FFmpeg 源码树（player/ffmpeg-9.0.1） | LGPL v2.1+ / GPL v2+ / GPL v3（FFmpeg 自身为多许可） | 阅读实现、核对 CVE 影响区间。不随发布物分发。 |
| vendor/ 下的开发文件（SDL3 头文件与 libSDL3.dll.a、FFmpeg 头文件与 lib*.dll.a） | 同上游（zlib / GPL v3） | 只用于**编译期链接**，不随发布物分发（.gitignore 已排除 vendor/）。 |

这里刻意把「依赖」和「参考」分开列：`potplayer-master` 是**读来学架构**的参考实现，其文件不进入任何发布物，因此不构成分发、也就不触发它的许可义务。**但如果你曾经从它那里拷贝过代码**，那些代码段仍受其 GPL v3 约束 —— 这件事只有作者本人能确认，本文件不做断言。

## 怎么复核本文件

```bat
:: 1) FFmpeg 版本与构建者（本文件的表格就来自这里）
type vendor\ffmpeg\include\libavutil\ffversion.h

:: 2) FFmpeg 到底是 GPL 还是 LGPL —— 上游档案自带的声明
::    解压 ffmpeg-release-full-shared.7z 后看 README.txt 的 License 行
::    以及对比随包 LICENSE 与 FFmpeg 源码树的 COPYING.GPLv3 是否同一份

:: 3) SDL3 的 zlib 原文
::    SDL3-devel-3.4.16-mingw.zip 内 LICENSE.txt

:: 4) JS 依赖的 license 字段
type host-js\node_modules\cordis\package.json

:: 5) 重新生成本文件并校验是否最新
F:\Python314\python.exe tools\gen_third_party_notices.py
F:\Python314\python.exe tools\gen_third_party_notices.py --check
```

---

事实来源：`vendor/` 头文件、随包档案、`host-js/node_modules`、`dist/` 实际文件。本文件不含生成时间与解释器版本 —— 那类字段会让 `--check` 在不同环境下误报过期。
