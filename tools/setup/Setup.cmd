@echo off
rem HIDway setup: choose what to install on this PC (HIDway, Tailscale,
rem Moonlight, Sunshine), each with its own official installer.
rem   Setup.cmd            interactive
rem   Setup.cmd -Update    update what is installed
rem   Setup.cmd -WhatIf    show what would happen
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0hidway-setup.ps1" %*
set rc=%errorlevel%
if "%~1"=="" pause
exit /b %rc%
