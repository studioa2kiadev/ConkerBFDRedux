@echo off
rem Runs our program straight into the game for %1 seconds of game time (default 40),
rem log in host_run.log. Optional: %2 = speed (fast-forward, e.g. 4), %3 = game seconds
rem to screenshot at, comma-separated (snap.ps1 -Log takes them).
set SECS=%~1
if "%SECS%"=="" set SECS=40
set CONKER_NO_CONTROLLER=1
set CONKER_PROBE=1
if exist C:\dev\ConkerRecompWin\log_window.txt set /p CONKER_LOG_WINDOW=<C:\dev\ConkerRecompWin\log_window.txt
if exist C:\dev\ConkerRecompWin\fblog.flag set CONKER_RT64_FBLOG=1
if exist C:\dev\ConkerRecompWin\texdump.flag set RT64_DUMP_TEXTURES=C:\dev\ConkerRecompWin\texdump
if exist C:\dev\ConkerRecompWin\rdramdump.flag set CONKER_RDRAM_DUMP=1
if exist C:\dev\ConkerRecompWin\test_crosshair.flag set CONKER_TEST_CROSSHAIR=1
if exist C:\dev\ConkerRecompWin\cam_trace.flag set CONKER_CAM_TRACE=1
if exist C:\dev\ConkerRecompWin\cam_nozoom.flag set CONKER_CAM_NOZOOM=1
if exist C:\dev\ConkerRecompWin\test_poke.txt set /p CONKER_TEST_POKE=<C:\dev\ConkerRecompWin\test_poke.txt
if exist C:\dev\ConkerRecompWin\vtxlight.flag set RT64_CBFD_VTXLIGHT_LOG=C:\dev\ConkerRecompWin\snaps\vtxlight.txt
if exist C:\dev\ConkerRecompWin\cull43.flag set CONKER_TEST_43_CULLING=1
if exist C:\dev\ConkerRecompWin\old_lightpos.flag set RT64_CBFD_OLD_LIGHTPOS=1
if exist C:\dev\ConkerRecompWin\dist_scale.txt set /p RT64_CBFD_DIST_SCALE=<C:\dev\ConkerRecompWin\dist_scale.txt
if exist C:\dev\ConkerRecompWin\ucode_log.flag set RT64_UCODE_LOG=1
if exist C:\dev\ConkerRecompWin\light_log.flag set RT64_CBFD_LIGHT_LOG=1
if exist C:\dev\ConkerRecompWin\point_dir.flag set RT64_CBFD_POINT_DIR=1
if exist C:\dev\ConkerRecompWin\basic_dir.flag set RT64_CBFD_BASIC_DIR=1
if exist C:\dev\ConkerRecompWin\light_off.flag set RT64_CBFD_LIGHT_OFF=1
if exist C:\dev\ConkerRecompWin\point_scale.txt set /p RT64_CBFD_POINT_SCALE=<C:\dev\ConkerRecompWin\point_scale.txt
if exist C:\dev\ConkerRecompWin\no_texgen.flag set RT64_CBFD_NO_TEXGEN=1
if exist C:\dev\ConkerRecompWin\texgen_log.flag set RT64_CBFD_TEXGEN_LOG=1
if exist C:\dev\ConkerRecompWin\snap_room.txt set /p CONKER_SNAP_ROOM=<C:\dev\ConkerRecompWin\snap_room.txt
if exist C:\dev\ConkerRecompWin\probe_voices.flag set CONKER_PROBE_VOICES=1
if exist C:\dev\ConkerRecompWin\probe_pause.flag set CONKER_PROBE_PAUSE=1
if exist C:\dev\ConkerRecompWin\present_log.flag set RT64_PRESENT_LOG=C:\dev\ConkerRecompWin\snaps\presents.txt
if exist C:\dev\ConkerRecompWin\record.flag set CONKER_RECORD_WAV=C:\dev\ConkerRecompWin\snaps\sound.wav
if not "%~2"=="" set CONKER_SPEED=%~2
if not "%~3"=="" set CONKER_SNAP_AT=%~3
if exist C:\dev\ConkerRecompWin\skip_intro.flag set CONKER_SKIP_INTRO=1
if exist C:\dev\ConkerRecompWin\input_script.txt set /p CONKER_INPUT_SCRIPT=<C:\dev\ConkerRecompWin\input_script.txt
"C:\dev\ConkerRecompWin\host\build\ConkerBFDReloaded.exe" --seconds %SECS% > C:\dev\ConkerRecompWin\host_run.log 2>&1
echo EXITCODE=%errorlevel%
