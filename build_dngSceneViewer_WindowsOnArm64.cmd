@echo off
setlocal
set Platform=windows
set PlatformArch=arm64
set PlatformSpec=vc17
set PlatformSpec_windows=vc17

set JAM=jam -sPlatform=%Platform% -sPlatformArch=%PlatformArch% -sPlatformSpec=%PlatformSpec% -sPlatformSpec_windows=%PlatformSpec_windows% -sConfig=dev

pushd samples\dngSceneViewer\prog
%JAM%
if errorlevel 1 (
  popd
  exit /b 1
)

call compile_all_prog_vromfs-WOA.bat
if errorlevel 1 (
  popd
  exit /b 1
)

cd shaders
call compile_shaders_dx11_WOA.bat
call compile_shaders_dx12_WOA.bat
call compile_shaders_tools_WOA.bat
popd
endlocal