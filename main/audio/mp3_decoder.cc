#include "mp3_decoder.h"
#include <esp_log.h>
#include <cstring>

const char* Mp3Decoder::TAG = "Mp3Decoder";

Mp3Decoder::Mp3Decoder() 
    : initialized_(false)
    , sample_rate_(44100)
    , channels_(2) {
}

Mp3Decoder::~Mp3Decoder() {
    Deinitialize();
}

bool Mp3Decoder::Initialize(int sample_rate, int channels) {
    if (initialized_) {
        ESP_LOGW(TAG, "MP3 decoder already initialized");
        return true;
    }
    
    sample_rate_ = sample_rate;
    channels_ = channels;
    
    ESP_LOGI(TAG, "Initializing MP3 decoder: sample_rate=%d, channels=%d", sample_rate_, channels_);
    
    // 清空缓冲区
    mp3_buffer_.clear();
    
    initialized_ = true;
    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
}

void Mp3Decoder::Deinitialize() {
    if (!initialized_) {
        return;
    }
    
    ESP_LOGI(TAG, "Deinitializing MP3 decoder");
    
    mp3_buffer_.clear();
    initialized_ = false;
    
    ESP_LOGI(TAG, "MP3 decoder deinitialized");
}

std::vector<int16_t> Mp3Decoder::DecodeChunk(const std::vector<uint8_t>& mp3_data) {
    std::vector<int16_t> pcm_data;
    
    if (!initialized_) {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
        return pcm_data;
    }
    
    if (mp3_data.empty()) {
        return pcm_data;
    }
    
    // 将新的MP3数据添加到缓冲区
    mp3_buffer_.insert(mp3_buffer_.end(), mp3_data.begin(), mp3_data.end());
    
    // 简单的占位符实现
    // 注意：这是一个简化的实现，实际项目中应该使用真正的MP3解码库
    // 这里只是返回一些静音数据作为占位符
    
    // 计算应该输出的PCM样本数（假设每帧1152个样本）
    size_t frame_size = 1152 * channels_;
    
    // 只有当缓冲区有足够数据时才进行解码，避免生成过多数据
    if (mp3_buffer_.size() >= 2048) { // 增大阈值，减少解码频率
        // 生成静音数据作为占位符
        pcm_data.resize(frame_size, 0);
        
        // 移除已处理的数据（简化处理）
        mp3_buffer_.erase(mp3_buffer_.begin(), mp3_buffer_.begin() + 1024);
        
        ESP_LOGD(TAG, "MP3 decode placeholder: %zu bytes -> %zu PCM samples", 
                 mp3_data.size(), pcm_data.size());
    }
    
    return pcm_data;
}
