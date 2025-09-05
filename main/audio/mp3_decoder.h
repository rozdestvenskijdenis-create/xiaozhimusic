#ifndef MP3_DECODER_H
#define MP3_DECODER_H

#include <vector>
#include <cstdint>
#include <memory>

// ESP-ADF includes
#include "audio_element.h"
#include "audio_pipeline.h"
#include "codec/mp3_decoder.h"

class Mp3Decoder {
public:
    Mp3Decoder();
    ~Mp3Decoder();

    bool Initialize(int sample_rate, int channels);
    void Deinitialize();
    
    bool PlayUrl(const std::string& url);
    void Stop();
    
    std::vector<int16_t> DecodeChunk(const std::vector<uint8_t>& mp3_data);

private:
    bool initialized_;
    int sample_rate_;
    int channels_;
    
    // ESP-ADF音频管道组件
    audio_pipeline_handle_t pipeline_;
    audio_element_handle_t mp3_decoder_;
    audio_element_handle_t http_stream_;
    audio_element_handle_t i2s_stream_;
    
    // MP3数据缓冲区
    std::vector<uint8_t> mp3_buffer_;
    
    // 输出缓冲区
    std::vector<int16_t> output_buffer_;
    
    static const char* TAG;
};

#endif // MP3_DECODER_H
