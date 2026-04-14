#pragma once

#include <mmdeviceapi.h>
#include <Audioclient.h>

#include <stdint.h>

float PCM8ToFloat(int8_t pcm)
{
    float f = ((float)pcm) / (float)127;
    if(f >  1.0f) f =  1.0f;
    if(f < -1.0f) f = -1.0f;

    return f;
}

float PCM16ToFloat(int16_t pcm)
{
    float f = ((float)pcm) / (float)32768;
    if(f >  1.0f) f =  1.0f;
    if(f < -1.0f) f = -1.0f;

    return f;
}

//Input expected in the lower 24 bits, while upper 8 has to be 0
float PCM24ToFloat(int32_t pcm)
{
    float f = ((float)pcm) / (float)8388608;
    if(f >  1.0f) f =  1.0f;
    if(f < -1.0f) f = -1.0f;

    return f;
}

float PCM32ToFloat(int32_t pcm)
{
    float f = ((float)pcm) / (float)2147483648;
    if(f >  1.0f) f =  1.0f;
    if(f < -1.0f) f = -1.0f;

    return f;
}

int16_t FloatToPCM16(float f)
{
    f = f * 32768.0f;
    if(f > 32767.0f) f = 32767.0f;
    if(f < -32768.0f) f = -32768.0f;

    int16_t i = (int16_t)f;
    return i;
}

struct SamplerOutput
{
    UINT32 frameCount;
    float* buffer;
};

class MMNotificationClientCallback;

struct SamplerState
{
    uint16_t channelCount;
    uint32_t sampleRate;
    uint16_t bytesPerSample;

    IMMDeviceEnumerator* deviceEnumerator;
    MMNotificationClientCallback* notifyCallback;
    IAudioClient2* audioClient;
    IAudioRenderClient* audioRenderClient;
    IAudioClock* audioClock;
    uint64_t clockFrequency = 0;
    UINT32 bufferSize;

    bool needReset;
};