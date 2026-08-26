@echo off
REM ============================================================
REM sessionlog 测试：本地运行 + HTML 报告
REM 用法：双击运行，或在 cmd / PowerShell 里执行 run_tests.bat
REM 依赖：pip install pytest pytest-html
REM ============================================================
cd /d "%~dp0"

echo ============================================
echo  sessionlog 单元测试 + HTML 报告
echo  目录: %~dp0
echo ============================================

REM 1. 检查 pytest 是否可用
python -m pytest --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] pytest 未安装，请先执行: pip install pytest pytest-html
    pause
    exit /b 1
)

REM 2. 检查 pytest-html 是否可用
python -m pytest --help 2>&1 | findstr /i "html" >nul
if errorlevel 1 (
    echo [WARN] pytest-html 未安装，报告将不包含 HTML
    echo        可执行: pip install pytest-html
)

REM 3. 清理旧报告（可选）
if exist report.html del report.html
if exist .pytest_cache rmdir /s /q .pytest_cache
if exist __pycache__ rmdir /s /q __pycache__
if exist tests\__pycache__ rmdir /s /q tests\__pycache__
if exist tests\.pytest_cache rmdir /s /q tests\.pytest_cache

REM 4. 运行测试 + 生成 HTML 报告
echo.
echo [1/2] 运行测试并生成 HTML 报告 ...
python -m pytest -v --html=report.html --self-contained-html

REM 5. 打开报告（无论成功失败都打开，方便看失败详情）
echo.
echo [2/2] 打开报告 report.html ...
start "" report.html

echo.
echo 完成。报告文件: %~dp0report.html
pause
