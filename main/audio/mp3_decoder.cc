#include "mp3_decoder.h"
#include <esp_log.h>
#include "opus_resampler.h"
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
    
    // 创建MP3解码器 - 让MP3解码器使用原始采样率，然后我们进行转换
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    // 不设置输出采样率，让MP3解码器使用原始采样率
    mp3_decoder_ = mp3_decoder_init(&mp3_cfg);
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        audio_element_deinit(http_stream_);
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    
    // 创建一个输出环形缓冲区来存储MP3解码后的PCM数据
    // 使用默认大小，避免内存问题
    output_ringbuf_ = rb_create(4096, 1);  // 4KB缓冲区，使用默认大小
    if (!output_ringbuf_) {
        ESP_LOGE(TAG, "Failed to create output ring buffer");
        audio_element_deinit(mp3_decoder_);
        audio_element_deinit(http_stream_);
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    
    // 将输出缓冲区连接到MP3解码器
    audio_element_set_output_ringbuf(mp3_decoder_, output_ringbuf_);
    
    i2s_stream_ = nullptr;
    ESP_LOGI(TAG, "Created output buffer for MP3 decoder (no I2S stream)");
    
    // 注册元素到管道（不包含I2S流）
    audio_pipeline_register(pipeline_, http_stream_, "http");
    audio_pipeline_register(pipeline_, mp3_decoder_, "mp3");
    
    // 链接管道元素：http -> mp3（不包含I2S）
    const char *link_tag[2] = {"http", "mp3"};
    if (audio_pipeline_link(pipeline_, &link_tag[0], 2) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to link audio pipeline elements");
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
    ESP_LOGI(TAG, "Pipeline: %p, HTTP stream: %p, MP3 decoder: %p, I2S stream: %p", 
             pipeline_, http_stream_, mp3_decoder_, i2s_stream_);
    return true;
}

bool Mp3Decoder::PlayUrl(const std::string& url) {
    if (!initialized_) {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
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
    
    ESP_LOGI(TAG, "Starting MP3 playback from URL: %s", url.c_str());
    
    // 设置HTTP流的URL
    esp_err_t ret = audio_element_set_uri(http_stream_, url.c_str());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set URI for HTTP stream, error: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "URI set successfully, starting pipeline");
    
    // 启动音频管道（只包含HTTP和MP3解码器）
    ret = audio_pipeline_run(pipeline_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio pipeline, error: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "MP3 playback started successfully (without I2S output)");
    ESP_LOGI(TAG, "Note: PCM data will be available through DecodeChunk method");
    
    // 等待一小段时间让MP3解码器开始工作，然后检测采样率
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 暂时禁用重采样器，直接使用MP3的原始采样率
    // 这样可以避免重采样器初始化失败的问题
    ESP_LOGI(TAG, "Skipping resampler setup for now, using MP3 original sample rate");
    resampler_.reset();
    
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
        
        // 注销所有元素（不包含I2S流）
        audio_pipeline_unregister(pipeline_, http_stream_);
        audio_pipeline_unregister(pipeline_, mp3_decoder_);
        
        // 销毁所有元素（不包含I2S流）
        if (mp3_decoder_) {
            audio_element_deinit(mp3_decoder_);
            mp3_decoder_ = nullptr;
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
    
    mp3_buffer_.clear();
    output_buffer_.clear();
    initialized_ = false;
    
    ESP_LOGI(TAG, "ESP-ADF MP3 decoder pipeline deinitialized");
}

std::vector<int16_t> Mp3Decoder::DecodeChunk(const std::vector<uint8_t>& mp3_data) {
    std::vector<int16_t> pcm_data;
    
    if (!initialized_ || !mp3_decoder_) {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
        return pcm_data;
    }
    
    if (mp3_data.empty()) {
        return pcm_data;
    }
    
    ESP_LOGD(TAG, "DecodeChunk called with %zu bytes of MP3 data", mp3_data.size());
    
    // 使用ESP-ADF的音频缓冲区API来获取PCM数据
    // 从MP3解码器的输出环形缓冲区读取PCM数据
    
    // 获取MP3解码器的输出环形缓冲区
    ringbuf_handle_t output_rb = audio_element_get_output_ringbuf(mp3_decoder_);
    if (!output_rb) {
        ESP_LOGD(TAG, "Failed to get output ringbuf from MP3 decoder");
        return pcm_data;
    }
    
    // 从输出环形缓冲区读取PCM数据
    char output_buffer[4096];
    int bytes_read = rb_read(output_rb, output_buffer, sizeof(output_buffer), 0); // 非阻塞读取
    if (bytes_read > 0) {
        // 将字节数据转换为int16_t PCM数据
        int16_t* pcm_samples = reinterpret_cast<int16_t*>(output_buffer);
        int sample_count = bytes_read / sizeof(int16_t);
        
        pcm_data.resize(sample_count);
        std::memcpy(pcm_data.data(), pcm_samples, bytes_read);
        
        ESP_LOGD(TAG, "MP3 decode: got %zu PCM samples from decoder", pcm_data.size());
    } else {
        ESP_LOGD(TAG, "MP3 decode: no PCM data available (bytes_read=%d)", bytes_read);
    }
    return pcm_data;
}

// 新增方法：从MP3解码器获取PCM数据
std::vector<int16_t> Mp3Decoder::GetPcmData() {
    std::vector<int16_t> pcm_data;
    
    if (!initialized_ || !output_ringbuf_) {
        return pcm_data;
    }
    
    // 从我们创建的输出缓冲区读取PCM数据，提高流畅度
    char output_buffer[2048];  // 增加缓冲区大小，提高流畅度
    int bytes_read = rb_read(output_ringbuf_, output_buffer, sizeof(output_buffer), 0); // 非阻塞读取
    if (bytes_read > 0) {
        // 将字节数据转换为int16_t PCM数据
        int16_t* pcm_samples = reinterpret_cast<int16_t*>(output_buffer);
        int sample_count = bytes_read / sizeof(int16_t);
        
        // 安全检查，避免过大的内存分配，平衡样本数量
        if (sample_count > 0 && sample_count < 1500) {  // 限制每次最多1500个样本，平衡流畅度和丢包
            std::vector<int16_t> raw_pcm_data(sample_count);
            std::memcpy(raw_pcm_data.data(), pcm_samples, bytes_read);
            
            // 简单的重采样：44100Hz -> 24000Hz
            const int input_sample_rate = 44100;
            const int output_sample_rate = 24000;
            
            if (input_sample_rate != output_sample_rate) {
                // 最简单的重采样：每2个样本取1个 (44100/2 = 22050，接近24000)
                int output_samples = raw_pcm_data.size() / 2;
                if (output_samples > 0) {
                    pcm_data.resize(output_samples);
                    for (int i = 0; i < output_samples; i++) {
                        pcm_data[i] = raw_pcm_data[i * 2];
                    }
                    ESP_LOGD(TAG, "Simple resampled %zu samples to %zu samples", raw_pcm_data.size(), pcm_data.size());
                } else {
                    pcm_data = std::move(raw_pcm_data);
                }
            } else {
                // 采样率相同，直接使用原始数据
                pcm_data = std::move(raw_pcm_data);
                ESP_LOGD(TAG, "Got %zu PCM samples from MP3 decoder (no resampling)", pcm_data.size());
            }
        } else {
            ESP_LOGW(TAG, "Invalid sample count: %d, skipping this chunk", sample_count);
        }
    }
    
    return pcm_data;
}

// 设置重采样器
void Mp3Decoder::SetResampler(int input_sample_rate, int output_sample_rate) {
    if (input_sample_rate != output_sample_rate) {
        ESP_LOGI(TAG, "Setting up simple resampler: %d Hz -> %d Hz", input_sample_rate, output_sample_rate);
        // 暂时不使用OpusResampler，直接设置重采样参数
        // 我们将在GetPcmData中实现简单的重采样
    } else {
        ESP_LOGI(TAG, "No resampling needed: %d Hz", input_sample_rate);
        resampler_.reset();
    }
}
