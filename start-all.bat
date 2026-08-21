@echo off
setlocal
REM High-Performance LOB — one-click start (gateway + bot + frontend)
REM Requires: MSVC Build Tools (for build.ps1) and Python 3.14+ in PATH
REM All ports are 9000 (gateway serves TCP length-prefix + WS /ws + HTTP /)

echo === Building Release (lob_tests 162, gateway.exe) ===
powershell -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Config Release
if errorlevel 1 (
  echo Build failed — see above
  pause
  exit /b 1
)

echo.
echo === Starting Gateway (FastOrderBook) on 127.0.0.1:9000 ===
start "LOB Gateway — 127.0.0.1:9000" cmd /k "cd /d "%~dp0" && build\Release\gateway.exe --port 9000 --book fast"
timeout /t 2 /nobreak >nul

echo === Starting Market-Maker Bot (stdlib-only) ===
start "LOB Bot — market_maker" cmd /k "cd /d "%~dp0" && python -m bot.market_maker --port 9000 --duration 0"
timeout /t 1 /nobreak >nul

echo === Opening Frontend http://127.0.0.1:9000/ ===
start http://127.0.0.1:9000/
timeout /t 1 /nobreak >nul

echo.
echo All running:
echo   Gateway  : http://127.0.0.1:9000/  (also TCP 4B BE JSON and WS ws://127.0.0.1:9000/ws)
echo   Frontend : http://127.0.0.1:9000/  (no-build vanilla JS — book ladder, trades, chart canvas, order entry 1/2/Enter/Esc)
echo   Bot      : python -m bot.market_maker --port 9000  (quotes 98/102 around mid, inventory skew, self-trade防)
echo.
echo To place a manual order via bot client:
echo   python -m bot.mm_client --port 9000 --test
echo To stop: close the Gateway/Bot windows or press Enter in the Gateway window, then close browser.
echo.
pause
