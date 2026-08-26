@echo off
REM sessionlog tests: run pytest with HTML report
REM Usage: double-click, or run in cmd/PowerShell
REM Deps: pip install pytest pytest-html
cd /d "%~dp0"

echo ============================================
echo  sessionlog unit tests + HTML report
echo  dir: %~dp0
echo ============================================

REM 1. check pytest
python -m pytest --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] pytest not found. Run: pip install pytest pytest-html
    pause
    exit /b 1
)

REM 2. check pytest-html
python -m pytest --help 2>&1 | findstr /i "html" >nul
if errorlevel 1 (
    echo [WARN] pytest-html not found. HTML report will be skipped.
    echo        Run: pip install pytest-html
)

REM 3. clean old artifacts
if exist report.html del report.html
if exist .pytest_cache rmdir /s /q .pytest_cache
if exist __pycache__ rmdir /s /q __pycache__
if exist tests\__pycache__ rmdir /s /q tests\__pycache__
if exist tests\.pytest_cache rmdir /s /q tests\.pytest_cache

REM 4. run tests + generate HTML
echo.
echo [1/2] running tests, generating HTML report...
python -m pytest -v --html=report.html --self-contained-html

REM 5. open report
echo.
echo [2/2] opening report.html...
start "" report.html

echo.
echo Done. Report: %~dp0report.html
pause
