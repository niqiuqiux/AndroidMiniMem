# UE4 Actor 查看器脚本

这些 Lua 脚本用于在 AMem 中查看 UE4 游戏的 Level Actor 数据。

## 文件说明

### 1. ue4_actor_viewer.lua
带 ImGui 可视化界面的 Actor 查看器。

**功能特性:**
- ImGui 表格展示 Actor 列表
- 显示 Actor 地址、RootComponent、位置坐标
- 支持刷新数据
- 支持过滤（仅显示有位置的 Actor）
- 点击详情按钮查看单个 Actor 信息

**使用方法:**
1. 在 AMem GUI 中打开 Lua Script 窗口
2. 点击 "Open ImGui Window" 按钮创建 ImGui 窗口
3. 在 ImGui 窗口的脚本编辑区域加载脚本：
```lua
dofile("scripts/ue4_actor_viewer.lua")
```
4. 脚本会在 ImGui 窗口中显示 Actor 列表

**注意事项:**
- 此脚本必须在 Lua ImGui 窗口中运行，不能在普通 Lua 窗口中运行
- ImGui 函数必须在正确的帧作用域内调用
- 脚本会在每帧自动刷新显示

### 2. ue4_actor_list.lua
纯文本版本的 Actor 列表查看器。

**功能特性:**
- 在控制台输出 Actor 列表
- 显示前 50 个 Actor 的详细信息
- 统计有效 Actor 数量和有位置的 Actor 数量

**使用方法:**
```lua
-- 在 AMem Lua 窗口中执行
dofile("scripts/ue4_actor_list.lua")
```

## 配置说明

### 模块基址
脚本中的 `libUE4_base` 需要根据实际情况调整。可以通过以下方式获取：

1. 在 AMem 中使用 MCP 工具：
```
/mcp amem get_module_base libUE4.so
```

2. 或在 Lua 中查询模块列表，找到 libUE4.so 的基址

### 偏移定义
脚本使用的偏移来自 `GameOffsets.h`：

```lua
local GWorld_offset = 0xb036900          -- GWorld 在模块中的偏移
local UWorldToPersistentLevel = 0x30     -- UWorld -> PersistentLevel
local ULevelToAActors = 0x98             -- ULevel -> Actors 数组
local RootComponentOffset = 0x130        -- AActor -> RootComponent
local RelativeLocationOffset = 0x11C     -- USceneComponent -> RelativeLocation
```

如果游戏版本更新，这些偏移可能需要调整。

## 数据结构

脚本遍历的 UE4 数据结构链：

```
GWorld (全局变量)
  └─> UWorld (指针)
       └─> PersistentLevel (偏移 0x30)
            └─> Actors (TArray, 偏移 0x98)
                 └─> AActor[] (指针数组)
                      └─> RootComponent (偏移 0x130)
                           └─> RelativeLocation (偏移 0x11C)
                                └─> X, Y, Z (float * 3)
```

## 输出示例

### ue4_actor_list.lua 输出示例：
```
=== UE4 Actor 列表查看器 ===

GWorld 地址: 0x7237CA4900
GWorld 指针: 0x71EF5FD060
PersistentLevel 地址: 0x71ED6CA800

Actors 数组地址: 0x72075660A0
Actors 总数: 112

====================================================================================================
[  0] Actor: 0x0071E72C7E10 | RootComp: 0x000000000000
      位置: 无

[  1] Actor: 0x0071F33F7B80 | RootComp: 0x00723C56E010
      位置: X=-4508.39, Y= 2601.84, Z=  955.45

[  2] Actor: 0x0071F33F8080 | RootComp: 0x00723C566050
      位置: X=  396.14, Y= 2257.34, Z=  955.45
...
```

## 注意事项

1. **模块基址**: 每次游戏重启后，模块基址可能会变化，需要重新获取
2. **偏移稳定性**: 游戏更新可能导致偏移变化，需要重新分析
3. **性能**: 默认最多读取 200 个 Actor（可在脚本中调整 `maxDisplay`）
4. **内存安全**: 脚本会检查指针有效性，避免读取无效地址

## 扩展功能

可以基于这些脚本扩展更多功能：

- 添加 Actor 名称解析（需要 FName 系统）
- 添加类名显示（需要 UClass 解析）
- 添加 Actor 属性读取（HP、Team 等）
- 添加坐标筛选（距离、范围等）
- 添加实时更新（定时刷新）

## 相关文件

- `D:\UE4\UE4_Android\fps2\AndroidImgui\game\UE4_Game\GameOffsets.h` - 偏移定义
- `D:\UE4\UE4_Android\fps2\com.ShuiSha.FPS2\AIOHeader.hpp` - UE4 SDK 结构定义
