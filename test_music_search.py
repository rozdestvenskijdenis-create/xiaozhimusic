#!/usr/bin/env python3
"""
音乐搜索测试脚本
用于测试小智设备的音乐搜索功能
"""

import json

def create_music_search_message(keyword):
    """创建音乐搜索消息"""
    message = {
        "type": "music_search",
        "keyword": keyword
    }
    return message

def main():
    print("小智音乐搜索测试脚本")
    print("=" * 50)
    
    # 测试音乐搜索
    test_keywords = ["青花瓷", "海阔天空", "小星星", "曹操"]
    
    for keyword in test_keywords:
        message = create_music_search_message(keyword)
        print(f"=== 测试搜索: {keyword} ===")
        print("发送消息:", json.dumps(message, ensure_ascii=False, indent=2))
        print()
    
    print("测试消息已生成。请将这些消息发送到小智设备进行测试。")
    print("您可以通过以下方式发送消息：")
    print("1. 通过WebSocket连接")
    print("2. 通过MQTT消息")
    print("3. 通过串口调试")
    print()
    print("示例WebSocket消息格式：")
    print('{"type": "music_search", "keyword": "青花瓷"}')

if __name__ == "__main__":
    main()
