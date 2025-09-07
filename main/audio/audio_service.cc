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
    
    // 注意：不再创建ESP-ADF MP3播放器，避免I2S冲突
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    
    // 清理M4A解码器
    if (m4a_decoder_) {
        m4a_decoder_->Deinitialize();
        m4a_decoder_.reset();
    }
    
    // 注意：不再使用ESP-ADF MP3播放器
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
    
    // 注意：禁用ESP-ADF MP3播放器初始化，避免I2S通道冲突
    // 使用自定义Mp3Decoder + PCM播放通道的方案
    ESP_LOGI(TAG, "ESP-ADF MP3 player initialization disabled to avoid I2S conflicts");
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
    }, "audio_output", 2048 * 2, this, 6, &audio_output_task_handle_);  // 提高优先级到6
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
    }, "audio_output", 2048, this, 6, &audio_output_task_handle_);  // 提高优先级到6
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
    }, "music_stream", 2048 * 6, this, 4, &music_task_handle_);  // 增加栈大小到12KB

    /* Start the audio decoder task (producer) */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioDecoderTask();
        vTaskDelete(NULL);
    }, "audio_decoder", 2048 * 8, this, 5, &audio_decoder_task_handle_);  // 高优先级解码任务

    /* Start the audio buffer manager task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioBufferManagerTask();
        vTaskDelete(NULL);
    }, "audio_buffer_mgr", 2048 * 4, this, 3, &audio_buffer_mgr_task_handle_);  // 缓冲区管理任务
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
    
    // 清理多线程音频处理队列
    {
        std::lock_guard<std::mutex> raw_lock(raw_audio_mutex_);
        raw_audio_queue_.clear();
    }
    raw_audio_cv_.notify_all();
    
    {
        std::lock_guard<std::mutex> pcm_lock(processed_pcm_mutex_);
        processed_pcm_queue_.clear();
    }
    processed_pcm_cv_.notify_all();
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
    int consecutive_empty_checks = 0;
    const int max_empty_checks = 50; // 连续空检查的最大次数（约5秒）
    const int min_buffer_size = 5;   // 最小缓冲区大小，确保连续播放
    
    while (true) {
        std::unique_lock<std::mutex> lock(audio_playback_mutex_);
        
        // 等待播放队列有足够的数据或服务停止
        audio_playback_cv_.wait(lock, [this, min_buffer_size]() { 
            return !audio_playback_queue_.empty() || service_stopped_; 
        });
        
        if (service_stopped_) {
            break;
        }
        
        // 播放音频数据
        if (!audio_playback_queue_.empty()) {
            consecutive_empty_checks = 0; // 重置空检查计数
            
            // 移除缓冲区等待逻辑，直接播放可用数据以保持流畅性
            // 多线程架构已经提供了足够的缓冲
            
            auto audio_data = std::move(audio_playback_queue_.front());
            audio_playback_queue_.pop_front();
            lock.unlock();
            
            // 播放PCM数据
            if (!codec_->output_enabled()) {
                esp_timer_stop(audio_power_timer_);
                esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
                codec_->EnableOutput(true);
            }
            
            ESP_LOGD(TAG, "Playing %zu PCM samples, queue size: %zu", 
                    audio_data.size(), audio_playback_queue_.size());
            codec_->OutputData(audio_data);
            
            /* Update the last output time */
            last_output_time_ = std::chrono::steady_clock::now();
            debug_statistics_.playback_count++;
            
            // 移除播放延迟，让音频输出任务专注于连续播放
            // 播放速度由音频硬件和采样率控制，不需要软件延迟
        } else {
            // 队列为空，检查是否音乐播放已结束
            consecutive_empty_checks++;
            if (music_playing_ && consecutive_empty_checks >= max_empty_checks) {
                ESP_LOGI(TAG, "Audio playback queue empty for too long, checking if music stream ended");
                // 这里可以添加额外的检查逻辑，比如检查HTTP连接状态
                // 暂时不自动停止，让MusicStreamTask来处理
            }
            lock.unlock();
            vTaskDelay(pdMS_TO_TICKS(1)); // 最小等待
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
    
    // 清理多线程音频处理队列
    {
        std::lock_guard<std::mutex> raw_lock(raw_audio_mutex_);
        raw_audio_queue_.clear();
    }
    raw_audio_cv_.notify_all();
    
    {
        std::lock_guard<std::mutex> pcm_lock(processed_pcm_mutex_);
        processed_pcm_queue_.clear();
    }
    processed_pcm_cv_.notify_all();
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
    
    // 初始化M4A解码器（如果尚未初始化）
    if (!m4a_decoder_) {
        ESP_LOGI(TAG, "Initializing M4A decoder for URL playback");
        m4a_decoder_ = std::make_unique<M4aDecoder>();
        if (!m4a_decoder_->Initialize(44100, codec_->output_channels())) {  // 使用标准44.1kHz采样率
            ESP_LOGE(TAG, "Failed to initialize M4A decoder");
            m4a_decoder_.reset();
            return;
        }
        ESP_LOGI(TAG, "M4A decoder initialized successfully");
    }
    
    // 使用ESP-ADF的完整管道播放M4A（避免手动解码导致的内存问题）
    ESP_LOGI(TAG, "Starting ESP-ADF pipeline M4A playback for URL: %s", url.c_str());
    
    // 设置HTTP流的URL并启动管道
    if (m4a_decoder_->PlayUrl(url)) {
        current_music_url_ = url;
        music_playing_ = true;
        ESP_LOGI(TAG, "Started ESP-ADF pipeline M4A playback from: %s", url.c_str());
        
        // 确保在音乐播放时保持唤醒词检测，允许语音中断
        ESP_LOGI(TAG, "Enabling wake word detection for music interruption");
        // EnableVoiceProcessing(false);  // 禁用语音处理，避免与音乐冲突
        // EnableWakeWordDetection(true); // 启用唤醒词检测，允许中断音乐
        // 启动一个任务来定期获取PCM数据并推送到播放队列
                    xTaskCreate([](void* arg) {
                        AudioService* audio_service = (AudioService*)arg;
                        audio_service->M4aPcmDataTask();
                        vTaskDelete(NULL);
                    }, "m4a_pcm_task", 8192, this, 4, nullptr);  // 降低优先级到4，避免抢占音频输出
                    
                    // 暂停多线程音频处理任务，避免冲突
                    ESP_LOGI(TAG, "Pausing multi-threaded audio processing tasks for M4A playback");
    } else {
        ESP_LOGE(TAG, "Failed to start ESP-ADF pipeline M4A playback");
    }
}

