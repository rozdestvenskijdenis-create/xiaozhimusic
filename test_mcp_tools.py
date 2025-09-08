#!/usr/bin/env python3
"""
MCP工具测试脚本
用于测试小智设备的MCP工具调用功能
"""

import json
import time

def create_mcp_message(method, params=None, id=1):
    """创建MCP消息"""
    message = {
        "jsonrpc": "2.0",
        "id": id,
        "method": method
    }
    if params:
        message["params"] = params
    return message

def test_tools_list():
    """测试获取工具列表"""
    message = create_mcp_message("tools/list")
    print("=== 测试获取工具列表 ===")
    print("发送消息:", json.dumps(message, ensure_ascii=False, indent=2))
    print()
    return message

def test_tool_call(tool_name, arguments=None):
    """测试调用工具"""
    params = {"name": tool_name}
    if arguments:
        params["arguments"] = arguments
    
    message = create_mcp_message("tools/call", params)
    print(f"=== 测试调用工具: {tool_name} ===")
    print("发送消息:", json.dumps(message, ensure_ascii=False, indent=2))
    print()
    return message

def main():
    print("小智MCP工具测试脚本")
    print("=" * 50)
    
    # 测试1: 获取工具列表
    test_tools_list()
    
    # 测试2: 调用简单测试工具
    test_tool_call("test_simple")
    
    # 测试3: 调用音乐测试工具
    test_tool_call("test_music_playback")
    
    # 测试4: 调用音乐搜索工具
    test_tool_call("test_search_music", {
        "keyword": "小星星"
    })
    
    # 测试5: 调用音乐控制工具
    test_tool_call("control_music", {
        "action": "stop"
    })
    
    print("测试消息已生成。请将这些消息发送到小智设备进行测试。")
    print("您可以通过以下方式发送消息：")
    print("1. 通过WebSocket连接")
    print("2. 通过MQTT消息")
    print("3. 通过串口调试")

if __name__ == "__main__":
    main()
