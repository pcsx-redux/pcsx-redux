@echo off
set OLDCWD=%cd%
cd %~dp0
set ROOT=%cd%
cd %OLDCWD%

set REL=%OLDCWD%

SETLOCAL EnableDelayedExpansion

set MAT=
set UPP=
for /f "tokens=*" %%a in ('echo.%ROOT:\=^&echo.%') do (
    set SUB=!SUB!%%a\
    call set TMP=%%src:!SUB!=%%
    set MAT=!SUB!
)
set REL=%UPP%!REL:%MAT%=!
if "!REL!" EQU "!ROOT!" (set REL=) ELSE (set "REL=!REL:\=/!")
set REL=/!REL!

docker pull ghcr.io/pcsx-redux/pcsx-redux-build:latest
docker run --rm -i -w"/project%REL%" -v "%ROOT%:/project" ghcr.io/pcsx-redux/pcsx-redux-build:latest make %*
