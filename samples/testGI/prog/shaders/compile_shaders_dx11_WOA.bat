@"../../../../tools/dagor_cdk/windows-arm64/dsc2-hlsl11-dev.exe" shaders_dx11.blk -q -shaderOn -nodisassembly -commentPP -codeDumpErr -o ../../../../_output/shaders/testGI-game~dx11 %1 %2 %3 %4 %5 %6 %7 %8 %9
@if %ERRORLEVEL% NEQ 0 exit /b 1

