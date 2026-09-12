@echo off
rem PluginPlayer launcher: drag a media file onto this file to play it.
setlocal
cd /d %~dp0dist
pluginplayer.exe %*
