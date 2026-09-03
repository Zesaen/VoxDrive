// AudioPlayer — ALSA 播放（16kHz 单声道 S16LE）
// 设备名读 conf tts.alsa_device（缺省 "default"，Jetson 上通常为 HDMI/3.5mm 输出）。
#define VOX_LOG_TAG "tts"
#include "vox_config.h"
#include "vox_log.h"

#include "AudioPlayer.h"

AudioPlayer::AudioPlayer()
{
    initialize();
}

AudioPlayer::~AudioPlayer()
{
    cleanup();
}

bool AudioPlayer::initialize()
{
    if (initialized_)
        return true;

    const std::string device = vox::config::get("tts.alsa_device", "default");
    int err = snd_pcm_open(&pcm_handle_, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
    {
        VOX_ERROR("打开 ALSA 设备 %s 失败: %s（检查 conf tts.alsa_device）",
                  device.c_str(), snd_strerror(err));
        return false;
    }
    VOX_INFO("ALSA 设备就绪: %s", device.c_str());
    initialized_ = true;
    return true;
}

void AudioPlayer::play(const int16_t *audioData, int audio_len, float speed)
{
    if (!initialized_ || !pcm_handle_)
    {
        VOX_WARN("播放跳过（设备未初始化）");
        return;
    }

    unsigned int sample_rate = static_cast<unsigned int>(16000 * speed);
    int err = snd_pcm_set_params(pcm_handle_,
                                 SND_PCM_FORMAT_S16_LE,
                                 SND_PCM_ACCESS_RW_INTERLEAVED,
                                 1,
                                 sample_rate,
                                 1,
                                 50000);
    if (err < 0)
    {
        VOX_ERROR("ALSA set_params 失败: %s", snd_strerror(err));
        return;
    }

    const snd_pcm_uframes_t frames = audio_len / 2;
    const int max_retries = 3;
    int retry_count = 0;

    while (true)
    {
        err = snd_pcm_writei(pcm_handle_, audioData, frames);
        if (err == -EPIPE)
        {
            if (++retry_count >= max_retries)
                break;
            VOX_WARN("ALSA underrun，重试 %d/%d", retry_count, max_retries);
            snd_pcm_prepare(pcm_handle_);
        }
        else if (err < 0)
        {
            VOX_ERROR("ALSA writei 失败: %s", snd_strerror(err));
            break;
        }
        else
        {
            break;
        }
    }

    if (snd_pcm_state(pcm_handle_) == SND_PCM_STATE_RUNNING)
    {
        snd_pcm_drain(pcm_handle_);
    }
}

void AudioPlayer::cleanup()
{
    if (pcm_handle_)
    {
        snd_pcm_close(pcm_handle_);
        pcm_handle_ = nullptr;
    }
    initialized_ = false;
}
