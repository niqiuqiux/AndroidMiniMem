#!/bin/bash
# ============================================================================
# Android Memory Engine - 构建脚本 (Socket 测试版)
# ============================================================================

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

print_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[OK]${NC} $1"; }
print_error()   { echo -e "${RED}[ERR]${NC} $1"; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BUILD_TYPE="Release"
CLEAN=false
JOBS=$(nproc)
NDK_PATH=""
NO_STRIP=false

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            cat << EOF
用法: $0 [选项]

选项:
    -h, --help          显示帮助
    -c, --clean         清理后重新构建
    -d, --debug         调试构建（保留符号）
    -r, --release       发布构建（默认）
    -j, --jobs <N>      并行任务数（默认: $(nproc)）
    --ndk <path>        指定 NDK 路径（默认读取 ANDROID_NDK_HOME）
    --no-strip          禁用符号剥离
EOF
            exit 0
            ;;
        -c|--clean)       CLEAN=true; shift ;;
        -d|--debug)       BUILD_TYPE="Debug"; NO_STRIP=true; shift ;;
        -r|--release)     BUILD_TYPE="Release"; shift ;;
        -j|--jobs)        JOBS="$2"; shift 2 ;;
        --ndk)            NDK_PATH="$2"; shift 2 ;;
        --no-strip)       NO_STRIP=true; shift ;;
        *) print_error "未知选项: $1"; exit 1 ;;
    esac
done

# 清理
if [ "$CLEAN" = true ]; then
    print_info "清理构建目录..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

# 解析 NDK 路径：命令行优先，其次使用 Android 工具链标准环境变量。
if [ -z "$NDK_PATH" ]; then
    NDK_PATH="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${ANDROID_NDK:-}}}"
fi
if [ -z "$NDK_PATH" ] || [ ! -f "$NDK_PATH/build/cmake/android.toolchain.cmake" ]; then
    print_error "未找到有效的 Android NDK，请使用 --ndk <path> 或设置 ANDROID_NDK_HOME"
    exit 1
fi

# 构建 CMake 选项 — MiniMem 仅编译 Socket 接口
CMAKE_OPTIONS=(
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DANDROID_NDK="${NDK_PATH}"
    -DBUILD_SOCKET_INTERFACE=ON
)

# 根据命令行选项覆盖符号剥离设置
$NO_STRIP && CMAKE_OPTIONS+=(-DENABLE_STRIP=OFF)

# 配置
print_info "配置项目 [${BUILD_TYPE}] ..."
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" "${CMAKE_OPTIONS[@]}"
print_success "配置完成"

# 构建
print_info "开始构建 (-j${JOBS}) ..."
cmake --build "$BUILD_DIR" -j"$JOBS"
print_success "构建成功"

# 显示产物
echo ""
for dir in bin lib; do
    target="${SCRIPT_DIR}/${dir}"
    if [ -d "$target" ] && [ "$(ls -A "$target" 2>/dev/null)" ]; then
        print_info "${dir}/:"
        ls -lh "$target" 2>/dev/null | tail -n +2 | awk '{printf "  %-30s %s\n", $9, $5}'
    fi
done
