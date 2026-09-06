# 小智音乐播放器 | XiaoZhi Music Player

（中文 | [English](README_en.md) | [日本語](README_ja.md)）

## 视频演示

👉 [ESP32小智播放URL网络音乐](https://www.bilibili.com/video/BV1b3adzNErv/?spm_id_from=333.1387.list.card_archive.click&vd_source=1dde8303ebb914b487ac692a09cf5d69)

## 项目介绍

这是一个基于虾哥开源 ESP32 项目扩展的音乐播放器项目，在原有小智 AI 聊天机器人功能基础上，新增了网络音乐播放功能。项目采用 MIT 许可证，支持免费使用和商业用途。

### 基于 MCP 控制万物

小智 AI 聊天机器人作为一个语音交互入口，利用 Qwen / DeepSeek 等大模型的 AI 能力，通过 MCP 协议实现多端控制。

![通过MCP控制万物](docs/mcp-based-graph.jpg)

### 核心功能

#### 原有功能
- Wi-Fi / ML307 Cat.1 4G 网络连接
- 离线语音唤醒 [ESP-SR](https://github.com/espressif/esp-sr)
- 支持两种通信协议（[WebSocket](docs/websocket.md) 或 MQTT+UDP）
- 使用 OPUS 音频编解码器
- 基于流式 ASR + LLM + TTS 架构的语音交互
- 说话人识别，识别当前说话人 [3D Speaker](https://github.com/modelscope/3D-Speaker)
- OLED / LCD 显示，支持表情显示
- 电池显示和电源管理
- 多语言支持（中文、英文、日文）
- 支持 ESP32-C3、ESP32-S3、ESP32-P4 芯片平台

#### 新增音乐功能
- 🎵 **网络音乐播放**：接入音乐 API，实现网络 M4A 音频的在线播放
- 🎶 **高质量音频**：支持 M4A 格式，提供更好的音质体验
- 🔄 **智能重采样**：自动处理不同采样率的音频转换
- 📱 **语音控制**：通过语音指令控制音乐播放、暂停、切换等操作

## 硬件要求

### 推荐硬件配置
- **主控芯片**：ESP32-S3（推荐）或 ESP32-C3、ESP32-P4
- **显示屏**：LCD_1.54_240x240 或兼容的 OLED/LCD 屏幕
- **音频输出**：I2S 数字音频输出，支持 MAX98357A 等功放模块
- **麦克风**：支持语音输入和唤醒功能
- **网络**：Wi-Fi 模块或 4G 模块

### 面包板 DIY 实践

参考飞书文档教程：

👉 ["小智AI聊天机器人百科全书"](https://ccnphfhqs21z.feishu.cn/wiki/F5krwD16viZoF0kKkvDcrZNYnhb?from=from_copylink)

## 软件实现

### 音频处理架构

#### M4A 解码流程
1. **网络获取**：通过 HTTP 请求获取 M4A 音频流
2. **解码处理**：使用 ESP-ADF 库的 M4A 解码器进行音频解码
3. **重采样**：将 44100Hz 立体声转换为 22050Hz 单声道
4. **输出播放**：通过 I2S 接口输出到音频设备

#### 技术细节
- **原始采样率**：M4A 文件通常为 44100Hz 立体声
- **目标采样率**：设备输出 24000Hz，转换为单声道后为 22050Hz
- **重采样算法**：使用高质量重采样算法确保音质
- **缓冲管理**：采用生产者-消费者模型，并行处理解码和播放

### API 接口

#### 音乐 API
- **API 地址**：https://doc.vkeys.cn/api-doc/v2/
- **音质选择**：使用 QQ 音乐 URL 音质 2（有损音质，平衡质量和带宽）
- **格式支持**：主要支持 M4A 格式，兼容性更好

#### MCP 协议扩展
- **设备端 MCP**：控制音频播放、音量调节、播放状态等
- **云端 MCP**：扩展大模型能力，支持音乐搜索、推荐等功能

## 开发环境

### 环境要求
- **IDE**：Cursor 或 VSCode
- **SDK**：ESP-IDF 5.4 或更高版本
- **系统**：推荐 Linux，编译速度更快，驱动问题更少
- **代码风格**：遵循 Google C++ 代码风格

### 快速开始

#### 1. 克隆项目
```bash
git clone https://github.com/your-repo/xiaozhiMusic.git
cd xiaozhiMusic
```

#### 2. 配置环境
```bash
# 安装 ESP-IDF
# 参考官方文档：https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/get-started/

# 设置环境变量
. $HOME/esp/esp-idf/export.sh
```

#### 3. 编译和烧录
```bash
# 配置项目
idf.py menuconfig

# 编译
idf.py build

# 烧录
idf.py flash monitor
```

### 配置说明

#### 网络配置
- 配置 Wi-Fi 连接信息
- 设置音乐 API 访问参数
- 配置 MCP 服务器地址

#### 音频配置
- 设置 I2S 音频输出参数
- 配置重采样参数
- 调整音频缓冲大小

## 使用指南

### 语音控制命令
- "播放音乐" - 开始播放音乐
- "暂停音乐" - 暂停当前播放
- "下一首" - 切换到下一首歌曲
- "上一首" - 切换到上一首歌曲
- "调节音量" - 调整播放音量

### 音乐搜索
- 支持通过歌曲名称搜索
- 支持通过歌手名称搜索
- 支持通过专辑名称搜索

## 故障排除

### 常见问题

#### 音频播放问题
- **无声音**：检查 I2S 连接和音频设备配置
- **音质差**：检查网络连接和重采样设置
- **播放卡顿**：调整音频缓冲大小和网络超时设置

#### 网络连接问题
- **无法连接 Wi-Fi**：检查 SSID 和密码配置
- **API 访问失败**：检查网络连接和 API 地址配置
- **音乐加载慢**：检查网络带宽和服务器响应

## 贡献指南

欢迎提交 Issue 和 Pull Request！

### 开发规范
- 遵循 Google C++ 代码风格
- 添加适当的注释和文档
- 确保代码通过所有测试
- 更新相关文档

### 联系方式
- **QQ 群**：1011329060
- **Issues**：在 GitHub 上提交问题
- **讨论**：欢迎在 Discussions 中交流

## 许可证

本项目采用 MIT 许可证，详见 [LICENSE](LICENSE) 文件。

## 致谢

- 感谢虾哥开源的小智 AI 聊天机器人项目
- 感谢 ESP-IDF 和 ESP-ADF 开发团队
- 感谢所有贡献者和用户的支持

## 相关项目

### 服务器端项目
- [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) Python 服务器
- [joey-zhou/xiaozhi-esp32-server-java](https://github.com/joey-zhou/xiaozhi-esp32-server-java) Java 服务器
- [AnimeAIChat/xiaozhi-server-go](https://github.com/AnimeAIChat/xiaozhi-server-go) Golang 服务器

### 客户端项目
- [huangjunsen0406/py-xiaozhi](https://github.com/huangjunsen0406/py-xiaozhi) Python 客户端
- [TOM88812/xiaozhi-android-client](https://github.com/TOM88812/xiaozhi-android-client) Android 客户端客户端


