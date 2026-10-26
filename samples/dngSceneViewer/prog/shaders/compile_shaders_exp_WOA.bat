@"../../../../tools/dagor_cdk/windows-arm64/dsc2-stub-dev.exe" shaders_tools_exp.blk -clearBlkHashInDump -q -shaderOn -no_sha1_cache -o ../../../../_output/shaders/dngSceneViewer-exp~dx11 %*
@if %ERRORLEVEL% NEQ 0 exit /b 1
