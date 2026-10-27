@echo off
rem Builds the Enhanced Tuning plugin (EnhancedTuning.dll) with Zig 0.15.2 (the same compiler Extra Options uses).
rem Usage: build.cmd [C:\path\to\zig.exe]     (default: zig on PATH)
rem Output: release\Enhanced Tuning\plugins\EnhancedTuning.dll
setlocal
set ZIG=%~1
if "%ZIG%"=="" set ZIG=zig
set "OUT=%~dp0release\Enhanced Tuning\plugins"
if not exist "%OUT%" mkdir "%OUT%"
set "SRC=%~dp0src"
"%ZIG%" cc -shared -O2 -target x86_64-windows-gnu -Wall -Werror ^
  -o "%OUT%\EnhancedTuning.dll" "%SRC%\livetune.c" "%SRC%\tuning_tab.c" "%SRC%\tuning_page.c" "%SRC%\tuning_slider.c" ^
  "%SRC%\tunes.c" "%SRC%\tuning_reload.c" "%SRC%\entry_hook.c" "%SRC%\ui_badge.c" "%SRC%\car_reprepare.c" ^
  "%SRC%\car_rebuild.c" "%SRC%\hp_display.c" -luser32
if errorlevel 1 exit /b 1
del "%OUT%\EnhancedTuning.pdb" "%OUT%\EnhancedTuning.lib" "%OUT%\livetune.lib" 2>nul
echo Built release\Enhanced Tuning\plugins\EnhancedTuning.dll
