@echo off
setlocal
set Platform=windows
set PlatformArch=arm64
set PlatformSpec=vc17
set PlatformSpec_windows=vc17

set JAM=jam -sPlatform=%Platform% -sPlatformArch=%PlatformArch% -sPlatformSpec=%PlatformSpec% -sPlatformSpec_windows=%PlatformSpec_windows% -sConfig=dev

pushd prog\tools\dargbox
call create_vfsroms.bat
cd shaders
call compile_shaders_dx11.bat
call compile_shaders_metal.bat
call compile_shaders_spirV.bat
popd

pushd prog\samples\physTest
%JAM% -f jamfile-test-bullet
%JAM% -f jamfile-test-jolt
cd shaders
call compile_game_shaders-dx11.bat
popd

pushd samples\skiesSample\prog
%JAM%
cd shaders
call compile_shaders_dx12.bat
call compile_shaders_dx11.bat
popd

pushd samples\testGI\prog
%JAM%
cd shaders
call compile_shaders_dx12.bat
call compile_shaders_dx11.bat
popd

pushd outerSpace\prog
call build_aot_compiler_arm64.cmd
%JAM% -sNeedDasAotCompile=yes
%JAM% -sNeedDasAotCompile=yes -sDedicated=yes
%JAM% -f jamfile-decrypt
call compile_all_prog_vromfs-arm64.cmd
cd shaders
call compile_shaders_dx11.bat
call compile_shaders_dx12.bat
cd ..\..\develop\gui
call build_ui.cmd
popd

pushd outerSpace\prog\utils\dev_launcher
call create_vfsroms.bat
popd
endlocal
