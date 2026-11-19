@echo off
rem Build everything into build\esp, the folder QEMU boots as a disk:
rem   build\esp\EFI\BOOT\BOOTX64.EFI   the kernel (Rust)
rem   build\esp\bin\*.exe              programs (C, from user\programs\*.c)
rem   build\esp\bin\*                  scripts and data (copied from user\files)
rem Returns a non-zero exit code if anything fails.
setlocal
cd /d "%~dp0"

echo === Building the kernel ===
where cargo >nul 2>nul
if errorlevel 1 (
    echo ERROR: cargo was not found. Install Rust from https://rustup.rs
    exit /b 1
)
cargo build --release
if errorlevel 1 (
    echo.
    echo ERROR: the kernel failed to build. See the compiler output above.
    exit /b 1
)
if not exist "build\esp\EFI\BOOT" mkdir "build\esp\EFI\BOOT"
copy /y "target\x86_64-unknown-uefi\release\rdx.efi" "build\esp\EFI\BOOT\BOOTX64.EFI" >nul
if errorlevel 1 (
    echo ERROR: could not copy the kernel into build\esp. Is QEMU still running?
    exit /b 1
)

echo === Building programs ===
set "LLVM=C:\Program Files\LLVM\bin"
for /f "delims=" %%C in ('where clang 2^>nul') do set "LLVM=%%~dpC"
if not exist "%LLVM%\clang.exe" (
    echo ERROR: clang was not found.
    echo Install it with:  winget install LLVM.LLVM
    exit /b 1
)
if not exist "build\user" mkdir "build\user"
if not exist "build\esp\bin" mkdir "build\esp\bin"

rem Freestanding C: no Windows headers, no C library, no stack probes.
rem Each function in its own section, so that the linker leaves out the
rem library code a program does not use.
set CFLAGS=--target=x86_64-pc-windows-msvc -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe -O2 -Wall -Wextra -ffunction-sections -fdata-sections -Iuser
rem A plain PE image at the address the RDX loader expects, importing nothing.
set LDFLAGS=/subsystem:native /entry:_start /nodefaultlib /fixed /base:0x10000400000 /nologo /opt:ref

rem The libraries every program can use: each user\*.c file (the C
rem runtime rdx.c, the desktop's gui.c, https in tls.c, and so on). The
rem linker takes from them only what a program actually uses.
if not exist "build\lib" mkdir "build\lib"
del /q "build\lib\*.obj" 2>nul
set "LIBS="
for %%Q in (user\*.c) do (
    "%LLVM%\clang.exe" %CFLAGS% -c "%%Q" -o "build\lib\%%~nQ.obj"
    if errorlevel 1 (
        echo ERROR: %%Q failed to compile.
        exit /b 1
    )
    call set "LIBS=%%LIBS%% build\lib\%%~nQ.obj"
)
for %%F in (user\programs\*.c) do (
    echo   %%~nF.exe
    "%LLVM%\clang.exe" %CFLAGS% -c "%%F" -o "build\user\%%~nF.obj"
    if errorlevel 1 (
        echo ERROR: %%F failed to compile.
        exit /b 1
    )
    "%LLVM%\lld-link.exe" %LDFLAGS% "/out:build\esp\bin\%%~nF.exe" "build\user\%%~nF.obj" %LIBS%
    if errorlevel 1 (
        echo ERROR: %%~nF.exe failed to link.
        exit /b 1
    )
)

rem Scripts and other plain files that should be on the boot disk.
copy /y "user\files\*" "build\esp\bin\" >nul

rem The data disk: where files created inside RDX are saved. It is created
rem once and then kept, so your files survive rebuilds. RDX only writes to a
rem disk that starts with the signature RDXDISK1. Delete the file to wipe it.
if not exist "build\disk.img" powershell -NoProfile -Command "$f=[IO.File]::Create('build\disk.img'); $f.SetLength(16MB); $b=[Text.Encoding]::ASCII.GetBytes('RDXDISK1'); $f.Write($b,0,8); $f.Close()"
if not exist "build\disk.img" (
    echo ERROR: could not create build\disk.img
    exit /b 1
)
exit /b 0
