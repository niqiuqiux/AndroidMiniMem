#!/bin/bash
# ============================================================================
# Android Memory Engine - 构建脚本 (Socket 测试版)
# ============================================================================

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[OK]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARN]${NC} $1"; }
print_error()   { echo -e "${RED}[ERR]${NC} $1"; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BUILD_TYPE="Release"
CLEAN=false
JOBS=$(nproc)
NDK_PATH=""
NO_OBFUSCATION=false
NO_STRIP=false

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            cat << EOF
用法: $0 [选项]

选项:
    -h, --help          显示帮助
    -c, --clean         清理后重新构建
    -d, --debug         调试构建（禁用混淆，保留符号）
    -r, --release       发布构建（默认）
    -j, --jobs <N>      并行任务数（默认: $(nproc)）
    --ndk <path>        指定 NDK 路径（默认: CMakeLists.txt 中的路径）
    --no-obfuscation    禁用代码混淆
    --no-strip          禁用符号剥离
EOF
            exit 0
            ;;
        -c|--clean)       CLEAN=true; shift ;;
        -d|--debug)       BUILD_TYPE="Debug"; NO_OBFUSCATION=true; NO_STRIP=true; shift ;;
        -r|--release)     BUILD_TYPE="Release"; shift ;;
        -j|--jobs)        JOBS="$2"; shift 2 ;;
        --ndk)            NDK_PATH="$2"; shift 2 ;;
        --no-obfuscation) NO_OBFUSCATION=true; shift ;;
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

# 解析 NDK 路径
if [ -z "$NDK_PATH" ]; then
    NDK_PATH="/home/qiu/Android/android-ndk-r28c"
fi

# 设置 LD_LIBRARY_PATH，确保 LLVM Pass 插件能找到 libc++.so
NDK_LIBCXX_DIR="${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64/lib/x86_64-unknown-linux-gnu"
if [ -d "$NDK_LIBCXX_DIR" ]; then
    export LD_LIBRARY_PATH="${NDK_LIBCXX_DIR}:${LD_LIBRARY_PATH}"
    print_info "LD_LIBRARY_PATH += ${NDK_LIBCXX_DIR}"
else
    print_warning "未找到 NDK libc++ 目录: ${NDK_LIBCXX_DIR}"
fi

# 构建 CMake 选项 — MiniMem 仅编译 Socket 接口
CMAKE_OPTIONS=(
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DANDROID_NDK="${NDK_PATH}"
    -DBUILD_SOCKET_INTERFACE=ON
)

# CMakeLists.txt 中 Linux 默认 FORCE ON 混淆，需要显式关闭
$NO_OBFUSCATION && CMAKE_OPTIONS+=(-DENABLE_OBFUSCATION=OFF)
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
