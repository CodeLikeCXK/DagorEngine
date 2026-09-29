@echo off
set Platform=windows
set PlatformArch=arm64
set PlatformSpec=vc17
if not "%1" == "" set PlatformArch=%1

set JAM=jam -sPlatform=windows -sPlatformArch=%PlatformArch% -sPlatformSpec=%PlatformSpec% -sPlatformSpec_windows=%PlatformSpec% -s Root=../..

rem DaEditorX
%JAM% -f sceneTools/daEditorX/jamfile-editor
  if errorlevel 1 goto error
%JAM% -f sceneTools/daEditorX/jamfile
  if errorlevel 1 goto error

rem dabuild
%JAM% -f sceneTools/assetExp/jamfile
  if errorlevel 1 goto error
%JAM% -f converters/ddsxCvt2/jamfile
  if errorlevel 1 goto error

rem AssetViewer
%JAM% -f AssetViewer/jamfile
  if errorlevel 1 goto error
%JAM% -f sceneTools/findUnusedTex/jamfile
  if errorlevel 1 goto error
%JAM% -f blkEditor/jamfile
  if errorlevel 1 goto error

rem daImpostorBaker
%JAM% -f sceneTools/impostorBaker/tool/jamfile
  if errorlevel 1 goto error

rem DDSx plugins
%JAM% -f sceneTools/assetExp/ddsxConv/jamfile
  if errorlevel 1 goto error
%JAM% -s iOS_exp=yes -f sceneTools/assetExp/ddsxConv/jamfile
  if errorlevel 1 goto error
%JAM% -s Tegra_exp=yes -f sceneTools/assetExp/ddsxConv/jamfile
  if errorlevel 1 goto error

rem shader compilers
%JAM% -f ../3rdPartyLibs/legacy_parser/dolphin/jamfile
  if errorlevel 1 goto error
%JAM% -f ../3rdPartyLibs/legacy_parser/whale/jamfile
  if errorlevel 1 goto error
%JAM% -f shaderCompiler2/jamfile-hlsl11
  if errorlevel 1 goto error
%JAM% -f shaderCompiler2/jamfile-hlsl2spirv
  if errorlevel 1 goto error
%JAM% -f shaderCompiler2/jamfile-hlsl2metal
  if errorlevel 1 goto error
%JAM% -f shaderCompiler2/jamfile-dx12
  if errorlevel 1 goto error
%JAM% -f shaderCompiler2/jamfile-stub
  if errorlevel 1 goto error
%JAM% -f shaderCompiler2/nodeBased/jamfile
  if errorlevel 1 goto error
%JAM% -f shaderInfo/jamfile
  if errorlevel 1 goto error
%JAM% -f ../3rdPartyLibs/scripts/duktape/jamfile
  if errorlevel 1 goto error

rem common minimal gui shaders for tools
pushd sceneTools\guiShaders_commonData
call compile_gui_shaders_dx11.cmd
call compile_gui_shaders_dx12.cmd
call compile_gui_shaders_spirv.cmd
popd

rem utils
%JAM% -f sceneTools/vromfsPacker/jamfile
  if errorlevel 1 goto error
%JAM% -f sceneTools/csvUtil/jamfile
  if errorlevel 1 goto error
%JAM% -f converters/ddsxCvt/jamfile
  if errorlevel 1 goto error
%JAM% -f converters/ddsx2dds/jamfile
  if errorlevel 1 goto error
%JAM% -f converters/ddsConverter/jamfile
  if errorlevel 1 goto error
%JAM% -f FontGenerator/jamfile
  if errorlevel 1 goto error
%JAM% -f converters/GuiTex/jamfile
  if errorlevel 1 goto error
%JAM% -f sceneTools/utils/jamfile-binBlk
  if errorlevel 1 goto error

%JAM% -f sceneTools/dumpGrp/jamfile
  if errorlevel 1 goto error
%JAM% -f sceneTools/dbldUtil/jamfile
  if errorlevel 1 goto error
%JAM% -f sceneTools/resDiff/jamfile
  if errorlevel 1 goto error
%JAM% -f sceneTools/resUpdate/jamfile
  if errorlevel 1 goto error
%JAM% -f sceneTools/resClean/jamfile
  if errorlevel 1 goto error

%JAM% -sConfig=dev -f consoleSq/jamfile
  if errorlevel 1 goto error

rem GUI tools
%JAM% -f dargbox/jamfile
  if errorlevel 1 goto error

rem Blender plugin
pushd dag4blend
__build_pack.py FINAL
popd

rem 3ds Max plugins, we don't care if these plugins fail to compile (this could happen due to missing SDK or compiler)
%JAM% -s MaxVer=Max2025 -f maxplug/jamfile
%JAM% -s MaxVer=Max2025 -f maxplug/jamfile-imp

%JAM% -s MaxVer=Max2024 -f maxplug/jamfile
%JAM% -s MaxVer=Max2024 -f maxplug/jamfile-imp

%JAM% -s MaxVer=Max2023 -f maxplug/jamfile
%JAM% -s MaxVer=Max2023 -f maxplug/jamfile-imp

%JAM% -s MaxVer=Max2022 -f maxplug/jamfile
%JAM% -s MaxVer=Max2022 -f maxplug/jamfile-imp

goto EOF

:error

echo.
echo An error occured
exit /b 1

:EOF
exit /b 0
