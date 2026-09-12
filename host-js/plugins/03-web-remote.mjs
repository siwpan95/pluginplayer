/**
 * 03-web-remote.mjs — 网页远程控制插件（JS 侧，进程外，零依赖）
 *
 * 演示能力：
 *   - 进程外插件可以提供自己的 UI（原生 C 插件也能，但写 HTML 显然更省事）
 *   - 单个 HTTP 服务同时提供静态页与 JSON 接口
 *   - 通过 ctx.player 调用原生内核：播放/暂停/跳转/音量/倍速，以及调用任何原生插件命令
 *   - ctx.effect() 保证卸载时 server 一定被关闭（可撤销副作用）
 *
 * ============================ 安全模型（重要） ============================
 * 本页能驱动内核执行**任意原生插件命令**（含写文件、退出进程），因此默认**关闭**，
 * 且开启后必须过下面五道关（对应上线前安全审计 F-04 / F-14）：
 *   1) 必须显式开启：config.enabled = true 或环境变量 PP_WEB_REMOTE=1
 *   2) Host 头白名单（只认 127.0.0.1 / localhost / [::1] + 本端口）
 *      —— 这一条专门挡 DNS rebinding
 *   3) Origin 头白名单（同源才放行；伪造 Origin 一律 403）—— 挡 CSRF
 *   4) 所有 /api/* 必须带 X-PP-Token，值为控制通道令牌
 *      —— 自定义头会触发浏览器预检，而我们不返回任何 CORS 头，跨域请求直接失败
 *   5) POST 必须 Content-Type: application/json
 * 另外：响应统一带 nosniff / CSP / X-Frame-Options / Referrer-Policy，且**不回显**输入。
 * ==========================================================================
 */
import http from 'node:http'
import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { fileURLToPath } from 'node:url'

const __dirname = path.dirname(fileURLToPath(import.meta.url))
/** 播放器启动时把控制通道令牌写在这里（见 src/core/main.cpp） */
const DEFAULT_TOKEN_FILE = path.resolve(__dirname, '..', '..', 'dist', 'config', 'rpc-token.txt')

