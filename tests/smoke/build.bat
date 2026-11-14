@echo off
rem Toolchain smoke test. Run from the project folder: tests\smoke\build.bat
setlocal
set ROOT=%~dp0..\..
set OUT=%ROOT%\build\smoke
if not exist "%OUT%" mkdir "%OUT%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -version [17.0^,18.0^) -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo MSVC 2022 Build Tools not found & exit /b 1)
rem vcvars64 calls vswhere by name, so put the installer folder on PATH
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
echo MSVC: %VS% (%VCToolsVersion%)

rem 1. clang 9 builds the "lifted" DLL with no CRT and no entry point
"%ROOT%\deps\llvm\bin\clang.exe" -O1 -c "%~dp0lifted.c" -o "%OUT%\lifted.obj" || exit /b 1
"%ROOT%\deps\llvm\bin\lld-link.exe" /dll /noentry /nodefaultlib /out:"%OUT%\lifted.dll" "%OUT%\lifted.obj" || exit /b 1

rem 2. MSVC builds the host exe against SDL3 and the Vulkan headers
cl /nologo /O2 /W3 /I"%ROOT%\deps\SDL3\include" /I"%ROOT%\deps\Vulkan-Headers\include" "%~dp0host.c" /Fo"%OUT%\\" /Fe"%OUT%\smoke.exe" /link /LIBPATH:"%ROOT%\deps\SDL3\lib\x64" SDL3.lib "%OUT%\lifted.lib" || exit /b 1
copy /y "%ROOT%\deps\SDL3\lib\x64\SDL3.dll" "%OUT%" >nul

rem 3. glslang compiles a GLSL shader to SPIR-V
"%ROOT%\deps\glslang\bin\glslang.exe" -V "%~dp0tri.frag" -o "%OUT%\tri.spv" || exit /b 1

"%OUT%\smoke.exe"