void AudioService::StopMusic() {
    music_playing_ = false;
    current_music_url_.clear();
    
    // 停止ESP-ADF M4A解码器
    if (m4a_decoder_) {
        m4a_decoder_->Stop();
        ESP_LOGI(TAG, "ESP-ADF M4A decoder stopped");
    }
    
    // 清空音频播放队列
    {
        std::lock_guard<std::mutex> lock(audio_playback_mutex_);
        audio_playback_queue_.clear();
    }
    audio_playback_cv_.notify_all();
    
    // 清理M4A解码器
    if (m4a_decoder_) {
        m4a_decoder_->Deinitialize();
        m4a_decoder_.reset();
        ESP_LOGI(TAG, "M4A decoder cleaned up");
    }
    
    // 音乐停止后恢复正常的语音处理状态
    ESP_LOGI(TAG, "Music stopped, restoring normal voice processing");
    // EnableVoiceProcessing(true);  // 恢复语音处理功能
    // EnableWakeWordDetection(true); // 保持唤醒词检测
    
    ESP_LOGI(TAG, "Music stopped");
}

void AudioService::MusicStreamTask() {
    ESP_LOGI(TAG, "Music stream task started");
    
    while (true) {
        // 如果正在播放音乐，暂停MusicStreamTask，避免与M4aPcmDataTask冲突
        if (music_playing_) {
            vTaskDelay(pdMS_TO_TICKS(100)); // 暂停100ms
            continue;
        }
        
        // 只有在没有播放音乐时才处理HTTP流
        if (!current_music_url_.empty()) {
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
                
                bool stream_ended_normally = false;
                int consecutive_empty_reads = 0;
                const int max_empty_reads = 10; // 连续空读取的最大次数
                
                while (music_playing_) {
                    bytes_read = http->Read(buffer, sizeof(buffer));
                    
                    if (bytes_read > 0) {
                        consecutive_empty_reads = 0; // 重置空读取计数
                        total_bytes_read += bytes_read;
                        
                        // 将MP3数据添加到缓冲区
                        mp3_buffer.insert(mp3_buffer.end(), buffer, buffer + bytes_read);
                        
                        // 当缓冲区达到一定大小时推送到解码队列
                        if (mp3_buffer.size() >= 2048) { // 增大缓冲区大小
                            // 将原始音频数据推送到解码队列，让专门的解码任务处理
                            {
                                std::lock_guard<std::mutex> lock(raw_audio_mutex_);
                                if (raw_audio_queue_.size() < 20) {  // 限制原始音频队列大小
                                    raw_audio_queue_.push_back(std::move(mp3_buffer));
                                    ESP_LOGD(TAG, "Added raw audio data to decoder queue, queue size: %zu", 
                                            raw_audio_queue_.size());
                                } else {
                                    ESP_LOGW(TAG, "Raw audio queue full, dropping data");
                                }
                            }
                            raw_audio_cv_.notify_all();  // 通知解码任务
                            
                            // 清空缓冲区，准备接收下一批数据
                            mp3_buffer.clear();
                        }
                        
                        // 减少读取延迟，提高数据流速度
                        vTaskDelay(pdMS_TO_TICKS(1));
                    } else if (bytes_read == 0) {
                        // HTTP流结束
                        consecutive_empty_reads++;
                        ESP_LOGI(TAG, "HTTP stream ended (bytes_read=0), consecutive empty reads: %d", consecutive_empty_reads);
                        
                        // 处理剩余的缓冲区数据
                        if (!mp3_buffer.empty()) {
                            // 将剩余的原始音频数据推送到解码队列
                            {
                                std::lock_guard<std::mutex> lock(raw_audio_mutex_);
                                if (raw_audio_queue_.size() < 20) {
                                    raw_audio_queue_.push_back(std::move(mp3_buffer));
                                    ESP_LOGD(TAG, "Added remaining raw audio data to decoder queue");
                                }
                            }
                            raw_audio_cv_.notify_all();
                            mp3_buffer.clear();
                        }
                        
                        stream_ended_normally = true;
                        break; // 退出读取循环
                    } else {
                        // 读取错误
                        consecutive_empty_reads++;
                        ESP_LOGW(TAG, "HTTP read error (bytes_read=%d), consecutive errors: %d", bytes_read, consecutive_empty_reads);
                        
                        if (consecutive_empty_reads >= max_empty_reads) {
                            ESP_LOGE(TAG, "Too many consecutive read errors, stopping music stream");
                            break;
                        }
                        
                        vTaskDelay(pdMS_TO_TICKS(100)); // 错误时等待更长时间
                    }
                }
                
                ESP_LOGI(TAG, "Music stream ended, total bytes read: %d, ended normally: %s", 
                         total_bytes_read, stream_ended_normally ? "yes" : "no");
                http->Close();
                
                // 如果流正常结束，停止音乐播放
                if (stream_ended_normally && music_playing_) {
                    ESP_LOGI(TAG, "Music stream completed normally, stopping playback");
                    StopMusic();
                } else if (music_playing_) {
                    // 如果流异常结束但仍在播放状态，等待后重试
                    ESP_LOGI(TAG, "Music stream ended abnormally, waiting before retry...");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
            } else {
                ESP_LOGE(TAG, "Failed to connect to MP3 stream: %s", current_music_url_.c_str());
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10)); // 减少检查间隔，提高响应性
    }
}

