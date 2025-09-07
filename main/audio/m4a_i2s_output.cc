#include "m4a_i2s_output.h"
#include <esp_log.h>
#include <cstring>
#include <algorithm>

const char* M4aI2sOutput::TAG = "M4aI2sOutput";

M4aI2sOutput::M4aI2sOutput() 
    : tx_handle_(nullptr)
    , enable_pin_(GPIO_NUM_NC)
    , initialized_(false)
    , playing_(false)
    , pcm_queue_(nullptr)
    , playback_task_(nullptr) {
}

M4aI2sOutput::~M4aI2sOutput() {
    Deinitialize();
}

bool M4aI2sOutput::Initialize(gpio_num_t bclk_pin, gpio_num_t lrclk_pin, gpio_num_t dout_pin, gpio_num_t enable_pin) {
    if (initialized_) {
        ESP_LOGW(TAG, "M4A I2S output already initialized");
        return true;
    }

    enable_pin_ = enable_pin;
    
    ESP_LOGI(TAG, "Initializing M4A I2S output: BCLK=%d, LRCLK=%d, DOUT=%d, EN=%d", 
             bclk_pin, lrclk_pin, dout_pin, enable_pin);

    // 配置功放使能管脚
    if (enable_pin_ != GPIO_NUM_NC) {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << enable_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(enable_pin_, 0); // 初始关闭功放
    }

    // 创建I2S通道配置（使用I2S_NUM_1，避免与语音对话的I2S_NUM_0冲突）
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 480; // 44100Hz下的合适帧数（双声道）
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    // 配置I2S标准模式
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk_pin,
            .ws = lrclk_pin,
            .dout = dout_pin,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    esp_err_t ret = i2s_channel_init_std_mode(tx_handle_, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
        return false;
    }

    // 定义PCM数据队列项结构体
    struct PcmQueueItem {
        int16_t* data;
        size_t size;
    };
    
    // 创建PCM数据队列
    pcm_queue_ = xQueueCreate(10, sizeof(PcmQueueItem));
    if (pcm_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create PCM queue");
        i2s_channel_disable(tx_handle_);
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
        return false;
    }

    // 创建播放任务
    BaseType_t task_ret = xTaskCreate(PlaybackTask, "m4a_i2s_playback", 4096, this, 5, &playback_task_);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        vQueueDelete(pcm_queue_);
        pcm_queue_ = nullptr;
        i2s_channel_disable(tx_handle_);
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "M4A I2S output initialized successfully at %dHz", SAMPLE_RATE);
    return true;
}

void M4aI2sOutput::Deinitialize() {
    if (!initialized_) {
        return;
    }

    ESP_LOGI(TAG, "Deinitializing M4A I2S output");

    // 停止播放
    Stop();

    // 删除播放任务
    if (playback_task_ != nullptr) {
        vTaskDelete(playback_task_);
        playback_task_ = nullptr;
    }

    // 删除队列
    if (pcm_queue_ != nullptr) {
        vQueueDelete(pcm_queue_);
        pcm_queue_ = nullptr;
    }

    // 禁用并删除I2S通道
    if (tx_handle_ != nullptr) {
        i2s_channel_disable(tx_handle_);
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
    }

    // 关闭功放
    EnableAmplifier(false);

    initialized_ = false;
    ESP_LOGI(TAG, "M4A I2S output deinitialized");
}

bool M4aI2sOutput::PlayPcmData(const int16_t* pcm_data, size_t data_size) {
    if (!initialized_) {
        ESP_LOGE(TAG, "I2S output not initialized");
        return false;
    }

    if (pcm_data == nullptr || data_size == 0) {
        ESP_LOGE(TAG, "Invalid PCM data: pcm_data=%p, data_size=%d", pcm_data, (int)data_size);
        return false;
    }

    ESP_LOGI(TAG, "PlayPcmData called with %d bytes (%d samples)", (int)data_size, (int)(data_size / sizeof(int16_t)));

    // 分配内存并复制数据
    int16_t* data_copy = (int16_t*)malloc(data_size);
    if (data_copy == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for PCM data");
        return false;
    }
    memcpy(data_copy, pcm_data, data_size);

    // 发送到队列
    struct PcmQueueItem {
        int16_t* data;
        size_t size;
    } queue_item = {data_copy, data_size};

    BaseType_t ret = xQueueSend(pcm_queue_, &queue_item, portMAX_DELAY);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send PCM data to queue");
        free(data_copy);
        return false;
    }

    return true;
}

void M4aI2sOutput::Stop() {
    if (!initialized_) {
        return;
    }

    ESP_LOGI(TAG, "Stopping M4A I2S playback");

    playing_ = false;

    // 清空队列
    if (pcm_queue_ != nullptr) {
        struct PcmQueueItem {
            int16_t* data;
            size_t size;
        } queue_item;
        
        while (xQueueReceive(pcm_queue_, &queue_item, 0) == pdTRUE) {
            if (queue_item.data != nullptr) {
                free(queue_item.data);
            }
        }
    }

    // 禁用I2S通道
    if (tx_handle_ != nullptr) {
        i2s_channel_disable(tx_handle_);
    }

    // 关闭功放
    EnableAmplifier(false);
}

void M4aI2sOutput::PlaybackTask(void* arg) {
    M4aI2sOutput* output = (M4aI2sOutput*)arg;
    output->ProcessPlayback();
}

void M4aI2sOutput::ProcessPlayback() {
    ESP_LOGI(TAG, "M4A I2S playback task started");

    while (true) {
        struct PcmQueueItem {
            int16_t* data;
            size_t size;
        } queue_item;

        // 等待PCM数据
        BaseType_t ret = xQueueReceive(pcm_queue_, &queue_item, portMAX_DELAY);
        if (ret != pdTRUE) {
            continue;
        }

        if (queue_item.data == nullptr || queue_item.size == 0) {
            continue;
        }

        // 如果是第一次播放，启用功放和I2S通道
        if (!playing_) {
            EnableAmplifier(true);
            
            // 启用I2S通道
            if (i2s_channel_enable(tx_handle_) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to enable I2S channel");
                free(queue_item.data);
                continue;
            }
            playing_ = true;
            ESP_LOGI(TAG, "I2S channel enabled for continuous playback");
        }

        // 播放PCM数据
        size_t bytes_written = 0;
        size_t total_bytes = queue_item.size;
        const uint8_t* data_ptr = (const uint8_t*)queue_item.data;
        
        ESP_LOGI(TAG, "Playing %d bytes of PCM data", (int)total_bytes);

        while (bytes_written < total_bytes && playing_) {
            size_t bytes_to_write = std::min((size_t)BUFFER_SIZE, total_bytes - bytes_written);
            size_t bytes_written_this_time = 0;
            
            esp_err_t write_ret = i2s_channel_write(tx_handle_, data_ptr + bytes_written, bytes_to_write, &bytes_written_this_time, portMAX_DELAY);
            if (write_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write to I2S: %s", esp_err_to_name(write_ret));
                break;
            }
            bytes_written += bytes_written_this_time;
        }

        // 释放数据内存
        free(queue_item.data);
    }
}

void M4aI2sOutput::EnableAmplifier(bool enable) {
    if (enable_pin_ != GPIO_NUM_NC) {
        gpio_set_level(enable_pin_, enable ? 1 : 0);
        ESP_LOGD(TAG, "Amplifier %s", enable ? "enabled" : "disabled");
    }
}
