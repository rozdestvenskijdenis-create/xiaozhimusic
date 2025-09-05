#include "mp3_decoder.h"
#include <esp_log.h>
#include <cstring>

// ESP-ADF includes
#include "http_stream.h"
#include "i2s_stream.h"

const char* Mp3Decoder::TAG = "Mp3Decoder";

Mp3Decoder::Mp3Decoder() 
    : initialized_(false)
    , sample_rate_(44100)
    , channels_(2)
    , pipeline_(nullptr)
    , mp3_decoder_(nullptr)
    , http_stream_(nullptr)
    , i2s_stream_(nullptr) {
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
    
    ESP_LOGI(TAG, "Initializing ESP-ADF MP3 decoder pipeline: sample_rate=%d, channels=%d", sample_rate_, channels_);
    
    // 创建音频管道
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline_ = audio_pipeline_init(&pipeline_cfg);
    if (pipeline_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize audio pipeline");
        return false;
    }
    
    // 创建HTTP流（用于接收MP3数据）
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_stream_ = http_stream_init(&http_cfg);
    if (http_stream_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize HTTP stream");
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    
    // 创建MP3解码器
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_cfg.stack_in_ext = false;  // 禁用外部内存分配，避免FreeRTOS补丁问题
    mp3_cfg.task_core = 1;         // 使用核心1，避免与主任务冲突
    mp3_cfg.task_prio = 4;         // 降低任务优先级
    mp3_decoder_ = mp3_decoder_init(&mp3_cfg);
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        audio_element_deinit(http_stream_);
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    
    // 创建I2S流（用于输出PCM数据）
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    // 在ESP-IDF 5.x中，配置在std_cfg中
    i2s_cfg.std_cfg.clk_cfg.sample_rate_hz = sample_rate_;
    i2s_cfg.std_cfg.slot_cfg.slot_mode = (channels_ == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
    i2s_stream_ = i2s_stream_init(&i2s_cfg);
    if (i2s_stream_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize I2S stream");
        audio_element_deinit(mp3_decoder_);
        audio_element_deinit(http_stream_);
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    
    // 注册所有元素到管道
    audio_pipeline_register(pipeline_, http_stream_, "http");
    audio_pipeline_register(pipeline_, mp3_decoder_, "mp3");
    audio_pipeline_register(pipeline_, i2s_stream_, "i2s");
    
    // 链接管道元素：http -> mp3 -> i2s
    const char *link_tag[3] = {"http", "mp3", "i2s"};
    if (audio_pipeline_link(pipeline_, &link_tag[0], 3) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to link audio pipeline elements");
        audio_element_deinit(i2s_stream_);
        audio_element_deinit(mp3_decoder_);
        audio_element_deinit(http_stream_);
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    
    // 注意：不在这里启动管道，而是在播放时启动
    // 这样可以避免I2S冲突，只在需要时占用I2S资源
    
    // 清空缓冲区
    mp3_buffer_.clear();
    output_buffer_.clear();
    
    initialized_ = true;
    ESP_LOGI(TAG, "ESP-ADF MP3 decoder pipeline initialized successfully");
    return true;
}

bool Mp3Decoder::PlayUrl(const std::string& url) {
    if (!initialized_) {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
        return false;
    }
    
    ESP_LOGI(TAG, "Starting MP3 playback from URL: %s", url.c_str());
    
    // 设置HTTP流的URL
    if (audio_element_set_uri(http_stream_, url.c_str()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set URI for HTTP stream");
        return false;
    }
    
    // 启动音频管道
    if (audio_pipeline_run(pipeline_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio pipeline");
        return false;
    }
    
    ESP_LOGI(TAG, "MP3 playback started successfully");
    return true;
}

void Mp3Decoder::Stop() {
    if (!initialized_) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping MP3 playback");
    
    // 停止音频管道
    if (audio_pipeline_stop(pipeline_) == ESP_OK) {
        audio_pipeline_wait_for_stop(pipeline_);
    }
    
    ESP_LOGI(TAG, "MP3 playback stopped");
}

void Mp3Decoder::Deinitialize() {
    if (!initialized_) {
        return;
    }
    
    ESP_LOGI(TAG, "Deinitializing ESP-ADF MP3 decoder pipeline");
    
    // 停止管道
    if (pipeline_) {
        audio_pipeline_stop(pipeline_);
        audio_pipeline_wait_for_stop(pipeline_);
        audio_pipeline_terminate(pipeline_);
        
        // 注销所有元素
        audio_pipeline_unregister(pipeline_, http_stream_);
        audio_pipeline_unregister(pipeline_, mp3_decoder_);
        audio_pipeline_unregister(pipeline_, i2s_stream_);
        
        // 销毁所有元素
        if (i2s_stream_) {
            audio_element_deinit(i2s_stream_);
            i2s_stream_ = nullptr;
        }
        if (mp3_decoder_) {
            audio_element_deinit(mp3_decoder_);
            mp3_decoder_ = nullptr;
        }
        if (http_stream_) {
            audio_element_deinit(http_stream_);
            http_stream_ = nullptr;
        }
        
        // 销毁管道
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
    }
    
    mp3_buffer_.clear();
    output_buffer_.clear();
    initialized_ = false;
    
    ESP_LOGI(TAG, "ESP-ADF MP3 decoder pipeline deinitialized");
}

std::vector<int16_t> Mp3Decoder::DecodeChunk(const std::vector<uint8_t>& mp3_data) {
    std::vector<int16_t> pcm_data;
    
    // 注意：由于我们现在使用完整的音频管道（HTTP -> MP3 -> I2S），
    // DecodeChunk方法不再需要。数据会通过HTTP流自动流入，
    // 通过MP3解码器自动解码，通过I2S流自动输出到硬件。
    // 这个方法保留用于兼容性，但实际不会使用。
    
    ESP_LOGD(TAG, "DecodeChunk called but using direct pipeline playback - returning empty data");
    
    return pcm_data;
}
