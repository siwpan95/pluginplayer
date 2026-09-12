/**
 * player-service.mjs — 把原生播放器的控制通道包装成 Cordis 服务 `ctx.player`
 *
 * 这是「进程外插件」的接入点：原生内核 playerplayer.exe 通过
 * 127.0.0.1 上的换行分隔 JSON-RPC 暴露播放能力，本服务把它变成
 * Cordis 上下文里的一个普通服务，于是 JS 插件就能像用本地对象一样用它。
 *
 * 与 C 版插件 SDK (src/sdk/player_plugin.h) 的概念对应关系：
 *   HostApi.get_stats()      <->  player.stats()
 *   HostApi.invoke_command() <->  player.command('effect.set', ['gray'])
 *   HostApi.subscribe()      <->  ctx.on('player/<topic>', fn)
 *   on_unload + revokeAll    <->  ctx.effect(() => disposeFn)
 */
import { Service } from 'cordis'
import net from 'node:net'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = path.dirname(fileURLToPath(import.meta.url))
const HOST_JS_ROOT = path.resolve(__dirname, '..')
/** 播放器启动时会把控制通道令牌写到这里（见 src/core/main.cpp） */
const DEFAULT_TOKEN_FILE = path.resolve(HOST_JS_ROOT, '..', 'dist', 'config', 'rpc-token.txt')

/**
 * 解析控制通道令牌。顺序：
 *   1) opts.token / --token
 *   2) 环境变量 PP_RPC_TOKEN
 *   3) opts.tokenFile / --token-file
 *   4) 默认文件 <repo>/dist/config/rpc-token.txt
 */
export function resolveToken(opts = {}) {
  if (opts.token) return String(opts.token).trim()
  const env = (process.env.PP_RPC_TOKEN || '').trim()
  if (env) return env
  const file = opts.tokenFile || DEFAULT_TOKEN_FILE
  try {
    const t = fs.readFileSync(file, 'utf8').trim()
    if (t) return t
  } catch {
    /* 落到下面的报错 */
  }
  throw new Error(
    `找不到控制通道令牌：${file}\n` +
    `      请用 --token <hex> 指定，或设置 PP_RPC_TOKEN，或先启动播放器（会自动生成该文件）。`)
}

export class PlayerService extends Service {
  constructor(ctx) {
    super(ctx, 'player')
    this.sock = null
    this.buf = ''
    this.nextId = 1
    this.pending = new Map()
    this.connected = false
    /** 令牌认证是否已完成。控制通道要求认证后才能调用任何方法。 */
    this.authed = false
    this.reconnects = 0
    this.counter = { requests: 0, events: 0, errors: 0 }
    this.opts = { host: '127.0.0.1', port: 7911, retryMs: 2000, token: '', tokenFile: '' }
    this._stopped = false
    this._tokenWarned = false
  }

  /** 建立（并自动重连）到原生内核的控制通道 */
  start(opts = {}) {
    Object.assign(this.opts, opts)
    this.ctx.effect(() => () => this.stop())
    this._connect()
    return this
  }

  stop() {
    this._stopped = true
    if (this.sock) {
      this.sock.removeAllListeners()
      this.sock.destroy()
      this.sock = null
    }
    this.connected = false
    this.authed = false
  }

