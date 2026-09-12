@echo off
rem PluginPlayer 启动器：把媒体文件拖到本文件上即可播放
setlocal
cd /d %~dp0dist
pluginplayer.exe %*
