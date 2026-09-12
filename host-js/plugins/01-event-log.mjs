/**
 * 01-event-log.mjs — 事件日志插件（JS 侧，进程外）
 *
 * 演示能力：
 *   - ctx.on('player/event') 通配订阅原生内核推来的所有事件
 *   - 按主题分别订阅（player/media.opened 等）
 *   - ctx.effect() 注册「可撤销副作用」：卸载时自动落盘并关闭定时器
 *   - 通过 ctx.player 服务查询状态（等价于 C 插件的 HostApi.get_stats）
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = path.dirname(fileURLToPath(import.meta.url))
const STATE_DIR = path.resolve(__dirname, '..', '.state')

const apply = (ctx, config = {}) => {
  const verbose = config.verbose !== false
  const counts = new Map()
  const timeline = []
  const startedAt = Date.now()

  /* 通配订阅：拿到全部事件 */
  ctx.on('player/event', ({ topic, data }) => {
    counts.set(topic, (counts.get(topic) || 0) + 1)

    if (topic === 'media.opened') {
      timeline.push({ t: Date.now() - startedAt, event: topic, path: data?.path })
      if (verbose) ctx.logger.info(`[evt] 打开媒体 ${data?.path}`)
    } else if (topic === 'player.seeked') {
      timeline.push({ t: Date.now() - startedAt, event: topic })
    } else if (topic === 'player.completed') {
      if (verbose) ctx.logger.info('[evt] 播放结束')
    } else if (topic === 'app.tick' && verbose) {
      const d = data || {}
      ctx.logger.debug(
        `[evt] tick pos=${Number(d.position ?? 0).toFixed(2)}s fps=${Number(d.fps ?? 0).toFixed(1)} ` +
        `decoded=${d.decoded} dropped=${d.dropped}`
      )
    } else if (verbose) {
      ctx.logger.debug(`[evt] ${topic} ${JSON.stringify(data ?? {})}`)
    }
  })

  /* 单独订阅几个关心的主题（更精确的写法） */
  ctx.on('player/paused', () => ctx.logger.info('[evt] 已暂停'))
  ctx.on('player/resumed', () => ctx.logger.info('[evt] 继续播放'))
  ctx.on('player/disconnected', () => ctx.logger.warn('[evt] 与原生内核断开'))
  ctx.on('player/connected', () => ctx.logger.info('[evt] 已连接原生内核'))

  /* 可撤销副作用：卸载时把统计落盘 + 打印摘要 */
  ctx.effect(() => {
    return () => {
      fs.mkdirSync(STATE_DIR, { recursive: true })
      const summary = {
        sessionMs: Date.now() - startedAt,
        requests: ctx.player.counter.requests,
        events: ctx.player.counter.events,
        reconnects: ctx.player.reconnects,
        byTopic: Object.fromEntries([...counts.entries()].sort((a, b) => b[1] - a[1])),
        timeline,
      }
      const out = path.join(STATE_DIR, 'js-event-log.json')
      fs.writeFileSync(out, JSON.stringify(summary, null, 2), 'utf8')
      ctx.logger.info(`[evt] 事件统计已写入 ${out}（共 ${ctx.player.counter.events} 条）`)
    }
  })

  /* 周期性打印一行摘要，方便观察 */
  const timer = setInterval(async () => {
    try {
      const st = await ctx.player.stats()
      ctx.logger.info(
        `[evt] 摘要 位置 ${Number(st.position).toFixed(1)}s/${Number(st.duration).toFixed(1)}s  ` +
        `暂停=${st.paused}  事件累计=${ctx.player.counter.events}`
      )
    } catch { /* 未连接时忽略 */ }
  }, 10000)

  ctx.effect(() => () => clearInterval(timer))

  ctx.logger.info(`[event-log] 已装载（详细日志=${verbose}）`)
}

export default { name: 'event-log', inject: ['player'], apply }
export const config = { verbose: true }