  _connect() {
    if (this._stopped) return
    const { host, port, retryMs } = this.opts
    let token
    try {
      token = resolveToken(this.opts)
    } catch (e) {
      /* 播放器可能还没启动（令牌文件尚未生成）。只报一次错，之后静默重试，
       * 否则「先起 host-js 再起播放器」这个很自然的顺序会永久连不上。 */
      this.counter.errors++
      if (!this._tokenWarned) {
        this._tokenWarned = true
        this.ctx.emit('player/error', { message: String(e.message || e) })
        this.ctx.logger?.warn?.(`[player] ${e.message}`)
      }
      if (!this._stopped) setTimeout(() => this._connect(), retryMs)
      return
    }
    this._tokenWarned = false
    const sock = net.createConnection({ host, port })
    this.sock = sock
    sock.setEncoding('utf8')
    sock.on('connect', () => {
      // 控制通道强制令牌认证：必须是连接后的第一条消息
      this.call('auth', { token })
        .then(() => {
          this.authed = true
          this.connected = true
          this.ctx.logger?.info?.(`已连接并认证原生内核 ${host}:${port}`)
          this.ctx.emit('player/connected', { host, port })
          // 默认订阅全部事件，由本服务按主题再分发给 Cordis 监听者
          this.call('topic.subscribe', { topic: '*' }).catch(() => {})
        })
        .catch((err) => {
          this.counter.errors++
          this.authed = false
          this.connected = false
          this.ctx.emit('player/error', { message: `控制通道认证失败: ${err.message}` })
          this.ctx.logger?.error?.(`[player] 控制通道认证失败: ${err.message}`)
          sock.destroy()
        })
    })
    sock.on('data', (chunk) => this._onData(chunk))
    sock.on('error', (err) => {
      this.counter.errors++
      this.ctx.emit('player/error', { message: String(err.message || err) })
    })
    sock.on('close', () => {
      const was = this.connected
      this.connected = false
      this.authed = false
      if (was) this.ctx.emit('player/disconnected', {})
      if (!this._stopped) {
        this.reconnects++
        setTimeout(() => this._connect(), retryMs)
      }
    })
  }

  _onData(chunk) {
    this.buf += chunk
    let i
    while ((i = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, i).trim()
      this.buf = this.buf.slice(i + 1)
      if (!line) continue
      let msg
      try {
        msg = JSON.parse(line)
      } catch {
        this.counter.errors++
        continue
      }
      if (msg.event) this._dispatchEvent(msg.event, msg.data)
      else if (msg.id && this.pending.has(msg.id)) {
        const { resolve, reject } = this.pending.get(msg.id)
        this.pending.delete(msg.id)
        if (msg.ok) resolve(msg.result)
        else reject(new Error(msg.error || '未知错误'))
      }
    }
  }

  _dispatchEvent(topic, data) {
    this.counter.events++
    // 两种订阅姿势：通配（player/event）与按主题（player/<topic>）
    this.ctx.emit('player/event', { topic, data })
    this.ctx.emit(`player/${topic}`, data)
  }

  /** 发起一次控制通道调用 */
  call(method, params = {}) {
    return new Promise((resolve, reject) => {
      // 注意：认证握手本身也走 call()，所以这里只要求 TCP 已建立，
      // 不能用 this.authed 作为门槛（否则 auth 永远发不出去）。
      if (!this.sock || this.sock.destroyed) {
        reject(new Error('未连接到原生内核（控制通道未启动？用 --rpc-port 7911 启动播放器）'))
        return
      }
      const id = this.nextId++
      this.counter.requests++
      this.pending.set(id, { resolve, reject })
      this.sock.write(JSON.stringify({ id, method, params }) + '\n')
      setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id)
          reject(new Error(`请求超时: ${method}`))
        }
      }, 10000)
    })
  }

  /* ---- 常用能力（与 HostApi 一一对应，取个短名字方便插件调用） ---- */
  info()                 { return this.call('player.info') }
  stats()                { return this.call('player.stats') }
  play()                 { return this.call('player.play') }
  pause()                { return this.call('player.pause') }
  toggle()               { return this.call('player.toggle') }
  seek(seconds)          { return this.call('player.seek', { seconds }) }
  seekBy(delta)          { return this.call('player.seek', { delta }) }
  volume(value)          { return this.call('player.volume', { value }) }
  mute(value)            { return this.call('player.mute', { value }) }
  speed(value)           { return this.call('player.speed', { value }) }
  open(path)             { return this.call('player.open', { path }) }
  command(name, args = []) { return this.call('command.invoke', { name, args }) }
  services()             { return this.call('service.list').then((r) => r.services) }
  plugins()              { return this.call('plugin.list').then((r) => r.plugins) }

  /** 简易文本时间轴，方便插件做日志 */
  static fmtTime(sec) {
    if (!isFinite(sec) || sec < 0) return '--:--'
    const s = Math.floor(sec)
    return `${String(Math.floor(s / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`
  }
}

export default PlayerService
