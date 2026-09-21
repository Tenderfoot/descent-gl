@echo off
rem Drop one or more videos on this to encode HUD comm clips.
rem
rem Explorer hands each dropped file over as its own argument, so this walks
rem them in order and gives each to make-comm-video.py. Where the clip
rem directory goes, how long the clip is and what happens to the soundtrack
rem are all the script's business; this file only finds an interpreter, keeps
rem the console open, and says which of the two programs is missing when one
rem of them is.

setlocal
set "SCRIPT=%~dp0make-comm-video.py"

if not exist "%SCRIPT%" (
    echo make-comm-video.py is not beside this batch file. Keep the two together.
    goto :hold
)

if "%~1"=="" (
    echo Drop one or more video files on this batch file. Each one becomes a
    echo clip directory of the same name, beside the file it came from.
    goto :hold
)

set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY (
    where python >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo Python was not found on PATH, so there is nothing to run the encoder with.
    goto :hold
)

where ffmpeg >nul 2>nul
if errorlevel 1 (
    echo ffmpeg was not found on PATH. make-comm-video.py shells out to ffmpeg
    echo and ffprobe for every clip, so it cannot do anything without them.
    goto :hold
)

rem The script holds its own console open for a file dropped straight on it.
rem The single pause below covers the whole drop instead, however many files
rem it carried, so tell the script to skip its own.
set "COMM_VIDEO_BATCH=1"
set "FAILED="

:next
if "%~1"=="" goto :done
echo === "%~nx1"
%PY% "%SCRIPT%" "%~f1"
if errorlevel 1 set "FAILED=1"
echo.
shift
goto :next

:done
if defined FAILED echo One or more clips failed. The messages above say why.

:hold
pause
endlocal
