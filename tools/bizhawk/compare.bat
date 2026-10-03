@echo off
cd /d C:\dev\ConkerRecompWin\bizhawk
del /q shots_cmp\*.* 2>nul
C:\dev\ConkerRecompWin\bizhawk\EmuHawk.exe --lua=C:\dev\ConkerRecompWin\bizhawk\compare.lua C:\dev\ConkerRecompWin\bizhawk\conker.z64
