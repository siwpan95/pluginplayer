#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""color_check.py — 色彩还原回归：YUV→RGB 的矩阵与电平范围有没有按标注来

为什么需要独立一层检查：色彩错了**不会崩、不会报错、不会掉帧**，只会让画面偏色
或发灰。内核自检、CLI 回归、单元测试都看不见这类缺陷 —— 只有真解一帧、读像素才看得见。
（`tests/unit/test_color.cpp` 钉的是"选哪套矩阵"的纯逻辑；这一层钉的是"选完之后
真的把系数喂给了 swscale"。两者缺一不可。）

做法：用随包的 ffmpeg 现造参考素材，跑 `--selftest` 拿到解码帧 PNG，
**在脚本里独立算出期望 RGB** 再逐像素比对。

关键一招是"纯元数据 A/B"：两个素材的码流**逐字节相同**、只有容器标注不同
（用 `-c copy` 加标注不会动像素）。如果播放器忽略了标注，两者输出必然相同 ——
所以这项检查不只验"值对不对"，还验"标注确实被读进去了"。

用法:
    F:\\Python314\\python.exe tests\\color_check.py [--keep]
退出码：0 = 全通过（或全部 SKIP），1 = 有失败。跳过**不计入**通过数。
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 与 src/core/app.h 的窗口默认尺寸无关，这里是素材尺寸
REF_W, REF_H = 320, 240

# 允许的偏差：libswscale 的系数表是定点近似，与教科书的浮点系数差 1~3 级
TOL = 4


# ============================================================================
# 期望值：自己算一遍，不引用被测程序的任何输出
# ============================================================================

def yuv_to_rgb(y, cb, cr, matrix, full_range):
    """按 ITU-R 系数把 YUV 还原成 RGB（0..255，含钳位）。

    先做电平范围展开（有限 16-235 / 224 → 全范围），再套矩阵。
    展开式与"有限范围专用系数"是等价的：1.1644 = 255/219，1.402*255/224 = 1.596。
    """
    if not full_range:
        y = (y - 16.0) * 255.0 / 219.0
        cb = (cb - 128.0) * 255.0 / 224.0
        cr = (cr - 128.0) * 255.0 / 224.0
    else:
        cb = float(cb) - 128.0
        cr = float(cr) - 128.0
    if matrix == "bt709":
        r = y + 1.5748 * cr
        g = y - 0.1873 * cb - 0.4681 * cr
        b = y + 1.8556 * cb
    else:  # bt601
        r = y + 1.4020 * cr
        g = y - 0.3441 * cb - 0.7141 * cr
        b = y + 1.7720 * cb
    return tuple(max(0, min(255, int(round(v)))) for v in (r, g, b))


# ============================================================================
# PNG 读取（只解析本项目自写 PNG 会产出的形态：8bit RGBA、filter 0）
# ============================================================================

