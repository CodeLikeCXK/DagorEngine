@if not exist "..\..\viewer\compiledShaders\compatPC" mkdir "..\..\viewer\compiledShaders\compatPC"
@"../../../../tools/dagor_cdk/windows-arm64/dsc2-hlsl11-dev.exe" shaders_dx11.blk -q -shaderOn -nodisassembly -commentPP -codeDumpErr -maxVSF 4096 -o ../../../../_output/shaders/dngSceneViewer~dx11 %1 %2 %3 %4 %5 %6 %7 %8 %9
@if %ERRORLEVEL% NEQ 0 exit /b 1
@"../../../../tools/dagor_cdk/windows-arm64/dsc2-hlsl11-dev.exe" shaders_compat11.blk -q -shaderOn -nodisassembly -commentPP -codeDumpErr -maxVSF 4096 -o ../../../../_output/shaders/dngSceneViewer~compat11 %1 %2 %3 %4 %5 %6 %7 %8 %9
@if %ERRORLEVEL% NEQ 0 exit /b 1
