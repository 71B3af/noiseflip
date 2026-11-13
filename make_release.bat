@echo off
rem Run from "x64 Native Tools Command Prompt for VS 2022". Builds, tests, and zips release\Noiseflip.
call build_msvc.bat || exit /b 1
rmdir /s /q release 2>nul
mkdir release\Noiseflip
copy noiseflip.exe release\Noiseflip\ >nul
xcopy /e /i /q ui release\Noiseflip\ui >nul
copy README.md release\Noiseflip\ >nul
powershell -NoProfile -Command "Compress-Archive -Path release\Noiseflip -DestinationPath Noiseflip-release.zip -Force"
echo Made Noiseflip-release.zip
