@echo off
setlocal
rem Builds the plugin and creates dist\Road-Trip-Overhaul-v<version>.zip for players:
rem   road_trip_overhaul.dll, README.txt, LICENSE.txt, THIRD_PARTY_NOTICES.txt
rem The version comes from include\version.h.
cd /d "%~dp0"

call "%~dp0build.bat" >nul || (echo [ERROR] Build failed - run build.bat to see why. & exit /b 1)

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$h = Get-Content 'include\version.h' -Raw;" ^
  "$ver = [regex]::Match($h, 'RTO_VERSION \""([^\""]+)\""').Groups[1].Value;" ^
  "$game = [regex]::Match($h, 'RTO_GAME_VERSION \""([^\""]+)\""').Groups[1].Value;" ^
  "$stage = Join-Path $env:TEMP ('rto_pkg_' + [guid]::NewGuid());" ^
  "New-Item -ItemType Directory $stage -Force | Out-Null;" ^
  "Copy-Item 'bin\road_trip_overhaul.dll' $stage;" ^
  "(Get-Content 'packaging\README.txt' -Raw).Replace('{VERSION}', $ver).Replace('{GAME_VERSION}', $game) | Set-Content (Join-Path $stage 'README.txt') -Encoding ascii;" ^
  "Copy-Item 'LICENSE' (Join-Path $stage 'LICENSE.txt');" ^
  "Copy-Item 'THIRD_PARTY_NOTICES.md' (Join-Path $stage 'THIRD_PARTY_NOTICES.txt');" ^
  "New-Item -ItemType Directory 'dist' -Force | Out-Null;" ^
  "$zip = 'dist\Road-Trip-Overhaul-v' + $ver + '.zip';" ^
  "if (Test-Path $zip) { Remove-Item $zip };" ^
  "Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip;" ^
  "Remove-Item $stage -Recurse;" ^
  "Write-Host ('Created ' + $zip + ' (v' + $ver + ', ' + $game + ')')" || exit /b 1