def read_png_rgba(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, w, h, idat = 8, 0, 0, bytearray()
    while pos < len(data):
        ln = struct.unpack_from(">I", data, pos)[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if tag == b"IHDR":
            w, h, depth, ctype, _, _, il = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype != 6 or il != 0:
                raise ValueError(f"不支持的 PNG 形态 depth={depth} ctype={ctype}")
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
    raw = zlib.decompress(bytes(idat))
    stride = w * 4
    rows, p, prev = [], 0, bytearray(stride)
    for _ in range(h):
        ft = raw[p]; p += 1
        cur = bytearray(raw[p:p + stride]); p += stride
        if ft == 1:
            for i in range(4, stride):
                cur[i] = (cur[i] + cur[i - 4]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = cur[i - 4] if i >= 4 else 0
                cur[i] = (cur[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = cur[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 0xFF
        elif ft != 0:
            raise ValueError(f"未知 filter {ft}")
        rows.append(bytes(cur))
        prev = cur
    return w, h, rows


def pixel(rows, x, y):
    r = rows[y]
    i = x * 4
    return (r[i], r[i + 1], r[i + 2])


# ============================================================================
# 运行环境
# ============================================================================

def ffmpeg_path():
    for name in ("ffmpeg.exe", "ffmpeg"):
        p = os.path.join(ROOT, "vendor", "ffmpeg", "bin", name)
        if os.path.exists(p):
            return p
    return None


def player_path():
    p = os.path.join(ROOT, "dist", "pluginplayer.exe")
    return p if os.path.exists(p) else None


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=180)
    return r.returncode, r.stdout.decode("utf-8", "replace")


# ============================================================================
# 参考素材
# ============================================================================

def make_assets(ff, work):
    """返回 [(名字, 素材路径, 左右两侧期望 RGB, 说明)]。"""
    os.makedirs(work, exist_ok=True)
    cases = []

    def geq(expr):
        return f"geq=lum='{expr}':cb=128:cr=128"

    # 1) 有限范围的中性黑/白（未标注）：16→0、235→255 才算对，
    #    若按全范围直读会变成 16/235 —— 这就是"发灰"。
    p = os.path.join(work, "neutral_limited.mp4")
    rc, out = run([ff, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                   "-i", f"nullsrc=s={REF_W}x{REF_H}:r=5:d=1",
                   "-vf", geq("if(lt(X,%d),16,235)" % (REF_W // 2)),
                   "-pix_fmt", "yuv420p", "-c:v", "libx264", "-crf", "0",
                   "-preset", "ultrafast", p])
    if rc != 0:
        raise RuntimeError(f"ffmpeg 生成失败: {out}")
    cases.append(("中性 有限范围→RGB", p,
                  yuv_to_rgb(16, 128, 128, "bt601", False),
                  yuv_to_rgb(235, 128, 128, "bt601", False),
                  "黑必须落到 0、白必须落到 255（按全范围直读会得到 16/235 = 发灰）"))

    # 2) 全范围的中性黑/白（容器标注 pc）：同样应落到 0/255
    p = os.path.join(work, "neutral_full.mp4")
    rc, out = run([ff, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                   "-i", f"nullsrc=s={REF_W}x{REF_H}:r=5:d=1",
                   "-vf", geq("if(lt(X,%d),0,255)" % (REF_W // 2)),
                   "-pix_fmt", "yuv420p", "-color_range", "pc",
                   "-c:v", "libx264", "-crf", "0", "-preset", "ultrafast", p])
    if rc != 0:
        raise RuntimeError(f"ffmpeg 生成失败: {out}")
    cases.append(("中性 全范围→RGB", p,
                  yuv_to_rgb(0, 128, 128, "bt601", True),
                  yuv_to_rgb(255, 128, 128, "bt601", True),
                  "全范围素材不能被当成有限范围去展开"))

    # 3/4) 纯元数据 A/B：同一段码流（逐字节相同）＋不同的容器标注。
    #      用 geq 写死一个"在色域内但很饱和"的 YUV，601 与 709 还原出的 RGB 差得够远
    #      （实测 R 差 16、G 差 18），足以判定播放器用了哪套矩阵。
    base = os.path.join(work, "sat_plain.mp4")
    rc, out = run([ff, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                   "-i", f"nullsrc=s={REF_W}x{REF_H}:r=5:d=1",
                   "-vf", "geq=lum=76:cb=109:cr=211",
                   "-pix_fmt", "yuv420p", "-c:v", "libx264", "-crf", "0",
                   "-preset", "ultrafast", base])
    if rc != 0:
        raise RuntimeError(f"ffmpeg 生成失败: {out}")
    tagged = os.path.join(work, "sat_colr709.mp4")
    # -c copy：只改容器标注，不动一个像素
    rc, out = run([ff, "-hide_banner", "-loglevel", "error", "-y", "-i", base,
                   "-c", "copy", "-colorspace", "bt709",
                   "-color_primaries", "bt709", "-color_trc", "bt709", tagged])
    if rc != 0:
        raise RuntimeError(f"ffmpeg 生成失败: {out}")

    e601 = yuv_to_rgb(76, 109, 211, "bt601", False)
    e709 = yuv_to_rgb(76, 109, 211, "bt709", False)
    cases.append(("饱和色 未标注→按分辨率猜 BT.601", base, e601, e601,
                  "SD 尺寸未标注：按 FFmpeg 的 auto 规则用 BT.601"))
    cases.append(("饱和色 容器标 BT.709→必须用 BT.709", tagged, e709, e709,
                  "与上一条码流相同、只有标注不同；两者输出必须不同"))
    return cases, (base, tagged, e601, e709)


# ============================================================================
# 主流程
# ============================================================================

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="保留中间产物（默认保留，便于复核）")
    args = ap.parse_args()

    ff, player = ffmpeg_path(), player_path()
    missing = []
    if not ff:
        missing.append("vendor/ffmpeg/bin/ffmpeg.exe")
    if not player:
        missing.append("dist/pluginplayer.exe")
    if missing:
        for name in ("中性 有限范围→RGB", "中性 全范围→RGB",
                     "饱和色 未标注→按分辨率猜 BT.601",
                     "饱和色 容器标 BT.709→必须用 BT.709"):
            print(f"[SKIP] {name}  —— 缺少 {'、'.join(missing)}（先跑 fetch_deps.py + build.py）")
        print("\n==== 色彩还原: 0/0 项通过，跳过 4 项，失败 0 项（跳过 ≠ 通过）====")
        return 0

    work = os.path.join(ROOT, "build", "_color_check")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work, exist_ok=True)
    try:
        cases, meta = make_assets(ff, work)
    except Exception as e:                     # noqa: BLE001
        print(f"[FAIL] 参考素材生成失败: {e}")
        print("\n==== 色彩还原: 0/1 项通过，跳过 0 项，失败 1 项 ====")
        return 1

    passed = failed = 0
    measured = {}
    for name, media, exp_l, exp_r, why in cases:
        outdir = os.path.join(work, "out_" + os.path.basename(media))
        # 产物写在 build/ 下（**刻意不写 dist/**）：dist/ 有"发布载荷 17 个文件"的口径，
        # 往里丢中间产物会直接把 release-payload 门禁顶翻。--write-dir 是给插件用的
        # 写入白名单（默认只允许 exeDir），输出目录在 dist/ 之外时必须显式追加。
        rc, out = run([player, "--selftest", "--frames", "3", "--out", outdir,
                       "--write-dir", work, media],
                      cwd=os.path.join(ROOT, "dist"))
        png = os.path.join(outdir, "frame_001.png")
        if not os.path.exists(png):
            print(f"[FAIL] {name}  —— selftest 没产出帧图（rc={rc}）\n        {out.strip()[-300:]}")
            failed += 1
            continue
        try:
            w, h, rows = read_png_rgba(png)
        except Exception as e:                 # noqa: BLE001
            print(f"[FAIL] {name}  —— 读帧图失败: {e}")
            failed += 1
            continue

        got_l = pixel(rows, w // 4, h // 2)          # 左半区中心
        got_r = pixel(rows, w * 3 // 4, h // 2)      # 右半区中心
        measured[os.path.basename(media)] = (got_l, got_r)

        def close(a, b):
            return all(abs(x - y) <= TOL for x, y in zip(a, b))

        ok = close(got_l, exp_l) and close(got_r, exp_r)
        if ok:
            passed += 1
            print(f"[PASS] {name}")
            print(f"        左 期望 {exp_l} 实测 {got_l} ／ 右 期望 {exp_r} 实测 {got_r}")
        else:
            failed += 1
            print(f"[FAIL] {name}")
            print(f"        左 期望 {exp_l} 实测 {got_l} ／ 右 期望 {exp_r} 实测 {got_r}（容差 ±{TOL}）")
            print(f"        说明: {why}")

    # 结构性断言：码流相同、标注不同的两个素材，输出必须不同。
    # 播放器若忽略标注，这两条会给出完全一样的结果 —— 那是比"数值差几级"严重得多的问题。
    base, tagged, e601, e709 = meta
    a = measured.get(os.path.basename(base))
    b = measured.get(os.path.basename(tagged))
    if a and b:
        diff = max(abs(x - y) for x, y in zip(a[0], b[0]))
        if diff >= 8:
            passed += 1
            print(f"[PASS] 纯元数据 A/B：同码流不同标注 → 输出确有差异（最大通道差 {diff}）")
        else:
            failed += 1
            print(f"[FAIL] 纯元数据 A/B：同码流不同标注却给出几乎一样的结果（最大通道差 {diff}）")
            print(f"        期望 BT.601 {e601} vs BT.709 {e709} —— 说明帧上的色彩标注没被采纳")

    total = passed + failed
    print(f"\n==== 色彩还原: {passed}/{total} 项通过，跳过 0 项，失败 {failed} 项 ====")
    print(f"（参考素材与帧图保留在 {work}）")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
