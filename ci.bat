@echo off
rem PluginPlayer one-click CI: gates + python/cpp unit tests + smoke + selftest + CLI.
rem Windows-only stages need Windows; on Linux use: python3 tools/ci.py --profile ci
rem Stages whose prerequisites are missing are SKIPPED with a reason (skip != pass).
setlocal
cd /d %~dp0
set "PP_PY=F:\Python314\python.exe"
if not exist "%PP_PY%" set "PP_PY="
rem Do NOT use `if not defined X where py && set ...`: in cmd the && ends
rem the if scope (if guards only the first command), so set runs always.
if not defined PP_PY (
  where py >nul 2>nul
  if not errorlevel 1 set "PP_PY=py -3"
)
if not defined PP_PY (
  where python >nul 2>nul
  if not errorlevel 1 set "PP_PY=python"
)
if not defined PP_PY (
  echo [ERR] Python not found. Set PP_PYTHON to a python.exe path. 1>&2
  pause & exit /b 6
)
if not exist vendor\ffmpeg\lib (
  echo [WARN] vendor\ is not ready: Windows stages will be SKIPPED.
  echo        Prepare it with: %PP_PY% tools\fetch_deps.py
)
%PP_PY% tools\ci.py %*
set "PP_RC=%ERRORLEVEL%"
if not "%PP_RC%"=="0" echo [FAIL] CI had failures, see output above.
exit /b %PP_RC%
