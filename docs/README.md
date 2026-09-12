# 文档索引

本目录的文档按"读它的时机"分三类。**先看你此刻要做什么**，再点进对应那份 ——
不需要通读。

## 我要……

| 我要…… | 读这份 | 里面有什么 |
|---|---|---|
| 第一次接触这个项目，搞懂它怎么搭起来 | [ARCHITECTURE.md](ARCHITECTURE.md) | 分层图、线程模型、A-V 同步、精确 seek、事件分发模式、控制通道协议与安全模型、插件 ABI 要点、构建踩坑 |
| 写一个插件（原生 C ABI 或进程外 JS） | [PLUGIN_GUIDE.md](PLUGIN_GUIDE.md) | 两种插件的开发指南，含可编译的最小示例与逐行讲解 |
| 在命令行里用这个项目 | [CLI.md](CLI.md) | `pluginplayer` 命令参考：命令详解、退出码、设计原则、已知边界、排查表 |
| 改代码 / 提交前自查 | [DEVELOPMENT.md](DEVELOPMENT.md) | 工具链、构建与测试的层次、机械门禁清单、提交前该跑什么 |
| 打包发布 / 换依赖版本 | [../DEPLOY.md](../DEPLOY.md) | 发布载荷口径、依赖更新流程、**许可义务（GPL v3）**、部署环境要点 |
| 看依赖许可与来源 | [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) | 逐组件许可、随包文件清单、怎样复核 |
| 让 AI 助手/协作者在这个仓库里干活 | [../AGENTS.md](../AGENTS.md) | 仓库约定、边界与禁区、命令速查、验证入口 |

## 推荐阅读顺序

```
ARCHITECTURE.md  ──►  PLUGIN_GUIDE.md      （想扩展功能）
       │
       ├─────────►  CLI.md                （想用起来）
       │
       └─────────►  DEVELOPMENT.md ──► DEPLOY.md   （想改它、发它）
```

`ARCHITECTURE.md` 是根：其余几份都假设你读过它的前两节（分层与线程模型）。

## 本目录的文档写法约定

这几条是刻意保持的，改文档时请沿用：

1. **一份文档一个主题**，别把"怎么用"和"怎么改"混在一份里 —— 用链接跳转。
2. **已知限制必须如实列出**，包括"没做"和"做得不彻底"的部分。宁可写出
   「测试仍然很薄」这种不好看的句子，也不要让读者误以为它是完整的。
3. **实测数据要标明可复现**：给出命令与输出，而不是"性能不错"这种形容词。
   凡是贴了数字的地方，都应当能用文档里的命令重新得到。
4. **结论前置**：先说要紧的（例如 `THIRD_PARTY_NOTICES.md` 的 GPL 结论放在第一节），
   细节和推导放后面。
5. **行尾不带空格**，段落一行到底（不手工折行），便于 diff 与检索。

## 与机械门禁的关系

下列约定由 `tools/verify.py` 自动核对，写文档时不用靠记：

| 门禁 | 管什么 |
|---|---|
| `doc-links` | 本目录所有 Markdown 里的相对链接必须能解析到真实文件 |
| `docs-index` | **本文件必须收录 `docs/` 下每一份 `.md`**（新增文档别忘了加进上面的表） |
| `readme-tree` | README 的目录结构里列出的文件必须真实存在 |
| `rpc-methods-align` | `ARCHITECTURE.md` 的「方法一览」必须与 `control.cpp` 的实现一致 |
| `exit-codes-align` | `CLI.md` 的退出码表必须与 `tools/pp_cli.py` 的常量一致 |
| `cli-commands-align` | `CLI.md` 与 README 的 CLI 命令表必须与实现一致 |
| `versions-align` | 文档里写的依赖版本必须与 `vendor/` 头文件、`node_modules` 一致 |
| `plugin-command-count` | README / `CLI.md` 声明的命令数必须与插件实际注册数一致 |
| `ci-entrypoints` | 流水线、钩子、`ci.bat` 里引用的工具路径必须存在（改名不会让 CI 静默失效） |

完整清单（18 条，含"哪条在缺 `vendor/` 时记 SKIP"）见 [DEVELOPMENT.md](DEVELOPMENT.md) §4。

```bat
:: 改完文档跑一遍就知道有没有写散
F:\Python314\python.exe tools\verify.py
```
