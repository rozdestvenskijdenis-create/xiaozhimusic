#include "audio_service.h"
#include <esp_log.h>
#include <cstring>

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_USE_AFE_WAKE_WORD
#include "wake_words/afe_wake_word.h"
#elif CONFIG_USE_ESP_WAKE_WORD
#include "wake_words/esp_wake_word.h"
#elif CONFIG_USE_CUSTOM_WAKE_WORD
#include "wake_words/custom_wake_word.h"
#endif

#define TAG "AudioService"


AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
    
    // 初始化ESP-ADF MP3播放器
    esp_adf_mp3_player_ = std::make_unique<EspAdfMp3Player>();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    
    // 清理MP3解码器
    if (mp3_decoder_) {
        mp3_decoder_->Deinitialize();
        mp3_decoder_.reset();
    }
    
    // 清理ESP-ADF MP3播放器
    if (esp_adf_mp3_player_) {
        esp_adf_mp3_player_->Deinitialize();
        esp_adf_mp3_player_.reset();
    }
}


void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    /* Setup the audio codec */
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_->SetComplexity(0);

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

#if CONFIG_USE_AFE_WAKE_WORD
    wake_word_ = std::make_unique<AfeWakeWord>();
#elif CONFIG_USE_ESP_WAKE_WORD
    wake_word_ = std::make_unique<EspWakeWord>();
#elif CONFIG_USE_CUSTOM_WAKE_WORD
    wake_word_ = std::make_unique<CustomWakeWord>();
#else
    wake_word_ = nullptr;
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
    
    // 初始化ESP-ADF MP3播放器
    if (esp_adf_mp3_player_) {
        ESP_LOGI(TAG, "Attempting to initialize ESP-ADF MP3 player with sample_rate=%d, channels=%d", 
                 codec_->output_sample_rate(), codec_->output_channels());
        if (esp_adf_mp3_player_->Initialize(codec_->output_sample_rate(), codec_->output_channels())) {
            ESP_LOGI(TAG, "ESP-ADF MP3 player initialized successfully");
        } else {
            ESP_LOGE(TAG, "Failed to initialize ESP-ADF MP3 player");
        }
    } else {
        ESP_LOGE(TAG, "ESP-ADF MP3 player is null");
    }
}

