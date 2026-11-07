@call ..\..\..\..\prog\_jBuild\make_dagor_tools_path.cmd
@%DAGOR_CDK_DIR%\dsc2-hlsl11-dev.exe shaders_compat11.blk -q -shaderOn -nodisassembly -commentPP -codeDumpErr -maxVSF 4096 -o ../../../../_output/shaders/dngSceneViewer-compat~dx11 -wx %*
@if %ERRORLEVEL% NEQ 0 exit /b 1

