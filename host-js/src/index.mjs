/**
 * index.mjs — 进程外 JS 插件宿主（Cordis）
 *
 * 用法:
 *   node src/index.mjs [--port 7911] [--plugins ./plugins] [--host 127.0.0.1]
 *
 * 工作方式:
 *   1. 建立 Cordis 根上下文
 *   2. 装载 PlayerService（把原生内核的控制通道变成 ctx.player）
 *   3. 扫描 plugins/ 下的 *.mjs，逐个 ctx.plugin() 装载
 *   4. 每个插件的所有注册都通过 ctx.effect / ctx.on 完成，
 *      Cordis 会在插件卸载时自动撤销 —— 与 C 版插件的「可撤销注册」语义一致
 */
import { Context } from 'cordis'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const __dirname = path.dirname(fileURLToPath(import.meta.url))
const ROOT = path.resolve(__dirname, '..')

function parseArgs(argv) {
  const out = {
    host: '127.0.0.1', port: 7911, plugins: path.join(ROOT, 'plugins'),
    exitAfter: 0, token: '', tokenFile: '',
  }
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    if (a === '--port') out.port = parseInt(argv[++i], 10)
    else if (a === '--host') out.host = argv[++i]
    else if (a === '--plugins') out.plugins = path.resolve(argv[++i])
    else if (a === '--exit-after') out.exitAfter = parseInt(argv[++i], 10)
    else if (a === '--token') out.token = argv[++i]
    else if (a === '--token-file') out.tokenFile = path.resolve(argv[++i])
    else if (a === '--help' || a === '-h') { out.help = true }
  }
  return out
}

const args = parseArgs(process.argv.slice(2))
if (args.help) {
  console.log(`PluginPlayer JS 插件宿主（Cordis）
用法: node src/index.mjs [选项]
  --host H        原生内核地址（默认 127.0.0.1）
  --port N        控制通道端口（默认 7911）
  --plugins DIR   JS 插件目录（默认 host-js/plugins）
  --exit-after N  运行 N 秒后自动优雅退出（用于自检/脚本，默认 0=常驻）
  --token HEX     控制通道令牌（默认自动读取 ../dist/config/rpc-token.txt）
  --token-file P  指定令牌文件路径

安全说明: 控制通道强制令牌认证。令牌由播放器启动时随机生成并写入
          dist/config/rpc-token.txt，也可用环境变量 PP_RPC_TOKEN 提供。`)
  process.exit(0)
}

const root = new Context()
const { PlayerService } = await import('./player-service.mjs')

/** 记录每个插件返回的 Fiber —— 卸载时按逆序 dispose()，
 *  这正是 Cordis「注册是可逆的副作用」的落地方式。 */
const fibers = []
fibers.push(await root.plugin(PlayerService))
root.player.start({
  host: args.host, port: args.port,
  token: args.token, tokenFile: args.tokenFile,
})

/* ---- 发现并装载 JS 插件 ---- */
const pluginDir = args.plugins
let loaded = 0
let failed = 0

if (fs.existsSync(pluginDir)) {
  const files = fs.readdirSync(pluginDir).filter((f) => f.endsWith('.mjs')).sort()
  for (const f of files) {
    const full = path.join(pluginDir, f)
    try {
      const mod = await import(pathToFileURL(full).href)
      const plugin = mod.default
      if (!plugin) throw new Error('缺少 default 导出')
      const cfg = typeof mod.config === 'object' ? mod.config : {}
      fibers.push(await root.plugin(plugin, cfg))
      loaded++
      console.log(`[host-js] 已装载 JS 插件: ${plugin.name || f}`)
    } catch (e) {
      failed++
      console.error(`[host-js] 装载失败 ${f}: ${e.message}`)
    }
  }
} else {
  console.error(`[host-js] 插件目录不存在: ${pluginDir}`)
}

/* ---- 连通性自检：等一小会儿再报告，便于自动化脚本判断 ---- */
setTimeout(async () => {
  let tip = '未连接（请先启动 pluginplayer.exe --rpc-port ' + args.port + '）'
  try {
    const pong = await root.player.call('ping')
    const st = await root.player.stats()
    tip = `已连接 abi=${pong.abi}  position=${st.position.toFixed(2)}s ` +
          `paused=${st.paused}  fps=${st.fps.toFixed(1)}`
  } catch (e) {
    tip += ` (${e.message})`
  }
  console.log(`[host-js] JS 插件 ${loaded} 个（失败 ${failed}）  控制通道: ${tip}`)
  console.log(`[host-js] 服务计数: 请求 ${root.player.counter.requests} / 事件 ${root.player.counter.events}`)
}, 1500)

/* ---- 优雅退出：SIGINT / SIGTERM / --exit-after 到点 ---- */
let shuttingDown = false
const shutdown = async (why) => {
  if (shuttingDown) return
  shuttingDown = true
  console.log(`\n[host-js] 收到 ${why}，按逆序卸载 ${fibers.length} 个 Fiber…`)
  for (let i = fibers.length - 1; i >= 0; i--) {
    try { await fibers[i].dispose() } catch (e) { console.error(`[host-js] dispose 失败: ${e.message}`) }
  }
  console.log('[host-js] 已全部卸载')
  process.exit(0)
}
process.on('SIGINT', () => shutdown('SIGINT'))
process.on('SIGTERM', () => shutdown('SIGTERM'))
if (args.exitAfter > 0) {
  setTimeout(() => shutdown(`--exit-after ${args.exitAfter}s`), args.exitAfter * 1000)
}
