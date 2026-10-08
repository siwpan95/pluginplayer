@echo off
rem One-click self test: build -> smoke -> make sample -> full chain -> JSON report
setlocal
cd /d %~dp0
if not exist vendor\ffmpeg\bin\ffmpeg.exe (
  echo [ERR] vendor\ffmpeg\bin\ffmpeg.exe is missing. Run tools\fetch_deps.py first.
  pause & exit /b 1
)
python build.py || pause
echo [1/3] smoke tests (tests/smoke_link.cpp + tests/smoke_audio.cpp)
python build.py --smoke
if errorlevel 1 echo [WARN] smoke test had failures, see output above
cd dist
if not exist test_assets mkdir test_assets
if not exist test_assets\sample.mp4 ..\vendor\ffmpeg\bin\ffmpeg.exe -y -hide_banner -loglevel error -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -shortest test_assets\sample.mp4
pluginplayer.exe --selftest --frames 90 --out selftest_out test_assets\sample.mp4
echo.
echo report: dist\selftest_out\selftest_report.json
pause
