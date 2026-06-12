@echo off
adb shell am start -n io.nava.calculator/android.app.NativeActivity
timeout /t 1 /nobreak >nul
for /f %%i in ('adb shell pidof io.nava.calculator') do set PID=%%i
echo Watching PID: %PID%
adb logcat --pid=%PID% *:E APP:D Renderer:D vulkan:D goldfish_vulkan:S ThreadedRenderer:S
