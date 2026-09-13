"""推送辅助：把已 staged 的改动提交并推到 gitee main。

用法：
    git add <files...>
    python tools/push_gitee.py "提交信息"

行为：
- 只提交当前已 `git add` 的内容，绝不 force-push，绝不自动 add 未跟踪文件。
- 从 ~/.git-credentials 读取 gitee 令牌（输出一律打码，不打印明文）。
- 若本地没有可推送的凭据，则只完成提交并提示跳过 push。

依赖：Python 3（标准库即可）。
"""
import subprocess, os, re, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(args, **kw):
    return subprocess.run(["git", "-C", REPO] + args, capture_output=True, text=True, **kw)


def main():
    if len(sys.argv) < 2:
        print("usage: python tools/push_gitee.py \"commit message\"")
        return 2

    msg = sys.argv[1]

    # 必须有已 staged 的改动
    st = run(["diff", "--cached", "--quiet"])
    if st.returncode == 0:
        print("NOTHING_STAGED: 先 `git add` 要提交的文件，再运行本脚本")
        return 1

    r = run(["commit", "-m", msg])
    print("COMMIT rc", r.returncode)
    print((r.stdout + r.stderr).strip()[:600])
    if r.returncode != 0:
        return r.returncode

    # 读取 gitee 令牌（仅用于本次推送，绝不打印明文）
    cred = os.path.expanduser("~/.git-credentials")
    token = user = None
    if os.path.isfile(cred):
        txt = open(cred, encoding="utf-8", errors="replace").read()
        m = re.search(r"https://([^:/]+):([^@\s]+)@gitee\.com", txt)
        if m:
            user, token = m.group(1), m.group(2)

    if not token:
        print("NO_TOKEN: 未找到 gitee 凭据，跳过 push（提交已创建）")
        return 0

    url = "https://%s:%s@gitee.com/siwpan/pluginplayer.git" % (user, token)
    masked = url.replace(token, "***")
    print("PUSH url", masked)
    r = run(["-c", "credential.helper=", "push", url, "main"], timeout=120)
    print("PUSH rc", r.returncode)
    print((r.stdout + r.stderr).replace(token, "***")[:800])
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
