#ifndef ESP_ADF_MP3_PLAYER_H
#define ESP_ADF_MP3_PLAYER_H

#include <string>
#include <memory>
#include <esp_log.h>

// ESP-ADF includes
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "codec/mp3_decoder.h"

class EspAdfMp3Player {
public:
    EspAdfMp3Player();
    ~EspAdfMp3Player();

    bool Initialize(int sample_rate = 44100, int channels = 2);
    void Deinitialize();
    
    bool PlayUrl(const std::string& url);
    void Stop();
    void Pause();
    void Resume();
    
    bool IsPlaying() const { return playing_; }
    bool IsInitialized() const { return initialized_; }
    
    int GetSampleRate() const { return sample_rate_; }
    int GetChannels() const { return channels_; }

private:
    bool initialized_;
    bool playing_;
    int sample_rate_;
    int channels_;
    
    // ESP-ADF pipeline components
    audio_pipeline_handle_t pipeline_;
    audio_element_handle_t http_stream_reader_;
    audio_element_handle_t mp3_decoder_;
    audio_element_handle_t i2s_stream_writer_;
    audio_event_iface_handle_t evt_;
    
    bool CreatePipeline();
    void DestroyPipeline();
    void HandleAudioEvents();
    
    static const char* TAG;
};

#endif // ESP_ADF_MP3_PLAYER_H
