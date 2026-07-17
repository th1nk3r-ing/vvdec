@echo off
setlocal

rem Locate Visual Studio via vswhere (ships with VS Installer)
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% set VSWHERE="%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
  echo ERROR: vswhere.exe not found. Please install Visual Studio.
  exit /b 1
)

for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if not defined VSINSTALL (
  echo ERROR: No Visual Studio with VC++ tools found.
  exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"

rem Use christian/develop branch which has DLL export fixes
git checkout -B develop christian/develop 2>nul

if not exist build mkdir build
cd build
if not exist Makefile cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DVVDEC_LIBRARY_ONLY=ON -DCMAKE_CXX_FLAGS="/wd4819" -DCMAKE_C_FLAGS="/wd4819" ..
if errorlevel 1 (
  echo ERROR: CMake configuration failed.
  pause
  exit /b 1
)

rem Build only the vvdec target (shared library)
nmake vvdec
if errorlevel 1 (
  echo ERROR: Build failed.
  pause
  exit /b 1
)

rem YUView expects vvdecLib.dll, but CMake target name is vvdec.
rem vvdec outputs to bin\release-shared\vvdec.dll
for /r "..\bin" %%F in (vvdec.dll) do (
  copy /y "%%F" vvdecLib.dll >nul
  echo Copied: %%F -> vvdecLib.dll
)

echo.
echo === vvdec build done ===
echo Output: build\vvdecLib.dll
pause
