#!/usr/bin/env python3
"""AMem MCP Server 启动脚本 (兼容入口).

为兼容历史配置保留此文件。推荐使用以下任一方式启动:

    amem-mcp                        # 已 pip install 后
    python -m amem_mcp              # 模块方式
    python server.py                # 旧配置兼容（当前文件）

所有逻辑已迁移到 `amem_mcp/` 包。
"""

from __future__ import annotations

import os
import sys

# 允许从任何 cwd 直接运行 `python <path>/server.py`
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

from amem_mcp.app import main  # noqa: E402


if __name__ == "__main__":
    main()
