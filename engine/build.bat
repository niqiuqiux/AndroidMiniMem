@echo off
REM ============================================================================
REM Android MiniMem Engine - Windows 构建脚本（仅 Socket 接口）
REM ============================================================================

setlocal EnableDelayedExpansion

REM 默认参数
set BUILD_DIR=build
set BUILD_TYPE=Release
set CLEAN=0
set JOBS=%NUMBER_OF_PROCESSORS%
set NDK_PATH=
set NO_OBFUSCATION=0
set NO_STRIP=0

REM 解析命令行参数
:parse_args
if "%~1"=="" goto end_parse_args
if /I "%~1"=="--help" goto show_help
if /I "%~1"=="-h" goto show_help
if /I "%~1"=="--clean" (
    set CLEAN=1
    shift
    goto parse_args
)
if /I "%~1"=="--debug" (
    set BUILD_TYPE=Debug
    set NO_OBFUSCATION=1
    set NO_STRIP=1
    shift
    goto parse_args
)
if /I "%~1"=="--release" (
    set BUILD_TYPE=Release
    shift
    goto parse_args
)
if /I "%~1"=="--ndk" (
    set NDK_PATH=%~2
    shift
    shift
    goto parse_args
)
if /I "%~1"=="--no-obfuscation" (
    set NO_OBFUSCATION=1
    shift
    goto parse_args
)
if /I "%~1"=="--no-strip" (
    set NO_STRIP=1
    shift
    goto parse_args
)
echo 未知选项: %~1
goto show_help

:end_parse_args

REM 清理构建目录
if %CLEAN%==1 (
    echo [INFO] 清理构建目录...
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo [SUCCESS] 清理完成
)

REM 创建构建目录
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

REM 构建 CMake 选项 —— MiniMem 仅 Socket 接口
set CMAKE_OPTIONS=-DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DBUILD_SOCKET_INTERFACE=ON

if not "%NDK_PATH%"=="" (
    set CMAKE_OPTIONS=!CMAKE_OPTIONS! -DANDROID_NDK=%NDK_PATH%
)
if %NO_OBFUSCATION%==1 (
    set CMAKE_OPTIONS=!CMAKE_OPTIONS! -DENABLE_OBFUSCATION=OFF
)
if %NO_STRIP%==1 (
    set CMAKE_OPTIONS=!CMAKE_OPTIONS! -DENABLE_STRIP=OFF
)

REM 配置项目
echo [INFO] 配置项目...
echo [INFO] CMake 选项: %CMAKE_OPTIONS%
cd "%BUILD_DIR%"
cmake .. %CMAKE_OPTIONS%

if errorlevel 1 (
    echo [ERROR] CMake 配置失败
    exit /b 1
)

echo [SUCCESS] 配置完成

REM 构建项目
echo [INFO] 开始构建...
cmake --build . --config %BUILD_TYPE% -j %JOBS%

if errorlevel 1 (
    echo [ERROR] 构建失败
    exit /b 1
)

echo [SUCCESS] 构建成功！
echo [INFO] 输出文件位于: %CD%\..\bin\ 和 %CD%\..\lib\

REM 显示构建产物
if exist "..\bin" (
    echo [INFO] 可执行文件:
    dir /b ..\bin
)

cd ..
goto :eof

:show_help
echo Android MiniMem Engine - 构建脚本（仅 Socket 接口）
echo.
echo 用法: %~nx0 [选项]
echo.
echo 选项:
echo     -h, --help              显示此帮助信息
echo     --clean                 清理构建目录
echo     --debug                 调试构建（不混淆，保留符号）
echo     --release               发布构建（默认）
echo     --ndk ^<path^>            指定 NDK 路径
echo     --no-obfuscation        禁用代码混淆
echo     --no-strip              禁用符号剥离
echo.
echo 示例:
echo     %~nx0                           # 默认构建
echo     %~nx0 --clean --release         # 清理后重新构建
echo     %~nx0 --debug                   # 调试构建
echo.
exit /b 0