std::vector<int16_t> AudioService::DecodeM4aChunk(const std::vector<uint8_t>& m4a_data) {
    std::vector<int16_t> pcm_data;
    
    if (m4a_data.empty()) {
        return pcm_data;
    }
    
    // 初始化M4A解码器（如果尚未初始化）
    if (!m4a_decoder_) {
        m4a_decoder_ = std::make_unique<M4aDecoder>();
        if (!m4a_decoder_->Initialize(44100, codec_->output_channels())) {  // 使用标准44.1kHz采样率
            ESP_LOGE(TAG, "Failed to initialize M4A decoder");
            m4a_decoder_.reset();
            return pcm_data;
        }
        ESP_LOGI(TAG, "M4A decoder initialized successfully");
    }
    
    // 使用真正的M4A解码器解码数据
    pcm_data = m4a_decoder_->DecodeChunk(m4a_data);
    
    if (!pcm_data.empty()) {
        ESP_LOGD(TAG, "M4A decode: %zu bytes -> %zu PCM samples", 
                 m4a_data.size(), pcm_data.size());
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

void AudioService::M4aPcmDataTask() {
    ESP_LOGI(TAG, "M4A PCM data task started");
    
    // 等待M4A解码器完全启动，避免开头跳过音乐
    ESP_LOGI(TAG, "Waiting for M4A decoder to fully initialize...");
    vTaskDelay(pdMS_TO_TICKS(3000)); // 等待2秒让解码器完全启动
    
    // 计算音频播放的时间间隔
    // 假设输出采样率是44.1kHz，每个样本16位，2通道
    const int output_sample_rate = 44100;
    const int channels = 2;
    const int bytes_per_sample = 2; // 16位 = 2字节
    const int bytes_per_frame = channels * bytes_per_sample; // 2通道 * 2字节 = 4字节/帧
    
    // 计算每次读取4096字节对应的播放时间（毫秒）
    const int buffer_size = 4096;
    const int samples_per_read = buffer_size / bytes_per_frame; // 4096 / 4 = 1024样本
    const int playback_time_ms = (samples_per_read * 1000) / output_sample_rate; // 1024 * 1000 / 44100 ≈ 23ms
    
    ESP_LOGI(TAG, "M4A data timing: %d bytes = %d samples = %d ms playback time", 
             buffer_size, samples_per_read, playback_time_ms);
    
    while (music_playing_) {
        if (m4a_decoder_) {
            // 检查队列状态，如果队列太满就暂停数据生产
            {
                std::lock_guard<std::mutex> lock(audio_playback_mutex_);
                if (audio_playback_queue_.size() > 2500) {  // 进一步提高队列阈值，减少暂停
                    vTaskDelay(pdMS_TO_TICKS(10)); // 增加暂停时间，让播放跟上
                    continue;
                }
            }
            
            // 从M4A解码器获取PCM数据
            std::vector<int16_t> pcm_data = m4a_decoder_->GetPcmData();
            if (!pcm_data.empty()) {
                // 直接进行重采样处理，简化流程
                std::vector<int16_t> processed_pcm = std::move(pcm_data);
                
                // 重采样：从44100Hz到24000Hz（1.8375:1下采样）- 使用简单低通滤波减少杂声
                if (processed_pcm.size() >= 4) {
                    std::vector<int16_t> downsampled_pcm;
                    const float ratio = 44100.0f / 24000.0f; // 1.8375
                    const int output_samples = processed_pcm.size() / ratio;
                    downsampled_pcm.reserve(output_samples);
                    
                    // 简单的3点移动平均低通滤波，减少高频杂声
                    for (int i = 0; i < output_samples; i++) {
                        float source_index = i * ratio;
                        int index = (int)source_index;
                        
                        if (index >= 1 && index < processed_pcm.size() - 1) {
                            // 使用3点移动平均进行简单低通滤波
                            int32_t sum = (int32_t)processed_pcm[index - 1] + 
                                         (int32_t)processed_pcm[index] + 
                                         (int32_t)processed_pcm[index + 1];
                            int16_t filtered = sum / 3;
                            downsampled_pcm.push_back(filtered);
                        } else if (index < processed_pcm.size()) {
                            // 边界情况，直接使用样本
                            downsampled_pcm.push_back(processed_pcm[index]);
                        }
                    }
                    
                    processed_pcm = std::move(downsampled_pcm);
                    ESP_LOGD(TAG, "Downsampled to %zu samples (44100Hz -> 24000Hz) with 3-point low-pass filter", processed_pcm.size());
                }
                
                // 推送到统一的音频播放队列
                {
                    std::lock_guard<std::mutex> lock(audio_playback_mutex_);
                    if (audio_playback_queue_.size() < 2000) {  // 进一步增加队列大小到2000
                        audio_playback_queue_.push_back(std::move(processed_pcm));
                        ESP_LOGD(TAG, "Added M4A PCM data to queue, queue size: %zu", 
                                audio_playback_queue_.size());
                    } else {
                        // 队列满了，跳过一些数据以减少丢包
                        static int skip_count = 0;
                        skip_count++;
                        if (skip_count % 10 == 0) {  // 每10次丢包才记录一次日志，减少日志输出
                            ESP_LOGW(TAG, "Audio playback queue full, dropping M4A PCM data (dropped %d times)", skip_count);
                        }
                    }
                }
                audio_playback_cv_.notify_all();  // 通知音频输出任务处理新数据
            }
        }
        
        // 控制数据生产速度，避免队列积压
        // 根据队列状态动态调整延迟
        {
            std::lock_guard<std::mutex> lock(audio_playback_mutex_);
            if (audio_playback_queue_.size() > 1500) {
                vTaskDelay(pdMS_TO_TICKS(100)); // 队列很满时大幅增加延迟
            } else if (audio_playback_queue_.size() > 1000) {
                vTaskDelay(pdMS_TO_TICKS(50));  // 队列较满时增加延迟
            } else if (audio_playback_queue_.size() > 500) {
                vTaskDelay(pdMS_TO_TICKS(20));  // 队列中等时中等延迟
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));  // 队列较空时最小延迟
            }
        }
    }
    
    ESP_LOGI(TAG, "M4A PCM data task ended");
}

