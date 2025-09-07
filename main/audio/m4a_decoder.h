#ifndef M4A_DECODER_H
#define M4A_DECODER_H

#include <vector>
#include <cstdint>
#include <memory>

// ESP-ADF includes
#include "audio_element.h"
#include "audio_pipeline.h"
#include "codec/aac_decoder.h"

// Forward declaration
class OpusResampler;

class M4aDecoder {
public:
    M4aDecoder();
    ~M4aDecoder();

    bool Initialize(int sample_rate, int channels);
    void Deinitialize();
    
    bool PlayUrl(const std::string& url);
    void Stop();
    
    std::vector<int16_t> DecodeChunk(const std::vector<uint8_t>& m4a_data);
    std::vector<int16_t> GetPcmData(); // 从M4A解码器获取PCM数据
    
    // 设置重采样器
    void SetResampler(int input_sample_rate, int output_sample_rate);
    
    // 获取解码器元素句柄
    audio_element_handle_t GetDecoderElement() const { return m4a_decoder_; }

private:
    bool initialized_;
    int sample_rate_;
    int channels_;
    
    // ESP-ADF音频管道组件
    audio_pipeline_handle_t pipeline_;
    audio_element_handle_t m4a_decoder_;
    audio_element_handle_t http_stream_;
    audio_element_handle_t i2s_stream_;
    
    // M4A数据缓冲区
    std::vector<uint8_t> m4a_buffer_;
    
    // 输出缓冲区
    std::vector<int16_t> output_buffer_;
    
    // ESP-ADF音频缓冲区
    ringbuf_handle_t output_ringbuf_;
    
    // 重采样器
    std::unique_ptr<OpusResampler> resampler_;
    
    static const char* TAG;
};

#endif // M4A_DECODER_H