const PAGE = `<!doctype html>
<html lang="zh-CN"><meta charset="utf-8">
<title>PluginPlayer 远程控制</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  :root{--bg:#fff;--fg:#1b1e23;--mut:#6b7280;--line:#e5e7eb;--acc:#2563eb;--acc2:#f3f4f6}
  @media (prefers-color-scheme:dark){:root{--bg:#16181d;--fg:#e8eaee;--mut:#9aa0aa;--line:#2b2f38;--acc:#60a5fa;--acc2:#22262e}}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.6 system-ui,"Segoe UI",sans-serif}
  .wrap{max-width:760px;margin:0 auto;padding:24px 18px 60px}
  h1{font-size:19px;margin:0 0 4px}
  .sub{color:var(--mut);font-size:12.5px;margin-bottom:18px}
  .card{border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin-bottom:14px}
  .row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
  button{background:var(--acc2);color:var(--fg);border:1px solid var(--line);border-radius:7px;
         padding:7px 13px;font-size:13px;cursor:pointer}
  button:hover{border-color:var(--acc)}
  button.p{background:var(--acc);color:#fff;border-color:transparent}
  input[type=range]{width:190px}
  input[type=text]{padding:7px 9px;border:1px solid var(--line);border-radius:7px;
                   background:transparent;color:var(--fg);flex:1;min-width:260px}
  .bar{height:8px;background:var(--acc2);border-radius:99px;overflow:hidden;margin:10px 0 6px}
  .bar>i{display:block;height:100%;background:var(--acc);width:0}
  .kv{display:grid;grid-template-columns:104px 1fr;gap:2px 10px;font-size:12.5px}
  .kv b{color:var(--mut);font-weight:500}
  code{background:var(--acc2);padding:1px 5px;border-radius:4px;font-size:12px}
  .st{font-size:12px;color:var(--mut)}
  .err{color:#c0392b}
</style>

<div class="wrap" id="gate" hidden>
  <h1>需要控制通道令牌</h1>
  <div class="sub">本页可以驱动本机播放器执行任意插件命令，因此必须提供令牌。</div>
  <div class="card">
    <div class="row">
      <input type="text" id="tok" placeholder="粘贴 dist/config/rpc-token.txt 的内容">
      <button class="p" id="tokgo">确定</button>
    </div>
    <div class="st err" id="tokmsg"></div>
    <div class="st" style="margin-top:8px">
      令牌由播放器每次启动时随机生成，位于 <code>dist/config/rpc-token.txt</code>。
    </div>
  </div>
</div>

<div class="wrap" id="main" hidden>
  <h1>PluginPlayer 远程控制</h1>
  <div class="sub">由 JS 插件 <code>03-web-remote.mjs</code> 提供 · 通过控制通道驱动原生内核</div>

  <div class="card">
    <div class="row">
      <button class="p" id="toggle">播放 / 暂停</button>
      <button data-seek="-10">-10s</button>
      <button data-seek="10">+10s</button>
      <button data-seek="-60">-1m</button>
      <button data-seek="60">+1m</button>
    </div>
    <div class="bar"><i id="prog"></i></div>
    <div class="row" style="justify-content:space-between">
      <span class="st" id="time">--:-- / --:--</span>
      <span class="st" id="pos"></span>
    </div>
  </div>

  <div class="card">
    <div class="row">
      <label>音量</label><input type="range" id="vol" min="0" max="100" value="100">
      <span class="st" id="volt">100%</span>
      <label style="margin-left:12px">倍速</label><input type="range" id="spd" min="25" max="300" value="100">
      <span class="st" id="spdt">1.00x</span>
      <button id="mute">静音</button>
    </div>
  </div>

  <div class="card">
    <div class="row">
      <label>原生插件命令</label>
      <select id="cmd">
        <option value="stats">stats</option>
        <option value="snapshot">snapshot（截图）</option>
        <option value="snapshot.gray">snapshot.gray</option>
        <option value="stamp">stamp（标记框开关）</option>
        <option value="effect.cycle">effect.cycle</option>
        <option value="effect.clear">effect.clear</option>
        <option value="hotkey.list">hotkey.list</option>
        <option value="viz.mode">viz.mode</option>
      </select>
      <input type="text" id="arg" placeholder="可选参数（如 bars）" style="min-width:150px">
      <button id="run">执行</button>
    </div>
  </div>

  <div class="card">
    <div class="kv">
      <b>文件</b><span id="k-file">-</span>
      <b>分辨率</b><span id="k-res">-</span>
      <b>编码</b><span id="k-codec">-</span>
      <b>状态</b><span id="k-state">-</span>
    </div>
  </div>
</div>

<script>
var $ = function (s) { return document.querySelector(s) }
var TOKEN_KEY = 'pp-rpc-token'
function getToken() { try { return sessionStorage.getItem(TOKEN_KEY) || '' } catch (e) { return '' } }
function showGate(msg) {
  $('#main').hidden = true
  $('#gate').hidden = false
  $('#tokmsg').textContent = msg || ''
}
function showMain() {
  $('#gate').hidden = true
  $('#main').hidden = false
}
var fmt = function (s) {
  if (!isFinite(s) || s < 0) return '--:--'
  s = Math.floor(s)
  return String(Math.floor(s/60)).padStart(2,'0') + ':' + String(s%60).padStart(2,'0')
}
async function api(action, value) {
  var r = await fetch('/api/action', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-PP-Token': getToken() },
    body: JSON.stringify({ action: action, value: value })
  })
  if (r.status === 401) { showGate('令牌无效'); throw new Error('unauthorized') }
  if (!r.ok) throw new Error('HTTP ' + r.status)
  return r.json()
}
async function tick() {
  try {
    var r = await fetch('/api/state', { headers: { 'X-PP-Token': getToken() } })
    if (r.status === 401) { showGate('令牌无效'); return }
    var s = await r.json()
    var st = s.stats || {}, m = s.info || {}
    var d = st.duration || 0, p = st.position || 0
    $('#prog').style.width = (d > 0 ? (p/d*100) : 0) + '%'
    $('#time').textContent = fmt(p) + ' / ' + fmt(d)
    $('#pos').textContent = 'fps ' + (st.fps||0).toFixed(1) + ' · 丢弃 ' + (st.dropped||0)
    $('#vol').value = Math.round((s.volume ?? 1) * 100)
    $('#volt').textContent = Math.round((s.volume ?? 1) * 100) + '%'
    $('#spd').value = Math.round((st.speed ?? 1) * 100)
    $('#spdt').textContent = (st.speed ?? 1).toFixed(2) + 'x'
    $('#k-file').textContent = (m.path || '-').split(/[\\\\/]/).pop()
    $('#k-res').textContent = (m.width||0) + '×' + (m.height||0) + '  ' + (m.fps||0).toFixed(2) + ' fps'
    $('#k-codec').textContent = (m.video_codec||'-') + ' / ' + (m.audio_codec||'-')
    $('#k-state').textContent = st.paused ? '已暂停' : '播放中'
  } catch (e) { $('#k-state').textContent = '连接失败: ' + e.message }
}
$('#tokgo').onclick = async function () {
  var t = $('#tok').value.trim()
  if (!t) { $('#tokmsg').textContent = '请输入令牌'; return }
  try { sessionStorage.setItem(TOKEN_KEY, t) } catch (e) {}
  try {
    var r = await fetch('/api/state', { headers: { 'X-PP-Token': t } })
    if (r.status === 401) { $('#tokmsg').textContent = '令牌不正确'; return }
    if (!r.ok) { $('#tokmsg').textContent = '服务返回 HTTP ' + r.status; return }
    showMain(); tick()
  } catch (e) { $('#tokmsg').textContent = '连接失败: ' + e.message }
}
$('#toggle').onclick = function () { api('toggle') }
document.querySelectorAll('[data-seek]').forEach(function (b) {
  b.onclick = function () { api('seekBy', Number(b.dataset.seek)) }
})
$('#vol').oninput = function (e) { api('volume', e.target.value/100) }
$('#spd').oninput = function (e) { api('speed', e.target.value/100) }
$('#mute').onclick = function () { api('mute') }
$('#run').onclick = function () {
  var a = $('#arg').value.trim()
  api('command', { name: $('#cmd').value, args: a ? [a] : [] })
}
if (getToken()) { showMain(); setInterval(tick, 500); tick() } else { showGate('') }
</script>
</html>`

