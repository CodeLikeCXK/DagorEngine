@echo off
setlocal

set Platform=windows
set PlatformArch=arm64
set PlatformSpec=vc17
set PlatformSpec_windows=vc17
set Config=dev

set JAM=jam -sPlatform=%Platform% -sPlatformArch=%PlatformArch% -sPlatformSpec=%PlatformSpec% -sPlatformSpec_windows=%PlatformSpec_windows% -sConfig=%Config%

set TARGET_COMPONENT=%1
if "%TARGET_COMPONENT%"=="" set TARGET_COMPONENT=all

echo ==============================================================================
echo Building DagorEngine for Windows on ARM64 [Component: %TARGET_COMPONENT%]
echo Platform=%Platform%, PlatformArch=%PlatformArch%, PlatformSpec=%PlatformSpec%
echo ==============================================================================

if /I "%TARGET_COMPONENT%"=="code" goto build_code
if /I "%TARGET_COMPONENT%"=="shaders" goto build_shaders
if /I "%TARGET_COMPONENT%"=="vromfs" goto build_vromfs
if /I "%TARGET_COMPONENT%"=="all" goto build_all

echo Unknown component: %TARGET_COMPONENT%
echo Valid options: all, code, shaders, vromfs
exit /b 1

:build_all
call :do_build_code
if errorlevel 1 exit /b 1
call :do_build_shaders
if errorlevel 1 exit /b 1
call :do_build_vromfs
if errorlevel 1 exit /b 1
echo.
echo ==============================================================================
echo All components built successfully!
echo ==============================================================================
exit /b 0

:build_code
call :do_build_code
exit /b %ERRORLEVEL%

:build_shaders
call :do_build_shaders
exit /b %ERRORLEVEL%

:build_vromfs
call :do_build_vromfs
exit /b %ERRORLEVEL%

:: ------------------------------------------------------------------------------
:: Subroutine: Build C/C++ Code via Jam
:: ------------------------------------------------------------------------------
:do_build_code
echo.
echo [1/3] Building C/C++ Code...

echo --- Building CDK tools ---
pushd prog\tools
call build_dagor_cdk_mini_WindowsOnArm64.cmd
if errorlevel 1 (
  echo build_dagor_cdk_mini_WindowsOnArm64.cmd failed, trying once more...
  call build_dagor_cdk_mini_WindowsOnArm64.cmd
)
if errorlevel 1 (
  echo Failed to build CDK, stop!
  popd
  exit /b 1
)
popd

echo --- Building physTest ---
pushd prog\samples\physTest
%JAM%
if errorlevel 1 ( popd & exit /b 1 )
%JAM% -f jamfile-test-jolt
if errorlevel 1 ( popd & exit /b 1 )
popd

echo --- Building skiesSample ---
pushd samples\skiesSample\prog
%JAM%
if errorlevel 1 ( popd & exit /b 1 )
popd

echo --- Building testGI ---
pushd samples\testGI\prog
%JAM%
if errorlevel 1 ( popd & exit /b 1 )
popd

echo --- Building outerSpace ---
pushd outerSpace\prog
call build_aot_compiler_arm64.cmd
if errorlevel 1 ( popd & exit /b 1 )
%JAM% -sNeedDasAotCompile=yes
if errorlevel 1 ( popd & exit /b 1 )
%JAM% -sNeedDasAotCompile=yes -sDedicated=yes
if errorlevel 1 ( popd & exit /b 1 )
%JAM% -f jamfile-decrypt
if errorlevel 1 ( popd & exit /b 1 )
popd

echo C/C++ Code build complete.
exit /b 0

:: ------------------------------------------------------------------------------
:: Subroutine: Build Shaders
:: ------------------------------------------------------------------------------
:do_build_shaders
echo.
echo [2/3] Building Shaders...

pushd prog\tools\dargbox\shaders
call compile_shaders_dx11.bat
call compile_shaders_dx12.bat
popd

pushd prog\samples\physTest\shaders
call compile_game_shaders-dx11.bat
popd

pushd samples\skiesSample\prog\shaders
call compile_shaders_dx12.bat -cppStcodeArch=arm64
call compile_shaders_dx11.bat -cppStcodeArch=arm64
popd

pushd samples\testGI\prog\shaders
call compile_shaders_dx12.bat
call compile_shaders_dx11.bat
popd

pushd outerSpace\prog\shaders
call compile_shaders_dx11.bat
call compile_shaders_dx12.bat
call compile_shaders_tools.bat
popd

echo Shaders build complete.
exit /b 0

:: ------------------------------------------------------------------------------
:: Subroutine: Build VROMFS and UI
:: ------------------------------------------------------------------------------
:do_build_vromfs
echo.
echo [3/3] Building VROMFS and UI...

pushd prog\tools\dargbox
call create_vfsroms.bat
popd

pushd outerSpace\prog
call compile_all_prog_vromfs-arm64.cmd
popd

pushd outerSpace\develop\gui
call build_ui.cmd
popd

pushd outerSpace\prog\utils\dev_launcher
call create_vfsroms.bat
popd

echo VROMFS and UI build complete.
exit /b 0