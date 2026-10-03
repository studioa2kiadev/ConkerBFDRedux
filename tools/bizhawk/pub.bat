@echo off
cd /d C:\dev\ConkerRecompWin\bizhawk
if not exist shots_intro mkdir shots_intro
C:\dev\ConkerRecompWin\bizhawk\EmuHawk.exe --lua=C:\dev\ConkerRecompWin\bizhawk\pub.lua C:\dev\ConkerRecompWin\bizhawk\conker.z64
