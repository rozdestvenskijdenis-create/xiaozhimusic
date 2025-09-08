# XiaoZhi Music Player | 小智音乐播放器

(English | [中文](README.md) | [日本語](README_ja.md))

## Video Demo

👉 [ESP32 XiaoZhi Playing URL Network Music](https://www.bilibili.com/video/BV1b3adzNErv/?spm_id_from=333.1387.list.card_archive.click&vd_source=1dde8303ebb914b487ac692a09cf5d69)

## Project Introduction

This is a music player project based on the open-source ESP32 project by Brother Xia, extending the original XiaoZhi AI chatbot functionality with new network music playback capabilities. The project is released under the MIT license, supporting free use and commercial purposes.

### Control Everything with MCP

As a voice interaction entry, the XiaoZhi AI chatbot leverages the AI capabilities of large models like Qwen / DeepSeek, and achieves multi-terminal control via the MCP protocol.

![Control everything via MCP](docs/mcp-based-graph.jpg)

### Core Features

#### Original Features
- Wi-Fi / ML307 Cat.1 4G network connectivity
- Offline voice wake-up [ESP-SR](https://github.com/espressif/esp-sr)
- Supports two communication protocols ([WebSocket](docs/websocket.md) or MQTT+UDP)
- Uses OPUS audio codec
- Voice interaction based on streaming ASR + LLM + TTS architecture
- Speaker recognition, identifies the current speaker [3D Speaker](https://github.com/modelscope/3D-Speaker)
- OLED / LCD display, supports emoji display
- Battery display and power management
- Multi-language support (Chinese, English, Japanese)
- Supports ESP32-C3, ESP32-S3, ESP32-P4 chip platforms

#### New Music Features
- 🎵 **Network Music Playback**: Integrated music API for online M4A audio streaming
- 🎶 **High-Quality Audio**: Supports M4A format for better audio quality experience
- 🔄 **Smart Resampling**: Automatically handles audio conversion between different sample rates
- 📱 **Voice Control**: Control music playback, pause, skip, and other operations through voice commands

## Hardware Requirements

### Recommended Hardware Configuration
- **Main Controller**: ESP32-S3 (recommended) or ESP32-C3, ESP32-P4
- **Display**: LCD_1.54_240x240 or compatible OLED/LCD screen
- **Audio Output**: I2S digital audio output, supports MAX98357A and other amplifier modules
- **Microphone**: Supports voice input and wake-up functionality
- **Network**: Wi-Fi module or 4G module

### Breadboard DIY Practice

See the Feishu document tutorial:

👉 ["XiaoZhi AI Chatbot Encyclopedia"](https://ccnphfhqs21z.feishu.cn/wiki/F5krwD16viZoF0kKkvDcrZNYnhb?from=from_copylink)

Breadboard demo:

![Breadboard Demo](docs/v1/wiring2.jpg)

### Supports 70+ Open Source Hardware (Partial List)

- <a href="https://oshwhub.com/li-chuang-kai-fa-ban/li-chuang-shi-zhan-pai-esp32-s3-kai-fa-ban" target="_blank" title="LiChuang ESP32-S3 Development Board">LiChuang ESP32-S3 Development Board</a>
- <a href="https://github.com/espressif/esp-box" target="_blank" title="Espressif ESP32-S3-BOX3">Espressif ESP32-S3-BOX3</a>
- <a href="https://docs.m5stack.com/zh_CN/core/CoreS3" target="_blank" title="M5Stack CoreS3">M5Stack CoreS3</a>
- <a href="https://docs.m5stack.com/en/atom/Atomic%20Echo%20Base" target="_blank" title="AtomS3R + Echo Base">M5Stack AtomS3R + Echo Base</a>
- <a href="https://gf.bilibili.com/item/detail/1108782064" target="_blank" title="Magic Button 2.4">Magic Button 2.4</a>
- <a href="https://www.waveshare.net/shop/ESP32-S3-Touch-AMOLED-1.8.htm" target="_blank" title="Waveshare ESP32-S3-Touch-AMOLED-1.8">Waveshare ESP32-S3-Touch-AMOLED-1.8</a>
- <a href="https://github.com/Xinyuan-LilyGO/T-Circle-S3" target="_blank" title="LILYGO T-Circle-S3">LILYGO T-Circle-S3</a>
- <a href="https://oshwhub.com/tenclass01/xmini_c3" target="_blank" title="XiaGe Mini C3">XiaGe Mini C3</a>
- <a href="https://oshwhub.com/movecall/cuican-ai-pendant-lights-up-y" target="_blank" title="Movecall CuiCan ESP32S3">CuiCan AI Pendant</a>
- <a href="https://github.com/WMnologo/xingzhi-ai" target="_blank" title="WMnologo-Xingzhi-1.54">WMnologo-Xingzhi-1.54TFT</a>
- <a href="https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html" target="_blank" title="SenseCAP Watcher">SenseCAP Watcher</a>
- <a href="https://www.bilibili.com/video/BV1BHJtz6E2S/" target="_blank" title="ESP-HI Low Cost Robot Dog">ESP-HI Low Cost Robot Dog</a>

<div style="display: flex; justify-content: space-between;">
  <a href="docs/v1/lichuang-s3.jpg" target="_blank" title="LiChuang ESP32-S3 Development Board">
    <img src="docs/v1/lichuang-s3.jpg" width="240" />
  </a>
  <a href="docs/v1/espbox3.jpg" target="_blank" title="Espressif ESP32-S3-BOX3">
    <img src="docs/v1/espbox3.jpg" width="240" />
  </a>
  <a href="docs/v1/m5cores3.jpg" target="_blank" title="M5Stack CoreS3">
    <img src="docs/v1/m5cores3.jpg" width="240" />
  </a>
  <a href="docs/v1/atoms3r.jpg" target="_blank" title="AtomS3R + Echo Base">
    <img src="docs/v1/atoms3r.jpg" width="240" />
  </a>
  <a href="docs/v1/magiclick.jpg" target="_blank" title="Magic Button 2.4">
    <img src="docs/v1/magiclick.jpg" width="240" />
  </a>
  <a href="docs/v1/waveshare.jpg" target="_blank" title="Waveshare ESP32-S3-Touch-AMOLED-1.8">
    <img src="docs/v1/waveshare.jpg" width="240" />
  </a>
  <a href="docs/v1/lilygo-t-circle-s3.jpg" target="_blank" title="LILYGO T-Circle-S3">
    <img src="docs/v1/lilygo-t-circle-s3.jpg" width="240" />
  </a>
  <a href="docs/v1/xmini-c3.jpg" target="_blank" title="XiaGe Mini C3">
    <img src="docs/v1/xmini-c3.jpg" width="240" />
  </a>
  <a href="docs/v1/movecall-cuican-esp32s3.jpg" target="_blank" title="CuiCan">
    <img src="docs/v1/movecall-cuican-esp32s3.jpg" width="240" />
  </a>
  <a href="docs/v1/wmnologo_xingzhi_1.54.jpg" target="_blank" title="WMnologo-Xingzhi-1.54">
    <img src="docs/v1/wmnologo_xingzhi_1.54.jpg" width="240" />
  </a>
  <a href="docs/v1/sensecap_watcher.jpg" target="_blank" title="SenseCAP Watcher">
    <img src="docs/v1/sensecap_watcher.jpg" width="240" />
  </a>
  <a href="docs/v1/esp-hi.jpg" target="_blank" title="ESP-HI Low Cost Robot Dog">
    <img src="docs/v1/esp-hi.jpg" width="240" />
  </a>
</div>

## Software Implementation

### Audio Processing Architecture

#### M4A Decoding Process
1. **Network Retrieval**: Get M4A audio stream through HTTP requests
2. **Decoding Processing**: Use ESP-ADF library's M4A decoder for audio decoding
3. **Resampling**: Convert 44100Hz stereo to 22050Hz mono
4. **Output Playback**: Output to audio device through I2S interface

#### Technical Details
- **Original Sample Rate**: M4A files are typically 44100Hz stereo
- **Target Sample Rate**: Device outputs 24000Hz, converted to 22050Hz mono
- **Resampling Algorithm**: Uses high-quality resampling algorithm to ensure audio quality
- **Buffer Management**: Adopts producer-consumer model for parallel decoding and playback processing

### API Interface

#### Music API
- **API Address**: https://doc.vkeys.cn/api-doc/v2/
- **Quality Selection**: Uses QQ Music URL quality 2 (lossy quality, balancing quality and bandwidth)
- **Format Support**: Primarily supports M4A format for better compatibility

#### MCP Protocol Extension
- **Device-side MCP**: Controls audio playback, volume adjustment, playback status, etc.
- **Cloud-side MCP**: Extends large model capabilities, supports music search, recommendations, etc.

## Development Environment

### Environment Requirements
- **IDE**: Cursor or VSCode
- **SDK**: ESP-IDF 5.4 or higher
- **System**: Linux recommended for faster compilation and fewer driver issues
- **Code Style**: Follow Google C++ code style

### Quick Start

#### 1. Clone Project
```bash
git clone https://github.com/your-repo/xiaozhiMusic.git
cd xiaozhiMusic
```

#### 2. Setup Environment
```bash
# Install ESP-IDF
# Refer to official documentation: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/

# Set environment variables
. $HOME/esp/esp-idf/export.sh
```

#### 3. Build and Flash
```bash
# Configure project
idf.py menuconfig

# Build
idf.py build

# Flash
idf.py flash monitor
```

### Configuration Guide

#### Network Configuration
- Configure Wi-Fi connection information
- Set music API access parameters
- Configure MCP server address

#### Audio Configuration
- Set I2S audio output parameters
- Configure resampling parameters
- Adjust audio buffer size

### Firmware Flashing

For beginners, it is recommended to use the firmware that can be flashed without setting up a development environment.

The firmware connects to the official [xiaozhi.me](https://xiaozhi.me) server by default. Personal users can register an account to use the Qwen real-time model for free.

👉 [Beginner's Firmware Flashing Guide](https://ccnphfhqs21z.feishu.cn/wiki/Zpz4wXBtdimBrLk25WdcXzxcnNS)

### Developer Documentation

- [Custom Board Guide](main/boards/README.md) - Learn how to create custom boards for XiaoZhi AI
- [MCP Protocol IoT Control Usage](docs/mcp-usage.md) - Learn how to control IoT devices via MCP protocol
- [MCP Protocol Interaction Flow](docs/mcp-protocol.md) - Device-side MCP protocol implementation
- [A detailed WebSocket communication protocol document](docs/websocket.md)

## Usage Guide

### Voice Control Commands
- "Play music" - Start playing music
- "Pause music" - Pause current playback
- "Next song" - Skip to next song
- "Previous song" - Go to previous song
- "Adjust volume" - Adjust playback volume

### Music Search
- Search by song name
- Search by artist name
- Search by album name

## Troubleshooting

### Common Issues

#### Audio Playback Issues
- **No sound**: Check I2S connection and audio device configuration
- **Poor audio quality**: Check network connection and resampling settings
- **Playback stuttering**: Adjust audio buffer size and network timeout settings

#### Network Connection Issues
- **Cannot connect to Wi-Fi**: Check SSID and password configuration
- **API access failed**: Check network connection and API address configuration
- **Slow music loading**: Check network bandwidth and server response

## Large Model Configuration

If you already have a XiaoZhi AI chatbot device and have connected to the official server, you can log in to the [xiaozhi.me](https://xiaozhi.me) console for configuration.

👉 [Backend Operation Video Tutorial (Old Interface)](https://www.bilibili.com/video/BV1jUCUY2EKM/)

## Contributing

Welcome to submit Issues and Pull Requests!

### Development Guidelines
- Follow Google C++ code style
- Add appropriate comments and documentation
- Ensure code passes all tests
- Update relevant documentation

### Contact Information
- **QQ Group**: 1011329060
- **Issues**: Submit issues on GitHub
- **Discussions**: Welcome to discuss in Discussions

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Thanks to Brother Xia for the open-source XiaoZhi AI chatbot project
- Thanks to the ESP-IDF and ESP-ADF development teams
- Thanks to all contributors and users for their support

## Related Open Source Projects

### Server-side Projects
- [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) Python server
- [joey-zhou/xiaozhi-esp32-server-java](https://github.com/joey-zhou/xiaozhi-esp32-server-java) Java server
- [AnimeAIChat/xiaozhi-server-go](https://github.com/AnimeAIChat/xiaozhi-server-go) Golang server

### Client Projects
- [huangjunsen0406/py-xiaozhi](https://github.com/huangjunsen0406/py-xiaozhi) Python client
- [TOM88812/xiaozhi-android-client](https://github.com/TOM88812/xiaozhi-android-client) Android client

## Star History

<a href="https://star-history.com/#78/xiaozhi-esp32&Date">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=78/xiaozhi-esp32&type=Date&theme=dark" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=78/xiaozhi-esp32&type=Date" />
   <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=78/xiaozhi-esp32&type=Date" />
 </picture>
</a> 
