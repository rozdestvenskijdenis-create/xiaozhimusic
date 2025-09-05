#include "esp_adf_mp3_player.h"
#include "codec/mp3_decoder.h"
#include <esp_log.h>
#include <cstring>

const char* EspAdfMp3Player::TAG = "EspAdfMp3Player";

EspAdfMp3Player::EspAdfMp3Player() 
    : initialized_(false)
    , playing_(false)
    , sample_rate_(44100)
    , channels_(2)
    , pipeline_(nullptr)
    , http_stream_reader_(nullptr)
    , mp3_decoder_(nullptr)
    , i2s_stream_writer_(nullptr)
    , evt_(nullptr) {
}

EspAdfMp3Player::~EspAdfMp3Player() {
    Deinitialize();
}

bool EspAdfMp3Player::Initialize(int sample_rate, int channels) {
    if (initialized_) {
        ESP_LOGW(TAG, "ESP-ADF MP3 player already initialized");
        return true;
    }
    
    sample_rate_ = sample_rate;
    channels_ = channels;
    
    ESP_LOGI(TAG, "Initializing ESP-ADF MP3 player: sample_rate=%d, channels=%d", sample_rate_, channels_);
    
    if (!CreatePipeline()) {
        ESP_LOGE(TAG, "Failed to create audio pipeline");
        return false;
    }
    
    initialized_ = true;
    ESP_LOGI(TAG, "ESP-ADF MP3 player initialized successfully");
    return true;
}

void EspAdfMp3Player::Deinitialize() {
    if (!initialized_) {
        return;
    }
    
    ESP_LOGI(TAG, "Deinitializing ESP-ADF MP3 player");
    
    Stop();
    DestroyPipeline();
    
    initialized_ = false;
    playing_ = false;
    
    ESP_LOGI(TAG, "ESP-ADF MP3 player deinitialized");
}

bool EspAdfMp3Player::CreatePipeline() {
    // Create audio pipeline
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline_ = audio_pipeline_init(&pipeline_cfg);
    if (pipeline_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize audio pipeline");
        return false;
    }
    
    // Create HTTP stream reader
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_stream_reader_ = http_stream_init(&http_cfg);
    if (http_stream_reader_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize HTTP stream reader");
        return false;
    }
    
    // Create MP3 decoder
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder_ = mp3_decoder_init(&mp3_cfg);
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        return false;
    }
    
    // Create I2S stream writer
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer_ = i2s_stream_init(&i2s_cfg);
    if (i2s_stream_writer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize I2S stream writer");
        return false;
    }
    
    // Register elements to pipeline
    audio_pipeline_register(pipeline_, http_stream_reader_, "http");
    audio_pipeline_register(pipeline_, mp3_decoder_, "mp3");
    audio_pipeline_register(pipeline_, i2s_stream_writer_, "i2s");
    
    // Link elements: http_stream --> mp3_decoder --> i2s_stream
    const char *link_tag[3] = {"http", "mp3", "i2s"};
    if (audio_pipeline_link(pipeline_, &link_tag[0], 3) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to link audio pipeline elements");
        return false;
    }
    
    // Create event interface
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    evt_ = audio_event_iface_init(&evt_cfg);
    if (evt_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize audio event interface");
        return false;
    }
    
    // Set up event listener
    audio_pipeline_set_listener(pipeline_, evt_);
    
    ESP_LOGI(TAG, "Audio pipeline created successfully");
    return true;
}

void EspAdfMp3Player::DestroyPipeline() {
    if (pipeline_ != nullptr) {
        audio_pipeline_stop(pipeline_);
        audio_pipeline_wait_for_stop(pipeline_);
        audio_pipeline_terminate(pipeline_);
        
        // Unregister elements
        audio_pipeline_unregister(pipeline_, http_stream_reader_);
        audio_pipeline_unregister(pipeline_, mp3_decoder_);
        audio_pipeline_unregister(pipeline_, i2s_stream_writer_);
        
        // Remove listener
        audio_pipeline_remove_listener(pipeline_);
        
        // Deinitialize pipeline
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
    }
    
    if (http_stream_reader_ != nullptr) {
        audio_element_deinit(http_stream_reader_);
        http_stream_reader_ = nullptr;
    }
    
    if (mp3_decoder_ != nullptr) {
        audio_element_deinit(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    
    if (i2s_stream_writer_ != nullptr) {
        audio_element_deinit(i2s_stream_writer_);
        i2s_stream_writer_ = nullptr;
    }
    
    if (evt_ != nullptr) {
        audio_event_iface_destroy(evt_);
        evt_ = nullptr;
    }
}

bool EspAdfMp3Player::PlayUrl(const std::string& url) {
    if (!initialized_) {
        ESP_LOGE(TAG, "Player not initialized");
        return false;
    }
    
    if (playing_) {
        ESP_LOGW(TAG, "Already playing, stopping current stream");
        Stop();
    }
    
    ESP_LOGI(TAG, "Starting playback of URL: %s", url.c_str());
    
    // Set URI for HTTP stream
    if (audio_element_set_uri(http_stream_reader_, url.c_str()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set URI for HTTP stream");
        return false;
    }
    
    // Start pipeline
    if (audio_pipeline_run(pipeline_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio pipeline");
        return false;
    }
    
    playing_ = true;
    ESP_LOGI(TAG, "Playback started successfully");
    
    // Start event handling task
    HandleAudioEvents();
    
    return true;
}

void EspAdfMp3Player::Stop() {
    if (!initialized_ || !playing_) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping playback");
    
    if (audio_pipeline_stop(pipeline_) == ESP_OK) {
        audio_pipeline_wait_for_stop(pipeline_);
    }
    
    playing_ = false;
    ESP_LOGI(TAG, "Playback stopped");
}

void EspAdfMp3Player::Pause() {
    if (!initialized_ || !playing_) {
        return;
    }
    
    ESP_LOGI(TAG, "Pausing playback");
    audio_pipeline_pause(pipeline_);
}

void EspAdfMp3Player::Resume() {
    if (!initialized_ || !playing_) {
        return;
    }
    
    ESP_LOGI(TAG, "Resuming playback");
    audio_pipeline_resume(pipeline_);
}

void EspAdfMp3Player::HandleAudioEvents() {
    if (!evt_) {
        return;
    }
    
    audio_event_iface_msg_t msg;
    esp_err_t ret = audio_event_iface_listen(evt_, &msg, 0); // Non-blocking
    
    if (ret != ESP_OK) {
        return;
    }
    
    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
        && msg.source == (void *) mp3_decoder_
        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
        
        audio_element_info_t music_info = {0};
        audio_element_getinfo(mp3_decoder_, &music_info);
        
        ESP_LOGI(TAG, "Music info: sample_rate=%d, bits=%d, channels=%d",
                 music_info.sample_rates, music_info.bits, music_info.channels);
        
        // Update I2S stream configuration based on music info
        i2s_stream_set_clk(i2s_stream_writer_, music_info.sample_rates, 
                          music_info.bits, music_info.channels);
        
        // Update our internal parameters
        sample_rate_ = music_info.sample_rates;
        channels_ = music_info.channels;
    }
    
    // Check for stop/finish events
    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT 
        && msg.source == (void *) i2s_stream_writer_
        && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
        && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || 
            ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
        
        ESP_LOGI(TAG, "Playback finished");
        playing_ = false;
    }
}
