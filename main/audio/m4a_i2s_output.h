#ifndef M4A_I2S_OUTPUT_H
#define M4A_I2S_OUTPUT_H

#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <cstring>

/**
 * 专门用于M4A音频播放的22050Hz I2S输出类
 * 解决重采样导致的音频播放不完整问题
 */
class M4aI2sOutput {
public:
    M4aI2sOutput();
    ~M4aI2sOutput();

    /**
     * 初始化22050Hz I2S输出
     * @param bclk_pin I2S位时钟管脚
     * @param lrclk_pin I2S左右声道时钟管脚  
     * @param dout_pin I2S数据输出管脚
     * @param enable_pin 功放使能管脚（可选）
     * @return 初始化是否成功
     */
    bool Initialize(gpio_num_t bclk_pin, gpio_num_t lrclk_pin, gpio_num_t dout_pin, gpio_num_t enable_pin = GPIO_NUM_NC);

    /**
     * 反初始化I2S输出
     */
    void Deinitialize();

    /**
     * 开始播放PCM数据
     * @param pcm_data PCM数据指针
     * @param data_size 数据大小（字节）
     * @return 是否成功开始播放
     */
    bool PlayPcmData(const int16_t* pcm_data, size_t data_size);

    /**
     * 停止播放
     */
    void Stop();

    /**
     * 检查是否正在播放
     */
    bool IsPlaying() const { return playing_; }

    /**
     * 获取采样率
     */
    int GetSampleRate() const { return 44100; }

    /**
     * 获取通道数
     */
    int GetChannels() const { return 2; }

private:
    static const char* TAG;
    static const int SAMPLE_RATE = 44100;
    static const int CHANNELS = 2;
    static const int BITS_PER_SAMPLE = 16;
    static const int BUFFER_SIZE = 8192; // 8KB缓冲区，提高连续性

    i2s_chan_handle_t tx_handle_;
    gpio_num_t enable_pin_;
    bool initialized_;
    bool playing_;
    QueueHandle_t pcm_queue_;
    TaskHandle_t playback_task_;

    /**
     * 播放任务
     */
    static void PlaybackTask(void* arg);
    void ProcessPlayback();

    /**
     * 启用/禁用功放
     */
    void EnableAmplifier(bool enable);
};

#endif // M4A_I2S_OUTPUT_H
