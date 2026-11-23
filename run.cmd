@echo off
rem Build RDX and boot it in QEMU. Double-click this file, or from a terminal:
rem   run.cmd            build + boot (QEMU window; kernel log shows here)
rem   run.cmd nobuild    boot the last build without recompiling
rem   run.cmd usb        give the virtual machine a USB keyboard instead of
rem                      relying on the PS/2 one (what most real PCs look like)
rem Options can be combined: run.cmd nobuild usb
rem
rem Inside RDX, type "desktop" for the graphical desktop. The mouse is a
rem virtual USB tablet, so it moves straight in and out of the window.
setlocal
cd /d "%~dp0"
title RDX

set "BUILD=1"
set "USB="
for %%A in (%*) do (
    if /i "%%A"=="nobuild" set "BUILD=0"
    if /i "%%A"=="usb" set "USB=-device usb-kbd,bus=usb.0"
)

if "%BUILD%"=="0" goto boot
call build.cmd
if errorlevel 1 goto fail

:boot
if not exist "build\esp\EFI\BOOT\BOOTX64.EFI" (
    echo ERROR: nothing has been built yet. Run without "nobuild" first.
    goto fail
)
if not exist "build\disk.img" (
    echo ERROR: build\disk.img is missing. Run without "nobuild" to create it.
    goto fail
)

set "QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe"
for /f "delims=" %%Q in ('where qemu-system-x86_64 2^>nul') do set "QEMU=%%Q"
if not exist "%QEMU%" (
    echo ERROR: QEMU was not found.
    echo Install it with:  winget install SoftwareFreedomConservancy.QEMU
    goto fail
)
for %%Q in ("%QEMU%") do set "OVMF=%%~dpQshare\edk2-x86_64-code.fd"
if not exist "%OVMF%" (
    echo ERROR: UEFI firmware not found at "%OVMF%"
    goto fail
)

echo === Booting RDX in QEMU (close the QEMU window to stop) ===
"%QEMU%" -machine q35 -accel whpx -accel tcg -m 1G -smp 4 ^
    -drive "if=pflash,format=raw,readonly=on,file=%OVMF%" ^
    -drive "format=raw,file=fat:rw:build\esp" ^
    -drive "format=raw,file=build\disk.img" ^
    -netdev user,id=net0 -device e1000,netdev=net0 -no-reboot -serial stdio ^
    -device qemu-xhci,id=usb -device usb-tablet,bus=usb.0 %USB%
if errorlevel 1 (
    echo.
    echo ERROR: QEMU exited with code %errorlevel%.
    goto fail
)
exit /b 0

:fail
echo.
echo Press any key to close this window...
pause >nul
exit /b 1