// 音频解码生产者任务 - 专门负责解码音频数据
void AudioService::AudioDecoderTask() {
    ESP_LOGI(TAG, "Audio decoder task started");
    
    while (!service_stopped_) {
        // 如果正在播放音乐，暂停多线程处理，避免冲突
        if (music_playing_) {
            vTaskDelay(pdMS_TO_TICKS(100)); // 暂停100ms
            continue;
        }
        std::unique_lock<std::mutex> lock(raw_audio_mutex_);
        
        // 等待原始音频数据或服务停止
        raw_audio_cv_.wait(lock, [this]() { 
            return !raw_audio_queue_.empty() || service_stopped_; 
        });
        
        if (service_stopped_) {
            break;
        }
        
        if (!raw_audio_queue_.empty()) {
            // 获取原始音频数据
            auto raw_data = std::move(raw_audio_queue_.front());
            raw_audio_queue_.pop_front();
            lock.unlock();
            
            // 解码音频数据
            std::vector<int16_t> pcm_data;
            if (current_music_url_.find(".wav") != std::string::npos || 
                (raw_data.size() >= 4 && memcmp(raw_data.data(), "RIFF", 4) == 0)) {
                ESP_LOGD(TAG, "Decoding WAV data in decoder task");
                pcm_data = DecodeWavChunk(raw_data);
            } else if (current_music_url_.find(".m4a") != std::string::npos) {
                ESP_LOGD(TAG, "Decoding M4A data in decoder task");
                pcm_data = DecodeM4aChunk(raw_data);
            } else {
                ESP_LOGD(TAG, "Using M4A decoder as fallback in decoder task");
                pcm_data = DecodeM4aChunk(raw_data);
            }
            
            // 将解码后的PCM数据推送到处理队列
            if (!pcm_data.empty()) {
                std::lock_guard<std::mutex> pcm_lock(processed_pcm_mutex_);
                if (processed_pcm_queue_.size() < 50) {  // 限制处理队列大小
                    processed_pcm_queue_.push_back(std::move(pcm_data));
                    ESP_LOGD(TAG, "Added decoded PCM to processing queue, size: %zu", 
                            processed_pcm_queue_.size());
                } else {
                    ESP_LOGW(TAG, "Processed PCM queue full, dropping data");
                }
            }
            processed_pcm_cv_.notify_all();
        }
        
        // 最小延迟，保持高响应性
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    ESP_LOGI(TAG, "Audio decoder task ended");
}

// 缓冲区管理任务 - 负责管理音频数据流和重采样
void AudioService::AudioBufferManagerTask() {
    ESP_LOGI(TAG, "Audio buffer manager task started");
    
    while (!service_stopped_) {
        // 如果正在播放音乐，暂停多线程处理，避免冲突
        if (music_playing_) {
            vTaskDelay(pdMS_TO_TICKS(100)); // 暂停100ms
            continue;
        }
        std::unique_lock<std::mutex> lock(processed_pcm_mutex_);
        
        // 等待处理后的PCM数据或服务停止
        processed_pcm_cv_.wait(lock, [this]() { 
            return !processed_pcm_queue_.empty() || service_stopped_; 
        });
        
        if (service_stopped_) {
            break;
        }
        
        if (!processed_pcm_queue_.empty()) {
            // 获取处理后的PCM数据
            auto pcm_data = std::move(processed_pcm_queue_.front());
            processed_pcm_queue_.pop_front();
            lock.unlock();
            
            // 在这里进行重采样和其他音频处理
            std::vector<int16_t> processed_pcm = std::move(pcm_data);
            
            // 获取AAC解码器的实际输出采样率信息
            int actual_sample_rate = 44100;  // 默认值，M4A通常是44.1kHz
            int actual_channels = 2;         // 默认值
            
            // 尝试从M4A解码器获取实际的采样率信息
            if (m4a_decoder_) {
                // 从AAC解码器获取音频信息
                audio_element_info_t info;
                if (audio_element_getinfo(m4a_decoder_->GetDecoderElement(), &info) == ESP_OK) {
                    actual_sample_rate = info.sample_rates;
                    actual_channels = info.channels;
                    ESP_LOGI(TAG, "AAC decoder actual output: %d Hz, %d channels", actual_sample_rate, actual_channels);
                }
            }
            
            // 转换为单声道（使用左声道）
            std::vector<int16_t> mono_pcm_data;
            if (actual_channels == 2) {
                // 立体声转单声道，取左声道
                int mono_samples = processed_pcm.size() / 2;
                mono_pcm_data.resize(mono_samples);
                for (int i = 0; i < mono_samples; i++) {
                    mono_pcm_data[i] = processed_pcm[i * 2]; // 取左声道
                }
                ESP_LOGI(TAG, "Converted stereo to mono: %d samples -> %d samples", processed_pcm.size(), mono_pcm_data.size());
            } else {
                // 已经是单声道，直接使用
                mono_pcm_data = std::move(processed_pcm);
                ESP_LOGD(TAG, "Already mono: %d samples", mono_pcm_data.size());
            }
            
            const int target_sample_rate = 12000; // 音频的实际输出速度，由系统决定
            
            if (actual_sample_rate != target_sample_rate) {
                // 计算重采样比例（单声道）
                float ratio = (float)actual_sample_rate / target_sample_rate;
        
                int output_samples = mono_pcm_data.size() / ratio;
                std::vector<int16_t> resampled_pcm(output_samples);
                
                // 使用3点移动平均低通滤波进行重采样，减少杂声
                for (int i = 0; i < output_samples; i++) {
                    int source_index = i * ratio;
                    
                    if (source_index >= 1 && source_index < mono_pcm_data.size() - 1) {
                        // 使用3点移动平均进行简单低通滤波
                        int32_t sum = (int32_t)mono_pcm_data[source_index - 1] + 
                                     (int32_t)mono_pcm_data[source_index] + 
                                     (int32_t)mono_pcm_data[source_index + 1];
                        resampled_pcm[i] = sum / 3;
                    } else if (source_index < mono_pcm_data.size()) {
                        // 边界情况，直接使用样本
                        resampled_pcm[i] = mono_pcm_data[source_index];
                    }
                }
                ESP_LOGI(TAG, "Downsampled %d mono samples to %d samples (%dHz -> %dHz, step=%.2f)", 
                        mono_pcm_data.size(), resampled_pcm.size(), actual_sample_rate, target_sample_rate, ratio);
                processed_pcm = std::move(resampled_pcm);
            } else {
                // 比例接近1，直接使用单声道数据
                processed_pcm = std::move(mono_pcm_data);
                ESP_LOGI(TAG, "Got %d mono PCM samples at %dHz (ratio≈1, no resampling)", 
                         processed_pcm.size(), actual_sample_rate);
            }
            
            // 推送到播放队列
            {
                std::lock_guard<std::mutex> playback_lock(audio_playback_mutex_);
                if (audio_playback_queue_.size() < 2000) {  // 大幅增加播放队列大小
                    audio_playback_queue_.push_back(std::move(processed_pcm));
                    ESP_LOGD(TAG, "Added processed PCM to playback queue, size: %zu", 
                            audio_playback_queue_.size());
                } else {
                    ESP_LOGW(TAG, "Playback queue full, dropping processed PCM data");
                }
            }
            audio_playback_cv_.notify_all();
        }
        
        // 最小延迟，保持高响应性
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    ESP_LOGI(TAG, "Audio buffer manager task ended");
}