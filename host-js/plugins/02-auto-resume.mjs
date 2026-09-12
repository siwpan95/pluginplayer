/**
 * 02-auto-resume.mjs — 断点续播插件（JS 侧，进程外）
 *
 * 这是「服务型插件」的范例：它既 **消费** 服务（inject: player），
 * 又 **提供** 服务（ctx.resume），与 C 版插件里
 * register_service / get_service 的用法一一对应。
 *
 * 演示能力：
 *   - Cordis Service 子类：构造函数里 super(ctx, 'resume') 即注册 ctx.resume
 *   - 状态持久化（JSON 文件），重启宿主后仍记得每个文件看到哪儿
 *   - 双保险恢复：media.opened 事件 + 连接后主动与内核「对账」
 *     （JS 宿主常常比播放器晚启动，纯事件驱动会错过开场事件）
 *   - 调用原生 C 插件的命令（ctx.player.command）实现跨语言协作
 */
import { Service } from 'cordis'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = path.dirname(fileURLToPath(import.meta.url))
const STATE_DIR = path.resolve(__dirname, '..', '.state')
const DB = path.join(STATE_DIR, 'positions.json')

const load = () => { try { return JSON.parse(fs.readFileSync(DB, 'utf8')) } catch { return {} } }
const save = (db) => {
  fs.mkdirSync(STATE_DIR, { recursive: true })
  fs.writeFileSync(DB, JSON.stringify(db, null, 2), 'utf8')
}

export default class ResumeService extends Service {
  static inject = ['player']
  static name = 'auto-resume'

  constructor(ctx, config = {}) {
    super(ctx, 'resume')
    ctx.logger.info('[auto-resume] 已装载（提供 ctx.resume 服务）')

    const minResume = config.minResumeSeconds ?? 5
    const saveIntervalMs = config.saveIntervalMs ?? 2000
    const db = load()
    this.db = db
    let current = null
    let lastSaved = 0

    /** 恢复指定媒体的进度 */
    this.restore = async (p, duration) => {
      current = p || null
      if (!current) return false
      const rec = db[current]
      if (!rec || !rec.position || rec.position < minResume) return false
      if (duration > 0 && rec.position > duration - 3) return false
      try {
        await ctx.player.seek(rec.position)
        ctx.logger.info(`[auto-resume] 从 ${rec.position.toFixed(1)}s 续播: ${path.basename(current)}`)
        /* 顺手调用原生插件命令，把续播状态打到窗口标题（JS -> C 插件） */
        await ctx.player.command('stamp', []).catch(() => {})
        await ctx.player.command('stamp', []).catch(() => {})
        return true
      } catch (e) {
        ctx.logger.warn(`[auto-resume] 续播失败: ${e.message}`)
        return false
      }
    }

    ctx.on('player/media.opened', (d) => this.restore(d?.path, Number(d?.duration || 0)))

    /* 与内核「对账」：处理宿主晚启动、错过事件的情况 */
    ctx.on('player/connected', async () => {
      try {
        const info = await ctx.player.info()
        if (info?.path) {
          ctx.logger.info('[auto-resume] 发现已打开中的媒体，执行一次状态对账')
          await this.restore(info.path, Number(info.duration || 0))
        }
      } catch { /* 忽略 */ }
    })

    const timer = setInterval(async () => {
      if (!current) return
      const now = Date.now()
      if (now - lastSaved < saveIntervalMs) return
      lastSaved = now
      try {
        const st = await ctx.player.stats()
        db[current] = { position: st.position, duration: st.duration, at: new Date().toISOString() }
        save(db)
      } catch { /* 忽略 */ }
    }, 1000)

    ctx.effect(() => () => {
      clearInterval(timer)
      save(db)
      ctx.logger.info(`[auto-resume] 已保存进度记忆（${Object.keys(db).length} 条）`)
    })

    ctx.on('player/completed', () => {
      if (!current || !db[current]) return
      ctx.logger.info(`[auto-resume] 已看完，清除记录: ${path.basename(current)}`)
      delete db[current]
      save(db)
    })
  }

  list() { return { ...this.db } }
  forget(p) { delete this.db[p]; save(this.db) }
  clear() { for (const k of Object.keys(this.db)) delete this.db[k]; save(this.db) }
}

export const config = { minResumeSeconds: 5, saveIntervalMs: 2000 }
