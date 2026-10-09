@echo off
set "MINGW=C:\Users\kmric\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
set "PATH=%MINGW%;%PATH%"
mingw32-make %*
