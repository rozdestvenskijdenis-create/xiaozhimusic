#include "m4a_decoder.h"
#include <esp_log.h>
#include "opus_resampler.h"
#include <cstring>

// ESP-ADF includes
#include "http_stream.h"
#include "i2s_stream.h"
#include "codec/aac_decoder.h"

const char* M4aDecoder::TAG = "M4aDecoder";

M4aDecoder::M4aDecoder() 
    : initialized_(false)
    , sample_rate_(44100)
    , channels_(2)
    , pipeline_(nullptr)
    , m4a_decoder_(nullptr)
    , http_stream_(nullptr)
    , i2s_stream_(nullptr) {
}

M4aDecoder::~M4aDecoder() {
    Deinitialize();
}

bool M4aDecoder::Initialize(int sample_rate, int channels) {
    if (initialized_) {
        ESP_LOGW(TAG, "M4A decoder already initialized");
        return true;
    }
    
    sample_rate_ = sample_rate;
    channels_ = channels;
    
    ESP_LOGI(TAG, "Initializing ESP-ADF M4A decoder pipeline: sample_rate=%d, channels=%d", sample_rate_, channels_);
    
    // 创建音频管道
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline_ = audio_pipeline_init(&pipeline_cfg);
    if (pipeline_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize audio pipeline");
        return false;
    }
    
    // 创建HTTP流（用于接收M4A数据）
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_stream_ = http_stream_init(&http_cfg);
    if (http_stream_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize HTTP stream");
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    
    // 创建M4A解码器 - 使用AAC解码器，让解码器自动检测采样率
    aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
    // DEFAULT_AAC_DECODER_CONFIG()已经配置为自动检测采样率和通道数
    m4a_decoder_ = aac_decoder_init(&aac_cfg);
    if (m4a_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize M4A decoder");
        audio_element_deinit(http_stream_);
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    
    // 创建一个输出环形缓冲区来存储M4A解码后的PCM数据
    output_ringbuf_ = rb_create(16384, 1);  // 16KB缓冲区，提高44.1kHz音频的缓冲能力
    if (!output_ringbuf_) {
        ESP_LOGE(TAG, "Failed to create output ring buffer");
        audio_element_deinit(m4a_decoder_);
        audio_element_deinit(http_stream_);
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    
    // 将输出缓冲区连接到M4A解码器
    audio_element_set_output_ringbuf(m4a_decoder_, output_ringbuf_);
    
    i2s_stream_ = nullptr;
    ESP_LOGI(TAG, "Created output buffer for M4A decoder (no I2S stream)");
    
    // 注册元素到管道（不包含I2S流）
    audio_pipeline_register(pipeline_, http_stream_, "http");
    audio_pipeline_register(pipeline_, m4a_decoder_, "m4a");
    
    // 链接管道元素：http -> m4a（不包含I2S）
    const char *link_tag[2] = {"http", "m4a"};
    if (audio_pipeline_link(pipeline_, &link_tag[0], 2) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to link audio pipeline elements");
        audio_element_deinit(m4a_decoder_);
        audio_element_deinit(http_stream_);
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    
    // 清空缓冲区
    m4a_buffer_.clear();
    output_buffer_.clear();
    
    initialized_ = true;
    ESP_LOGI(TAG, "ESP-ADF M4A decoder pipeline initialized successfully");
    ESP_LOGI(TAG, "Pipeline: %p, HTTP stream: %p, M4A decoder: %p, I2S stream: %p", 
             pipeline_, http_stream_, m4a_decoder_, i2s_stream_);
    return true;
}

bool M4aDecoder::PlayUrl(const std::string& url) {
    if (!initialized_) {
        ESP_LOGE(TAG, "M4A decoder not initialized");
        return false;
    }
    
    if (!http_stream_) {
        ESP_LOGE(TAG, "HTTP stream is null");
        return false;
    }
    
    if (!pipeline_) {
        ESP_LOGE(TAG, "Audio pipeline is null");
        return false;
    }
    
    ESP_LOGI(TAG, "Starting M4A playback from URL: %s", url.c_str());
    
    // 设置HTTP流的URL
    esp_err_t ret = audio_element_set_uri(http_stream_, url.c_str());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set URI for HTTP stream, error: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "URI set successfully, starting pipeline");
    
    // 启动音频管道（只包含HTTP和M4A解码器）
    ret = audio_pipeline_run(pipeline_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio pipeline, error: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "M4A playback started successfully (without I2S output)");
    ESP_LOGI(TAG, "Note: PCM data will be available through DecodeChunk method");
    // 等待更长时间让M4A解码器完全启动并开始处理数据
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    // 暂时禁用重采样器，直接使用M4A的原始采样率
    ESP_LOGI(TAG, "Skipping resampler setup for now, using M4A original sample rate");
    resampler_.reset();
    
    return true;
}

void M4aDecoder::Stop() {
    if (!initialized_) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping M4A playback");
    
    // 停止音频管道
    if (audio_pipeline_stop(pipeline_) == ESP_OK) {
        audio_pipeline_wait_for_stop(pipeline_);
    }
    
    ESP_LOGI(TAG, "M4A playback stopped");
}

void M4aDecoder::Deinitialize() {
    if (!initialized_) {
        return;
    }
    
    ESP_LOGI(TAG, "Deinitializing ESP-ADF M4A decoder pipeline");
    
    // 停止管道
    if (pipeline_) {
        audio_pipeline_stop(pipeline_);
        audio_pipeline_wait_for_stop(pipeline_);
        audio_pipeline_terminate(pipeline_);
        
        // 注销所有元素（不包含I2S流）
        audio_pipeline_unregister(pipeline_, http_stream_);
        audio_pipeline_unregister(pipeline_, m4a_decoder_);
        
        // 销毁所有元素（不包含I2S流）
        if (m4a_decoder_) {
            audio_element_deinit(m4a_decoder_);
            m4a_decoder_ = nullptr;
        }
        if (http_stream_) {
            audio_element_deinit(http_stream_);
            http_stream_ = nullptr;
        }
        if (output_ringbuf_) {
            rb_destroy(output_ringbuf_);
            output_ringbuf_ = nullptr;
        }
        
        // 销毁管道
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
    }
    
    m4a_buffer_.clear();
    output_buffer_.clear();
    initialized_ = false;
    
    ESP_LOGI(TAG, "ESP-ADF M4A decoder pipeline deinitialized");
}

std::vector<int16_t> M4aDecoder::DecodeChunk(const std::vector<uint8_t>& m4a_data) {
    std::vector<int16_t> pcm_data;
    
    if (!initialized_ || !m4a_decoder_) {
        ESP_LOGE(TAG, "M4A decoder not initialized");
        return pcm_data;
    }
    
    if (m4a_data.empty()) {
        return pcm_data;
    }
    
    ESP_LOGI(TAG, "DecodeChunk called with %d bytes of M4A data", m4a_data.size());
    
    ringbuf_handle_t output_rb = audio_element_get_output_ringbuf(m4a_decoder_);
    if (!output_rb) {
        ESP_LOGI(TAG, "Failed to get output ringbuf from M4A decoder");
        return pcm_data;
    }
    
    char output_buffer[4096];
    int bytes_read = rb_read(output_rb, output_buffer, sizeof(output_buffer), 0); // 非阻塞读取
    if (bytes_read > 0) {
        // 将字节数据转换为int16_t PCM数据
        int16_t* pcm_samples = reinterpret_cast<int16_t*>(output_buffer);
        int sample_count = bytes_read / sizeof(int16_t);
        
        pcm_data.resize(sample_count);
        std::memcpy(pcm_data.data(), pcm_samples, bytes_read);
        
        ESP_LOGI(TAG, "M4A decode: got %d PCM samples from decoder", pcm_data.size());
    } else {
        ESP_LOGI(TAG, "M4A decode: no PCM data available (bytes_read=%d)", bytes_read);
    }
    return pcm_data;
}

// 新增方法：从M4A解码器获取PCM数据
std::vector<int16_t> M4aDecoder::GetPcmData() {
    std::vector<int16_t> pcm_data;
    
    if (!initialized_ || !output_ringbuf_) {
        return pcm_data;
    }
    
    // 从我们创建的输出缓冲区读取PCM数据，控制数据量
    char output_buffer[4096];  // 增加缓冲区大小，提高数据流
    int bytes_read = rb_read(output_ringbuf_, output_buffer, sizeof(output_buffer), 0); // 非阻塞读取
    if (bytes_read > 0) {
        // 将字节数据转换为int16_t PCM数据
        int16_t* pcm_samples = reinterpret_cast<int16_t*>(output_buffer);
        int sample_count = bytes_read / sizeof(int16_t);
        
        // 安全检查，避免过大的内存分配，平衡样本数量
        if (sample_count > 0 && sample_count < 3000) {  // 进一步增加样本限制，适应实际数据量
            std::vector<int16_t> raw_pcm_data(sample_count);
            std::memcpy(raw_pcm_data.data(), pcm_samples, bytes_read);
            
            // 获取AAC解码器的实际输出采样率
            int actual_sample_rate = 44100;  // 默认值
            int actual_channels = 2;         // 默认值
            
            // 尝试从AAC解码器获取实际的采样率信息
            if (m4a_decoder_) {
                // 从AAC解码器获取音频信息
                audio_element_info_t info;
                if (audio_element_getinfo(m4a_decoder_, &info) == ESP_OK) {
                    actual_sample_rate = info.sample_rates;
                    actual_channels = info.channels;
                    ESP_LOGI(TAG, "AAC decoder actual output: %d Hz, %d channels", actual_sample_rate, actual_channels);
                } else {
                    ESP_LOGW(TAG, "Failed to get AAC decoder info, using defaults");
                }
            }
            const int target_sample_rate = 24000; // 音频的实际输出速度，由系统决定
            
            if (actual_sample_rate != target_sample_rate) {
                // 计算重采样比例
                float ratio = (float)actual_sample_rate * actual_channels / target_sample_rate;
        
                int output_samples = raw_pcm_data.size() / ratio;
                pcm_data.resize(output_samples);
                //进行样本缩小，提高播放速度
                for (int i = 0; i < output_samples; i++) {
                    pcm_data[i] = raw_pcm_data[i * ratio];
                }
                ESP_LOGI(TAG, "Downsampled %d samples to %d samples (%dHz -> %dHz, step=%.2f)", raw_pcm_data.size(), pcm_data.size(), actual_sample_rate, target_sample_rate, ratio);
            } else {
                // 比例接近1，直接使用原始数据
                pcm_data = std::move(raw_pcm_data);
                ESP_LOGI(TAG, "Got %d PCM samples from M4A decoder at %dHz (ratio≈1, no resampling)", 
                         pcm_data.size(), actual_sample_rate);
            }
        } else {
            ESP_LOGI(TAG, "Invalid sample count: %d, skipping this chunk", sample_count);
        }
    }
    
    return pcm_data;
}

// 设置重采样器
void M4aDecoder::SetResampler(int input_sample_rate, int output_sample_rate) {
    if (input_sample_rate != output_sample_rate) {
        ESP_LOGI(TAG, "Setting up resampler: %d Hz -> %d Hz", input_sample_rate, output_sample_rate);
        // 如果需要重采样，可以在这里实现
        // 目前支持标准44.1kHz输出
    } else {
        ESP_LOGI(TAG, "No resampling needed: using %d Hz (standard 44.1kHz)", input_sample_rate);
        resampler_.reset();
    }
}
