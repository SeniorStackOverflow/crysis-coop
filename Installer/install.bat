@echo off
rem Crysis Coop - double-click installer (runs install.ps1 next to this file)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
