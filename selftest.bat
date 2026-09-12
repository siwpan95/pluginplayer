@echo off
rem 一键自检：构建 -> 生成测试视频 -> 跑通全链路 -> 输出 JSON 报告
setlocal
cd /d %~dp0
if not exist vendor\ffmpeg\bin\ffmpeg.exe (
  echo [ERR] 缺少 vendor\ffmpeg\bin\ffmpeg.exe，请先执行 tools\fetch_deps.py
  pause & exit /b 1
)
F:\Python314\python.exe build.py || pause
cd dist
if not exist test_assets mkdir test_assets
if not exist test_assets\sample.mp4 ..\vendor\ffmpeg\bin\ffmpeg.exe -y -hide_banner -loglevel error -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -shortest test_assets\sample.mp4
pluginplayer.exe --selftest --frames 90 --out selftest_out test_assets\sample.mp4
echo.
echo 报告: dist\selftest_out\selftest_report.json
pause
