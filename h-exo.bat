@echo off
REM h-exo.bat - Единый скрипт для H-Exo Omni-Core
REM build / deploy / test - все в одном

set ACTION=%1

if "%ACTION%"=="" goto help
if "%ACTION%"=="build" goto build
if "%ACTION%"=="deploy" goto deploy
if "%ACTION%"=="test" goto test
if "%ACTION%"=="all" goto all
goto help

:help
echo H-Exo Omni-Core - Unified Script
echo.
echo Usage: h-exo.bat [command]
echo.
echo Commands:
echo   build   - Build kernel
echo   deploy  - Deploy via TFTP
echo   test    - Run tests
echo   all     - Build + Deploy + Test
echo.
echo Examples:
echo   h-exo.bat build
echo   h-exo.bat deploy
echo   h-exo.bat all
goto end

:build
echo ========================================
echo   BUILD
echo ========================================
call build.bat
if errorlevel 1 goto error
goto end

:deploy
echo ========================================
echo   DEPLOY
echo ========================================
powershell -ExecutionPolicy Bypass -File deploy_tftp_fixed.ps1
if errorlevel 1 goto error
goto end

:test
echo ========================================
echo   TEST
echo ========================================
powershell -ExecutionPolicy Bypass -File test_universal.ps1
if errorlevel 1 goto error
goto end

:all
echo ========================================
echo   BUILD + DEPLOY + TEST
echo ========================================
call build.bat
if errorlevel 1 goto error
echo.
echo [!] Kernel built. Ready to deploy?
echo     Connect to COM3, then press any key...
pause >nul
powershell -ExecutionPolicy Bypass -File deploy_tftp_fixed.ps1
if errorlevel 1 goto error
echo.
echo [!] Kernel deployed. Ready to test?
echo     Press any key to start tests...
pause >nul
powershell -ExecutionPolicy Bypass -File test_universal.ps1
if errorlevel 1 goto error
goto end

:error
echo.
echo [!] FAILED with error %errorlevel%
exit /b 1

:end
echo.
echo [+] Done
