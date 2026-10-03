@echo off
cd /d C:\dev\ConkerRecompWin\bizhawk
if not exist shots_intro mkdir shots_intro
del /q shots_intro\*.* 2>nul
C:\dev\ConkerRecompWin\bizhawk\EmuHawk.exe --lua=C:\dev\ConkerRecompWin\bizhawk\intro.lua C:\dev\ConkerRecompWin\bizhawk\conker.z64
