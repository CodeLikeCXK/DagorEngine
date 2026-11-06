@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem dngSceneViewer performance capture helper.
rem Usage:
rem   capture_performance.cmd [dx11|dx12] [arm64|x86_64] [extra viewer arguments]
rem Examples:
rem   capture_performance.cmd dx12 arm64
rem   capture_performance.cmd dx11 arm64 -config:video/resolution:t=1280x720

set "SCRIPT_DIR=%~dp0"
set "VIEWER_DIR=%SCRIPT_DIR%viewer"
set "API=%~1"
set "ARCH=%~2"

if not defined API set "API=dx12"
if not defined ARCH set "ARCH=arm64"

if /I not "%API%"=="dx11" if /I not "%API%"=="dx12" goto :usage
if /I not "%ARCH%"=="arm64" if /I not "%ARCH%"=="x86_64" goto :usage

set "EXE=%VIEWER_DIR%\windows-%ARCH%\dng_scene_viewer-dev.exe"
set "SCENE=content/east_district/scenes/east_district_demo.blk"
set "CAPTURE_ROOT=%SCRIPT_DIR%captures"

if not exist "%EXE%" (
  echo ERROR: viewer executable not found:
  echo        %EXE%
  exit /b 2
)

where wpr.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows Performance Recorder ^(wpr.exe^) was not found.
  exit /b 3
)

where typeperf.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: typeperf.exe was not found.
  exit /b 4
)

for /f %%I in ('powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "STAMP=%%I"
if not defined STAMP set "STAMP=unknown_time"

set "OUT=%CAPTURE_ROOT%\%STAMP%_%API%_%ARCH%"
mkdir "%OUT%" >nul 2>&1
if not exist "%OUT%" (
  echo ERROR: cannot create output directory:
  echo        %OUT%
  exit /b 5
)

echo dngSceneViewer performance capture
echo API:       %API%
echo Arch:      %ARCH%
echo Executable:%EXE%
echo Output:    %OUT%
echo.

(
  echo capture_start=%date% %time%
  echo api=%API%
  echo arch=%ARCH%
  echo executable=%EXE%
  echo scene=%SCENE%
  echo command_line=%EXE% -config:scene:t=%SCENE% -config:video/driver:t=%API% -config:graphics/consolePreset:t=low %~3 %~4 %~5 %~6 %~7 %~8 %~9
  echo working_directory=%VIEWER_DIR%
) > "%OUT%\metadata.txt"

systeminfo > "%OUT%\systeminfo.txt" 2>&1
powercfg /getactivescheme > "%OUT%\power_scheme.txt" 2>&1
wmic cpu get Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed /format:list > "%OUT%\cpu.txt" 2>&1
wmic path Win32_VideoController get Name,DriverVersion,AdapterRAM /format:list > "%OUT%\gpu.txt" 2>&1
tasklist /v > "%OUT%\processes_before.txt" 2>&1

rem Keep a copy of the active viewer configuration used by the run.
if exist "%VIEWER_DIR%\config.blk" copy /y "%VIEWER_DIR%\config.blk" "%OUT%\config.blk" >nul
if exist "%VIEWER_DIR%\dng_scene_viewer.config.blk" copy /y "%VIEWER_DIR%\dng_scene_viewer.config.blk" "%OUT%\dng_scene_viewer.config.blk" >nul

set "WPR_STARTED=0"
wpr -cancel >nul 2>&1
wpr -start CPU -start GPU -filemode > "%OUT%\wpr_start.log" 2>&1
if errorlevel 1 (
  rem Some Windows installations do not expose the built-in GPU profile.
  rem Fall back to CPU scheduling/usage capture instead of aborting the run.
  wpr -cancel >nul 2>&1
  wpr -start CPU -filemode >> "%OUT%\wpr_start.log" 2>&1
  if errorlevel 1 (
    echo WARNING: WPR could not be started. See "%OUT%\wpr_start.log".
  ) else (
    set "WPR_STARTED=1"
    echo WARNING: WPR GPU profile unavailable; CPU profile is being collected.
  )
) else (
  set "WPR_STARTED=1"
)

rem Record stable system counters until the viewer exits. WPR contains the detailed
rem thread scheduling information; typeperf provides an easy-to-read CSV summary.
start "dngSceneViewer typeperf" /b typeperf.exe ^
  "\Processor(_Total)\%% Processor Time" ^
  "\Processor(*)\%% Processor Time" ^
  "\Memory\Available MBytes" ^
  "\System\Processor Queue Length" ^
  -si 1 -o "%OUT%\system_counters.csv" > "%OUT%\typeperf.log" 2>&1

pushd "%VIEWER_DIR%"
echo Starting viewer. Close the viewer when the reproduction is complete.
start "dngSceneViewer capture" /wait "%EXE%" ^
  "-config:scene:t=%SCENE%" ^
  "-config:video/driver:t=%API%" ^
  "-config:graphics/consolePreset:t=low" ^
  %~3 %~4 %~5 %~6 %~7 %~8 %~9
set "VIEWER_EXIT_CODE=%errorlevel%"
popd

rem Stop only the counter process belonging to this capture. Matching the output
rem filename avoids terminating another typeperf instance owned by the user.
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$out = [IO.Path]::GetFullPath('%OUT%\system_counters.csv'); Get-CimInstance Win32_Process -Filter \"Name='typeperf.exe'\" | Where-Object { $_.CommandLine -like ('*' + $out + '*') } | ForEach-Object { Invoke-CimMethod -InputObject $_ -MethodName Terminate }" > "%OUT%\typeperf_stop.log" 2>&1

if "%WPR_STARTED%"=="1" (
  wpr -stop "%OUT%\dngSceneViewer_%API%_%ARCH%.etl" "dngSceneViewer %API% %ARCH%" > "%OUT%\wpr_stop.log" 2>&1
  if errorlevel 1 echo WARNING: WPR could not be stopped. See "%OUT%\wpr_stop.log".
)

tasklist /v > "%OUT%\processes_after.txt" 2>&1

rem Preserve the engine's game logs without changing the source log directory.
if exist "%VIEWER_DIR%\.game_logs" (
  xcopy "%VIEWER_DIR%\.game_logs" "%OUT%\.game_logs" /E /I /H /Y >nul 2>&1
)

(
  echo capture_end=%date% %time%
  echo viewer_exit_code=%VIEWER_EXIT_CODE%
  echo output=%OUT%
) >> "%OUT%\metadata.txt"

echo.
echo Capture complete:
echo %OUT%
exit /b %VIEWER_EXIT_CODE%

:usage
echo Usage: %~nx0 [dx11^|dx12] [arm64^|x86_64] [extra viewer arguments]
echo Default: dx12 arm64
exit /b 1
