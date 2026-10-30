@echo off
rem Builds Chorium.exe (the Windows runtime) with Visual Studio 2022 into the
rem runtime folder (next to chorium.dll) and copies wasmtime.dll there.
rem Windowed program: double-clicking it runs the app in its apps folder.
rem
rem   build.cmd [runtime dir] [wasmtime c-api dir]
setlocal
set HERE=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=C:\me-tor\chorium-out\win-x64
set WASMTIME=%~2
if "%WASMTIME%"=="" set WASMTIME=C:\me-tor\chorium-tools\wasmtime\wasmtime-v50.0.1-x86_64-windows-c-api

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "delims=" %%i in ('call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set VS=%%i
if not defined VS (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set FLAGS=/nologo /O2 /W4 /WX /utf-8 /MD /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
  /wd4100 /wd4201 /I "%WASMTIME%\include" /I "%HERE%..\..\interfaces\native"

rem The Direct2D surface is C++ (DirectWrite has no C headers).
cl %FLAGS% /std:c++17 /EHsc /c /Fo"%OUT%\obj\surface_d2d.obj" "%HERE%surface_d2d.cpp" || exit /b 1

cl %FLAGS% /std:c11 ^
  /Fo"%OUT%\obj\\" /Fe"%OUT%\Chorium.exe" ^
  "%HERE%main.c" "%HERE%contract.c" "%HERE%vals.c" "%HERE%json.c" "%HERE%loader.c" ^
  "%OUT%\obj\surface_d2d.obj" ^
  /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /STACK:0x800000 /MANIFEST:EMBED /MANIFESTINPUT:"%HERE%host.manifest" ^
  "%OUT%\chorium.dll.lib" "%WASMTIME%\lib\wasmtime.dll.lib" ^
  user32.lib gdi32.lib shell32.lib ws2_32.lib advapi32.lib userenv.lib ntdll.lib bcrypt.lib ole32.lib comctl32.lib d2d1.lib dwrite.lib || exit /b 1

copy /y "%WASMTIME%\lib\wasmtime.dll" "%OUT%\" >nul || exit /b 1
echo built %OUT%\Chorium.exe
