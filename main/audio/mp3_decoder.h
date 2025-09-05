#ifndef MP3_DECODER_H
#define MP3_DECODER_H

#include <vector>
#include <cstdint>

class Mp3Decoder {
public:
    Mp3Decoder();
    ~Mp3Decoder();

    bool Initialize(int sample_rate, int channels);
    void Deinitialize();
    
    std::vector<int16_t> DecodeChunk(const std::vector<uint8_t>& mp3_data);

private:
    bool initialized_;
    int sample_rate_;
    int channels_;
    
    // 简单的MP3解码器实现
    // 注意：这是一个占位符实现，实际项目中应该使用真正的MP3解码库
    std::vector<uint8_t> mp3_buffer_;
    
    static const char* TAG;
};

#endif // MP3_DECODER_H
