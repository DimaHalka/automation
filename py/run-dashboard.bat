@echo off
cd /d "%~dp0"
if exist "C:\Program Files\Python312\pythonw.exe" (
    start "" "C:\Program Files\Python312\pythonw.exe" dashboard.py
) else (
    start "" pythonw dashboard.py
)
