@echo off
rem Plays the game as usual while saving every picture it shows (once each) to
rem C:\dev\ConkerRecompWin\texdump_play, so the HD Icons pack can cover screens the test runs
rem can't reach (tools/textures/build_icon_pack.sh picks them up).
set RT64_DUMP_TEXTURES=C:\dev\ConkerRecompWin\texdump_play
if not exist C:\dev\ConkerRecompWin\texdump_play mkdir C:\dev\ConkerRecompWin\texdump_play
start "" "C:\dev\ConkerRecompWin\host\build\ConkerBFDReloaded.exe"