void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 1);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, 3, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048, this, 3, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        vTaskDelete(NULL);
    }, "opus_codec", 2048 * 13, this, 2, &opus_codec_task_handle_);

    /* Start the music streaming task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->MusicStreamTask();
        vTaskDelete(NULL);
    }, "music_stream", 2048 * 3, this, 4, &music_task_handle_);
}

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    // 停止音乐播放
    StopMusic();
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
    
    // 清理统一的音频播放队列
    {
        std::lock_guard<std::mutex> playback_lock(audio_playback_mutex_);
        audio_playback_queue_.clear();
    }
    audio_playback_cv_.notify_all();
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (codec_->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the wake word */
        if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
            std::vector<int16_t> data;
            int samples = wake_word_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    wake_word_->Feed(data);
                    continue;
                }
            }
        }

        /* Feed the audio processor */
        if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
            std::vector<int16_t> data;
            int samples = audio_processor_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    audio_processor_->Feed(std::move(data));
                    continue;
                }
            }
        }

        ESP_LOGE(TAG, "Should not be here, bits: %lx", bits);
        break;
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        // audio_queue_cv_.wait(lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_; });
        audio_playback_cv_.wait(lock, [this]() { 
            return !audio_playback_queue_.empty() || 
                   service_stopped_; 
        });
        if (service_stopped_) {
            break;
        }
        // 播放音频数据
        if (!audio_playback_queue_.empty()) {
            std::lock_guard<std::mutex> playback_lock(audio_playback_mutex_);
            auto audio_data = std::move(audio_playback_queue_.front());
            audio_playback_queue_.pop_front();
            lock.unlock();
            
            // 播放PCM数据
            if (!codec_->output_enabled()) {
                esp_timer_stop(audio_power_timer_);
                esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
                codec_->EnableOutput(true);
            }
            codec_->OutputData(audio_data);
            
            /* Update the last output time */
            last_output_time_ = std::chrono::steady_clock::now();
            debug_statistics_.playback_count++;
        }
    }
    ESP_LOGW(TAG, "Audio output task stopped");
}
void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) ||
                (!audio_decode_queue_.empty());
        });
        if (service_stopped_) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty()) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            if (opus_decoder_->Decode(std::move(packet->payload), task->pcm)) {
                // Resample if the sample rate is different
                if (opus_decoder_->sample_rate() != codec_->output_sample_rate()) {
                    int target_size = output_resampler_.GetOutputSamples(task->pcm.size());
                    std::vector<int16_t> resampled(target_size);
                    output_resampler_.Process(task->pcm.data(), task->pcm.size(), resampled.data());
                    task->pcm = std::move(resampled);
                }

                lock.lock();
                // 将解码后的PCM数据推送到统一的音频播放队列
                {
                    std::lock_guard<std::mutex> playback_lock(audio_playback_mutex_);
                    if (audio_playback_queue_.size() < 20) {
                        audio_playback_queue_.push_back(std::move(task->pcm));
                    }
                }
                audio_playback_cv_.notify_all();
            } else {
                ESP_LOGE(TAG, "Failed to decode audio");
                lock.lock();
            }
            debug_statistics_.decode_count++;
        }
        
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;
            if (!opus_encoder_->Encode(std::move(task->pcm), packet->payload)) {
                ESP_LOGE(TAG, "Failed to encode audio");
                continue;
            }

            if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    audio_send_queue_.push_back(std::move(packet));
                }
                if (callbacks_.on_send_queue_available) {
                    callbacks_.on_send_queue_available();
                }
            } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                audio_testing_queue_.push_back(std::move(packet));
            }
            debug_statistics_.encode_count++;
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    audio_queue_cv_.wait(lock, [this]() { return audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE; });
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() { return audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE; });
        } else {
            return false;
        }
    }
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        wake_word_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS);
            audio_processor_initialized_ = true;
        }

        /* We should make sure no audio is playing */
        ResetDecoder();
        audio_input_need_warmup_ = true;
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        audio_processor_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS);
        audio_processor_initialized_ = true;
    }

    audio_processor_->EnableDeviceAec(enable);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::PlaySound(const std::string_view& ogg) {
    const uint8_t* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();
    size_t offset = 0;

    auto find_page = [&](size_t start)->size_t {
        for (size_t i = start; i + 4 <= size; ++i) {
            if (buf[i] == 'O' && buf[i+1] == 'g' && buf[i+2] == 'g' && buf[i+3] == 'S') return i;
        }
        return static_cast<size_t>(-1);
    };

    bool seen_head = false;
    bool seen_tags = false;
    int sample_rate = 16000; // 默认值

    while (true) {
        size_t pos = find_page(offset);
        if (pos == static_cast<size_t>(-1)) break;
        offset = pos;
        if (offset + 27 > size) break;

        const uint8_t* page = buf + offset;
        uint8_t page_segments = page[26];
        size_t seg_table_off = offset + 27;
        if (seg_table_off + page_segments > size) break;

        size_t body_size = 0;
        for (size_t i = 0; i < page_segments; ++i) body_size += page[27 + i];

        size_t body_off = seg_table_off + page_segments;
        if (body_off + body_size > size) break;

        // Parse packets using lacing
        size_t cur = body_off;
        size_t seg_idx = 0;
        while (seg_idx < page_segments) {
            size_t pkt_len = 0;
            size_t pkt_start = cur;
            bool continued = false;
            do {
                uint8_t l = page[27 + seg_idx++];
                pkt_len += l;
                cur += l;
                continued = (l == 255);
            } while (continued && seg_idx < page_segments);

            if (pkt_len == 0) continue;
            const uint8_t* pkt_ptr = buf + pkt_start;

            if (!seen_head) {
                // 解析OpusHead包
                if (pkt_len >= 19 && std::memcmp(pkt_ptr, "OpusHead", 8) == 0) {
                    seen_head = true;
                    
                    // OpusHead结构：[0-7] "OpusHead", [8] version, [9] channel_count, [10-11] pre_skip
                    // [12-15] input_sample_rate, [16-17] output_gain, [18] mapping_family
                    if (pkt_len >= 12) {
                        uint8_t version = pkt_ptr[8];
                        uint8_t channel_count = pkt_ptr[9];
                        
                        if (pkt_len >= 16) {
                            // 读取输入采样率 (little-endian)
                            sample_rate = pkt_ptr[12] | (pkt_ptr[13] << 8) | 
                                        (pkt_ptr[14] << 16) | (pkt_ptr[15] << 24);
                            ESP_LOGI(TAG, "OpusHead: version=%d, channels=%d, sample_rate=%d", 
                                   version, channel_count, sample_rate);
                        }
                    }
                }
                continue;
            }
            if (!seen_tags) {
                // Expect OpusTags in second packet
                if (pkt_len >= 8 && std::memcmp(pkt_ptr, "OpusTags", 8) == 0) {
                    seen_tags = true;
                }
                continue;
            }

            // Audio packet (Opus)
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = 60;
            packet->payload.resize(pkt_len);
            std::memcpy(packet->payload.data(), pkt_ptr, pkt_len);
            PushPacketToDecodeQueue(std::move(packet), true);
        }

        offset = body_off + body_size;
    }
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    std::lock_guard<std::mutex> playback_lock(audio_playback_mutex_);
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    opus_decoder_->ResetState();
    timestamp_queue_.clear();
    audio_decode_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
    
    // 清理统一的音频播放队列
    {
        std::lock_guard<std::mutex> playback_lock(audio_playback_mutex_);
        audio_playback_queue_.clear();
    }
    audio_playback_cv_.notify_all();
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        codec_->EnableOutput(false);
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

// MP3音乐播放相关方法实现
void AudioService::PlayMusicFromUrl(const std::string& url) {
    ESP_LOGI(TAG, "PlayMusicFromUrl called with URL: %s", url.c_str());
    
    if (music_playing_) {
        ESP_LOGI(TAG, "Music already playing, stopping current music");
        StopMusic();
    }
    
    // 使用ESP-ADF MP3播放器播放
    if (esp_adf_mp3_player_ && esp_adf_mp3_player_->IsInitialized()) {
        if (esp_adf_mp3_player_->PlayUrl(url)) {
            current_music_url_ = url;
            music_playing_ = true;
            ESP_LOGI(TAG, "Started ESP-ADF MP3 playback from: %s", url.c_str());
        } else {
            ESP_LOGE(TAG, "Failed to start ESP-ADF MP3 playback");
        }
    } else {
        ESP_LOGW(TAG, "ESP-ADF MP3 player not available, falling back to custom implementation");
        current_music_url_ = url;
        music_playing_ = true;
        ESP_LOGI(TAG, "Starting custom MP3 stream from: %s", url.c_str());
    }
}

void AudioService::StopMusic() {
    music_playing_ = false;
    current_music_url_.clear();
    
    // 停止ESP-ADF MP3播放器
    if (esp_adf_mp3_player_ && esp_adf_mp3_player_->IsInitialized()) {
        esp_adf_mp3_player_->Stop();
        ESP_LOGI(TAG, "ESP-ADF MP3 player stopped");
    }
    
    // 清空音频播放队列
    {
        std::lock_guard<std::mutex> lock(audio_playback_mutex_);
        audio_playback_queue_.clear();
    }
    audio_playback_cv_.notify_all();
    
    // 清理MP3解码器
    if (mp3_decoder_) {
        mp3_decoder_->Deinitialize();
        mp3_decoder_.reset();
        ESP_LOGI(TAG, "MP3 decoder cleaned up");
    }
    
    ESP_LOGI(TAG, "Music stopped");
}

void AudioService::MusicStreamTask() {
    ESP_LOGI(TAG, "Music stream task started");
    
    while (true) {
        if (music_playing_ && !current_music_url_.empty()) {
            ESP_LOGI(TAG, "Starting music stream from: %s", current_music_url_.c_str());
            
            // 创建HTTP客户端
            auto network = Board::GetInstance().GetNetwork();
            auto http = network->CreateHttp(0);
            
            if (http->Open("GET", current_music_url_)) {
                ESP_LOGI(TAG, "Connected to MP3 stream");
                
                // 检查HTTP状态码
                auto status_code = http->GetStatusCode();
                if (status_code != 200) {
                    ESP_LOGE(TAG, "HTTP request failed with status code: %d", status_code);
                    http->Close();
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
                
                // 开始流式读取MP3数据
                char buffer[4096];
                int bytes_read;
                std::vector<uint8_t> mp3_buffer;
                int total_bytes_read = 0;
                
                while (music_playing_ && (bytes_read = http->Read(buffer, sizeof(buffer))) > 0) {
                    if (bytes_read > 0) {
                        total_bytes_read += bytes_read;
                        
                        // 将MP3数据添加到缓冲区
                        mp3_buffer.insert(mp3_buffer.end(), buffer, buffer + bytes_read);
                        
                        // 当缓冲区达到一定大小时进行解码
                        if (mp3_buffer.size() >= 2048) { // 增大缓冲区大小
                            // 根据URL和文件头判断文件类型并解码
                            std::vector<int16_t> pcm_data;
                            if (current_music_url_.find(".wav") != std::string::npos || 
                                (mp3_buffer.size() >= 4 && memcmp(mp3_buffer.data(), "RIFF", 4) == 0)) {
                                ESP_LOGD(TAG, "Detected WAV format, using WAV decoder");
                                pcm_data = DecodeWavChunk(mp3_buffer);
                            } else if (current_music_url_.find(".m4a") != std::string::npos) {
                                ESP_LOGW(TAG, "M4A format not supported yet, skipping");
                                mp3_buffer.clear();
                                continue;
                            } else {
                                ESP_LOGD(TAG, "Using MP3 decoder");
                                pcm_data = DecodeMp3Chunk(mp3_buffer);
                            }
                            
                            if (!pcm_data.empty()) {
                                // 推送到统一的音频播放队列
                                {
                                    std::lock_guard<std::mutex> lock(audio_playback_mutex_);
                                                                    if (audio_playback_queue_.size() < 30) { // 进一步增大队列大小
                                    audio_playback_queue_.push_back(std::move(pcm_data));
                                    ESP_LOGD(TAG, "Added PCM data to queue, queue size: %zu", 
                                            audio_playback_queue_.size());
                                } else {
                                    // 减少日志频率，避免日志刷屏
                                    static int drop_count = 0;
                                    if (++drop_count % 10 == 1) {
                                        ESP_LOGW(TAG, "Audio playback queue full, dropping PCM data (dropped %d times)", drop_count);
                                    }
                                }
                                }
                                audio_playback_cv_.notify_all();
                            }
                            
                            // 清空已处理的MP3数据
                            mp3_buffer.clear();
                        }
                        
                        // 控制读取速度
                        vTaskDelay(pdMS_TO_TICKS(5));
                    }
                }
                
                ESP_LOGI(TAG, "Music stream ended, total bytes read: %d", total_bytes_read);
                http->Close();
                
                // 如果音乐还在播放状态，等待一段时间后重试
                if (music_playing_) {
                    ESP_LOGI(TAG, "Music stream ended, waiting before retry...");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
            } else {
                ESP_LOGE(TAG, "Failed to connect to MP3 stream: %s", current_music_url_.c_str());
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(100)); // 检查间隔
    }
}

std::vector<int16_t> AudioService::DecodeMp3Chunk(const std::vector<uint8_t>& mp3_data) {
    std::vector<int16_t> pcm_data;
    
    if (mp3_data.empty()) {
        return pcm_data;
    }
    
    // 初始化MP3解码器（如果尚未初始化）
    if (!mp3_decoder_) {
        mp3_decoder_ = std::make_unique<Mp3Decoder>();
        if (!mp3_decoder_->Initialize(codec_->output_sample_rate(), codec_->output_channels())) {
            ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
            mp3_decoder_.reset();
            return pcm_data;
        }
        ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    }
    
    // 使用真正的MP3解码器解码数据
    pcm_data = mp3_decoder_->DecodeChunk(mp3_data);
    
    if (!pcm_data.empty()) {
        ESP_LOGD(TAG, "MP3 decode: %zu bytes -> %zu PCM samples", 
                 mp3_data.size(), pcm_data.size());
    }
    
    return pcm_data;
}

std::vector<int16_t> AudioService::DecodeWavChunk(const std::vector<uint8_t>& wav_data) {
    std::vector<int16_t> pcm_data;
    
    if (wav_data.size() < 44) { // WAV文件头至少44字节
        return pcm_data;
    }
    
    // 检查WAV文件头
    if (memcmp(wav_data.data(), "RIFF", 4) != 0 || 
        memcmp(wav_data.data() + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV file header");
        return pcm_data;
    }
    
    // 查找data块
    size_t data_offset = 0;
    for (size_t i = 12; i < wav_data.size() - 8; i += 4) {
        if (memcmp(wav_data.data() + i, "data", 4) == 0) {
            data_offset = i + 8;
            break;
        }
    }
    
    if (data_offset == 0 || data_offset >= wav_data.size()) {
        ESP_LOGE(TAG, "WAV data chunk not found");
        return pcm_data;
    }
    
    // 提取PCM数据
    size_t data_size = wav_data.size() - data_offset;
    pcm_data.resize(data_size / sizeof(int16_t));
    memcpy(pcm_data.data(), wav_data.data() + data_offset, data_size);
    
    ESP_LOGD(TAG, "WAV decode: %zu bytes -> %zu PCM samples", data_size, pcm_data.size());
    
    return pcm_data;
}