@echo off
if not "%MINGW%"=="" set "PATH=%MINGW%;%PATH%"
where mingw32-make >nul 2>nul
if errorlevel 1 (
    if exist "C:\mingw64\bin" set "PATH=C:\mingw64\bin;%PATH%"
)
mingw32-make %*
