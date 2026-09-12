# 随包字体来源

字体来源站点（用户提供）：https://hao.uisdc.com/font/

本机字体资产库：`G:\assets\fonts\`
（其中 `etc/` 下为下载好的免费商用字体压缩包，取自字由/猫啃等渠道）

## 当前随包字体

| 文件 | 字体 | 体积 | 轮廓 |
|---|---|---|---|
| `ZCOOLKuaiLe-Regular.ttf` | 站酷快乐体 2016 修订版 | 1,639,036 B | TrueType `glyf` |

**为什么必须是 TrueType `glyf` 轮廓**：字体渲染用的是 `stb_truetype`，它只解析
TrueType 的 `glyf` 表，**不支持 OTF/CFF**。给它一个 `.otf` 会在 `stbtt_InitFont()`
处失败，OSD 随即回退到 SDL 调试点阵字（不崩，但中文会显示成 `?`）。

## 许可

站酷快乐体：字体发布方（站酷 ZCOOL）声明为免费商用。**该 zip 内没有随附 LICENSE
文本，字体的 name 表也没有 nameID 13/14 许可字段** —— 目前只能依据发布方公开声明
与来源目录判断，未取得可逐字引用的许可原文。这一点已在
`THIRD_PARTY_NOTICES.md` 的"证据"栏如实标注，不要当成已核实。

若需要最强合规证据：换成 Noto Sans SC / 思源黑体（SIL OFL 1.1，raw 上带完整
LICENSE，约 8 MB），同步改 `release-payload` 的文件数与体积声明。

**红线**：微软雅黑（`msyh.ttc` 等）版权属方正，**不可随包分发** —— 本机可以读取
使用，但不能打进发布物。