/** 安全响应头（所有响应统一带上） */
const SECURITY_HEADERS = {
  'X-Content-Type-Options': 'nosniff',
  'X-Frame-Options': 'DENY',
  'Referrer-Policy': 'no-referrer',
  'Cache-Control': 'no-store',
  'Content-Security-Policy':
    "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; " +
    "connect-src 'self'; img-src 'self' data:; base-uri 'none'; form-action 'none'",
}

function resolveToken(config) {
  if (config.token) return String(config.token).trim()
  const env = (process.env.PP_RPC_TOKEN || '').trim()
  if (env) return env
  const file = config.tokenFile || DEFAULT_TOKEN_FILE
  try {
    const t = fs.readFileSync(file, 'utf8').trim()
    if (t) return t
  } catch {
    /* 落到下面返回空串 */
  }
  return ''
}

function tokenEquals(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string') return false
  const ba = Buffer.from(a)
  const bb = Buffer.from(b)
  if (ba.length !== bb.length) return false
  return crypto.timingSafeEqual(ba, bb)
}

const apply = (ctx, config = {}) => {
  const port = config.port ?? 7912
  const host = config.host ?? '127.0.0.1'
  const enabled = config.enabled === true || process.env.PP_WEB_REMOTE === '1'

  if (!enabled) {
    ctx.logger.info(
      '[web-remote] 默认关闭。本页可执行任意原生插件命令，开启方式：' +
      '环境变量 PP_WEB_REMOTE=1 或 config.enabled=true（见 README 安全说明）')
    return
  }

  const token = resolveToken(config)
  if (!token) {
    ctx.logger.error(
      '[web-remote] 未找到控制通道令牌，拒绝启动。请设置 PP_RPC_TOKEN，' +
      `或确认 ${DEFAULT_TOKEN_FILE} 存在（播放器启动时会生成）。`)
    return
  }

  /* Host / Origin 白名单：只认本机回环 + 本端口。
   * Host 校验是防 DNS rebinding 的关键 —— 攻击者域名解析到 127.0.0.1 后，
   * Host 头仍是攻击者域名，会被这里拒掉。 */
  const allowedHosts = new Set([
    `127.0.0.1:${port}`, `localhost:${port}`, `[::1]:${port}`,
  ])
  const allowedOrigins = new Set([
    `http://127.0.0.1:${port}`, `http://localhost:${port}`, `http://[::1]:${port}`,
  ])

  const readBody = (req) => new Promise((resolve) => {
    let b = ''
    req.on('data', (c) => {
      b += c
      if (b.length > 1e5) { req.destroy(); resolve(null) }
    })
    req.on('end', () => resolve(b))
  })

  const server = http.createServer(async (req, res) => {
    const send = (code, headers, body) => {
      /* 显式给 Content-Length：避免 Node 默认走 chunked，
       * 让裸 socket / 简易 HTTP 客户端也能直接拿到完整正文。 */
      const buf = Buffer.from(body == null ? '' : String(body), 'utf8')
      res.writeHead(code, Object.assign({}, SECURITY_HEADERS,
                                        { 'Content-Length': buf.length }, headers))
      res.end(buf)
    }
    const json = (o, code = 200) => {
      send(code, { 'Content-Type': 'application/json; charset=utf-8' },
           JSON.stringify(o))
    }
    const plain = (code, msg) =>
      send(code, { 'Content-Type': 'text/plain; charset=utf-8' }, msg)

    /* ---- 第 2 道关：Host 白名单（挡 DNS rebinding） ---- */
    const hostHeader = String(req.headers.host || '').toLowerCase()
    if (!allowedHosts.has(hostHeader)) {
      plain(403, 'forbidden host')
      return
    }
    /* ---- 第 3 道关：Origin 白名单（挡 CSRF） ---- */
    const origin = req.headers.origin
    if (origin !== undefined) {
      if (origin === 'null' || !allowedOrigins.has(String(origin).toLowerCase())) {
        plain(403, 'forbidden origin')
        return
      }
    }
    /* 跨域预检一律不通过与 CORS 相关的头，让浏览器自行失败 */
    if (req.method === 'OPTIONS') {
      plain(405, 'preflight not allowed')
      return
    }

    const url = new URL(req.url, `http://${host}:${port}`)

    try {
      /* 静态页本身不需要令牌（不含任何敏感数据，且必须能被浏览器取到） */
      if (req.method === 'GET' && (url.pathname === '/' || url.pathname === '/index.html')) {
        send(200, { 'Content-Type': 'text/html; charset=utf-8' }, PAGE)
        return
      }

      /* ---- 第 4 道关：/api/* 一律要求令牌 ---- */
      if (url.pathname.startsWith('/api/')) {
        const got = req.headers['x-pp-token']
        if (!tokenEquals(got, token)) {
          json({ ok: false, error: 'unauthorized' }, 401)
          return
        }
      }

      if (req.method === 'GET' && url.pathname === '/api/state') {
        const [info, stats, vol] = await Promise.all([
          ctx.player.info(),
          ctx.player.stats(),
          ctx.player.call('player.volume', {}),
        ])
        json({ info, stats, volume: vol.volume })
        return
      }

      if (req.method === 'POST' && url.pathname === '/api/action') {
        /* ---- 第 5 道关：强制 JSON（挡简单请求型 CSRF） ---- */
        const ctype = String(req.headers['content-type'] || '').toLowerCase()
        if (!ctype.startsWith('application/json')) {
          json({ ok: false, error: 'content-type must be application/json' }, 415)
          return
        }
        const raw = await readBody(req)
        if (raw === null) { json({ ok: false, error: 'body too large' }, 413); return }
        let body
        try {
          body = JSON.parse(raw || '{}')
        } catch {
          json({ ok: false, error: 'invalid json' }, 400)
          return
        }
        if (body === null || typeof body !== 'object' || Array.isArray(body)) {
          json({ ok: false, error: 'invalid payload' }, 400)
          return
        }
        const action = body.action
        const value = body.value
        if (typeof action !== 'string' || action.length > 32) {
          json({ ok: false, error: 'invalid action' }, 400)
          return
        }
        let result = null
        switch (action) {
          case 'toggle':  result = await ctx.player.toggle(); break
          case 'play':    result = await ctx.player.play(); break
          case 'pause':   result = await ctx.player.pause(); break
          case 'seek':    result = await ctx.player.seek(Number(value)); break
          case 'seekBy':  result = await ctx.player.seekBy(Number(value)); break
          case 'volume':  result = await ctx.player.volume(Number(value)); break
          case 'mute':    result = await ctx.player.mute(); break
          case 'speed':   result = await ctx.player.speed(Number(value)); break
          case 'command':
            if (!value || typeof value.name !== 'string') {
              json({ ok: false, error: 'command.name required' }, 400)
              return
            }
            result = await ctx.player.command(value.name,
                                              Array.isArray(value.args) ? value.args : [])
            break
          default:
            /* 不回显输入，避免把不可信内容原样送回去 */
            json({ ok: false, error: 'unknown action' }, 400)
            return
        }
        ctx.logger.info(`[web-remote] ${action} -> ${JSON.stringify(result)}`)
        json({ ok: true, result })
        return
      }

      json({ ok: false, error: 'not found' }, 404)
    } catch (e) {
      /* 只回固定文案，不回显底层错误信息 */
      ctx.logger.warn(`[web-remote] 请求处理失败: ${e && e.message}`)
      json({ ok: false, error: 'internal error' }, 500)
    }
  })

  server.on('error', (e) => ctx.logger.warn(`[web-remote] server 错误: ${e.message}`))
  server.listen(port, host, () => {
    ctx.logger.info(`[web-remote] 控制页已启动 http://${host}:${port}/（需控制通道令牌）`)
  })

  ctx.effect(() => () => {
    server.close()
    ctx.logger.info('[web-remote] 控制页已关闭')
  })
}

export default { name: 'web-remote', inject: ['player'], apply }
export const config = { host: '127.0.0.1', port: 7912, enabled: false }
