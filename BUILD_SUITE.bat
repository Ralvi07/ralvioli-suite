@echo off
rem Compiles the Windows resource files and builds the native Suite applications.
setlocal
cd /d "%~dp0" || exit /b 1
if not defined MSYS2_UCRT64_BIN set "MSYS2_UCRT64_BIN=C:\msys64\ucrt64\bin"
set "PATH=%MSYS2_UCRT64_BIN%;%PATH%"
if not exist "%MSYS2_UCRT64_BIN%\g++.exe" goto missing_msys2
if not exist "%MSYS2_UCRT64_BIN%\windres.exe" goto missing_msys2
if not defined ISCC if exist "%LocalAppData%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LocalAppData%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC set "ISCC=ISCC.exe"
if not exist "%ISCC%" (
    "%SystemRoot%\System32\where.exe" "%ISCC%" >nul 2>&1 || goto missing_inno
)
if not exist build mkdir build || exit /b 1
set "FLAGS=-std=c++17 -O2 -s -DUNICODE -D_UNICODE -mwindows -static -static-libgcc -static-libstdc++"
set "LIBS=-lcomdlg32 -lgdi32 -lcomctl32 -luser32"
call :build Merger "" || exit /b 1
call :build Viewer "-lshell32" || exit /b 1
call :build Plotter "-lsetupapi -lwinusb" || exit /b 1
"%ISCC%" RalvioliSuite_installer.iss || exit /b 1
echo Built build\Ralvioli Suite Setup.exe
exit /b 0

:build
echo Building Ralvioli %~1
"%MSYS2_UCRT64_BIN%\windres.exe" "Ralvioli%~1.rc" -O coff -o "build\Ralvioli%~1_res.o" || exit /b 1
"%MSYS2_UCRT64_BIN%\g++.exe" %FLAGS% -o "build\Ralvioli %~1.exe" "Ralvioli%~1.cpp" "build\Ralvioli%~1_res.o" %LIBS% %~2 || exit /b 1
exit /b 0

:missing_msys2
echo MSYS2 UCRT64 tools were not found in "%MSYS2_UCRT64_BIN%".
echo From the MSYS2 UCRT64 shell, run pacman -S mingw-w64-ucrt-x86_64-toolchain
exit /b 1

:missing_inno
echo Inno Setup 6 was not found. Install it or set ISCC to the path of ISCC.exe.
exit /b 1
