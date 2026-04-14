#pragma once

#include "Platform.hpp"

#define DR_FLAC_IMPLEMENTATION
#include "dr_libs/dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_libs/dr_mp3.h"

#include <stdint.h>
#include <stdlib.h>

enum WAVE_FORMAT
{
    WAVE_FORMAT_PCM = 0x0001,
    WAVE_FORMAT_IEEE_FLOAT = 0x0003,
    WAVE_FORMAT_ALAW = 0x0006,
    WAVE_FORMAT_MULAW = 0x0007,
    WAVE_FORMAT_EXTENSIBLE = 0xFFFE
};

enum AudioFileType
{
    AudioFileType_Unknown = 0,
    AudioFileType_WAV,
    AudioFileType_FLAC,
    AudioFileType_MP3
};

struct WAVLoadTemp
{
    WAVE_FORMAT format;
    uint16_t bitsPerSample;
};

struct WAVFile
{
    AudioFileType type;
    uint16_t channelCount;
    uint32_t sampleRate;
    uint64_t samplesPerChannel;
    float* samples;
};

//Returns how much to advance
uint32_t ProcessWavChunk(WAVFile* result, WAVLoadTemp* temp, uint8_t* bytes)
{
    uint32_t advance = *((uint32_t*)(bytes + 4)) + 8;

    //Spec says if chunk size is odd there must be a padding byte so we are accounting for that here
    if (advance % 2 == 1)
    {
        advance++;
    }
    
    if (bytes[0] == 'f' && bytes[1] == 'm' && bytes[2] == 't' && bytes[3] == ' ')
    {
        bytes += 8; //Skip name (4 bytes) and chunk size (4 bytes)

        temp->format = (WAVE_FORMAT)(*((uint16_t*)bytes));
        bytes += 2;
        
        //in wav files channels are always interleaved in data
        result->channelCount = (*((uint16_t*)bytes));
        bytes += 2;

        result->sampleRate = (*((uint32_t*)bytes));
        bytes += 4;
        
        //uint32_t avgBytesPerSec = (*((uint32_t*)bytes));
        bytes += 4;
        
        //uint16_t blockAlign = (*((uint16_t*)bytes));
        bytes += 2;

        temp->bitsPerSample = (*((uint16_t*)bytes));
        bytes += 2;

        uint16_t extensionSize = (*((uint16_t*)bytes));
        bytes += 2;

        if (extensionSize >= 22)
        {
            //uint16_t validBitsPerSample = (*((uint16_t*)bytes));
            bytes += 2;
            
            //Speaker position mask
            //uint32_t channelMask = (*((uint32_t*)bytes));
            bytes += 4;
        }
    }
    else if (bytes[0] == 'J' && bytes[1] == 'U' && bytes[2] == 'N' && bytes[3] == 'K')
    {
        bytes += 8; //Skip name (4 bytes) and chunk size (4 bytes)
    }
    else if (bytes[0] == 's' && bytes[1] == 'm' && bytes[2] == 'p' && bytes[3] == 'l')
    {
        bytes += 8; //Skip name (4 bytes) and chunk size (4 bytes)
    }
    else if (bytes[0] == 'L' && bytes[1] == 'I' && bytes[2] == 'S' && bytes[3] == 'T')
    {
        bytes += 8; //Skip name (4 bytes) and chunk size (4 bytes)
    }
    else if (bytes[0] == 'f' && bytes[1] == 'a' && bytes[2] == 'c' && bytes[3] == 't')
    {
        bytes += 8; //Skip name (4 bytes) and chunk size (4 bytes)
        
        //Some value here that the spec doesn't explain well, it's some samples per channel, but why would we need that?
        bytes += 4;
    }
    else if (bytes[0] == 'd' && bytes[1] == 'a' && bytes[2] == 't' && bytes[3] == 'a')
    {
        bytes += 4; //Skip name (4 bytes)

        uint32_t dataSize = *((uint32_t*)bytes);
        bytes += 4;

        uint32_t bytesPerSample = temp->bitsPerSample / 8;
        result->samplesPerChannel = dataSize / bytesPerSample / result->channelCount;
        result->samples = (float*)malloc((dataSize / bytesPerSample) * sizeof(float));

        uint32_t writer = 0;
        for (uint32_t i = 0; i < result->samplesPerChannel; i++)
        {
            for (uint32_t c = 0; c < result->channelCount; c++)
            {
                float amplitude = 0.0f;

                switch (temp->format)
                {
                case WAVE_FORMAT_PCM:
                {
                    if (temp->bitsPerSample == 8)
                    {
                        int8_t pcm = *((int8_t*)bytes);
                        amplitude = ((float)pcm) / (float)INT8_MAX;

                        bytes += 1;
                    }
                    else if (temp->bitsPerSample == 16)
                    {
                        int16_t pcm = *((int16_t*)bytes);
                        amplitude = ((float)pcm) / (float)INT16_MAX;

                        bytes += 2;
                    }
                    else if (temp->bitsPerSample == 24)
                    {
                        amplitude = (bytes[0] << 8 | bytes[1] << 16 | bytes[2] << 24) / 2147483648.0f;

                        bytes += 3;
                    }
                    else if (temp->bitsPerSample == 32)
                    {
                        int32_t pcm = *((int32_t*)bytes);
                        amplitude = ((float)pcm) / (float)INT32_MAX;

                        bytes += 4;
                    }
                    else
                    {
                        Warning("Unkown wav pcm sample format!");
                        return advance;
                    }

                    if(amplitude > 1.0f)
                    {
                        amplitude = 1.0f;
                    }
                    if(amplitude < -1.0f)
                    {
                        amplitude = -1.0f;
                    }

                } break;

                case WAVE_FORMAT_IEEE_FLOAT:
                {
                    if (temp->bitsPerSample == 32)
                    {
                        amplitude = *((float*)bytes);
                        bytes += 4;
                    }
                    else if (temp->bitsPerSample == 64)
                    {
                        amplitude = (float)(*((double*)bytes));
                        bytes += 8;
                    }
                    else
                    {
                        Warning("Unkown wav float sample format!");
                        return advance;
                    }

                } break;
                
                default:
                    Warning("Unkown wav sample format!");
                    return advance;
                }

                result->samples[writer++] = amplitude;
            }
        }
    }
    else if ((bytes[0] == 'i' && bytes[1] == 'd' && bytes[2] == '3' && bytes[3] == ' ') ||
        (bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3' && bytes[3] == ' '))
    {
        //id3 chunk is not standard in wav files
    }
    else
    {
        Warning("Unrecognized chunk in WAV file!");
    }

    return advance;
}

void ReadWAV(WAVFile* result, void* buffer)
{
    result->type = AudioFileType_WAV;

    //RIFF chunk
    uint8_t* bytes = (uint8_t*)buffer;

    if (bytes[0] != 'R' || bytes[1] != 'I' || bytes[2] != 'F' || bytes[3] != 'F')
    {
        Warning("wav riff identifier invalid!");
        return;
    }
    bytes += 4;
    
    uint8_t* end = bytes + *((uint32_t*)bytes);
    bytes += 4;
    
    //"WAVE"
    bytes += 4;

    WAVLoadTemp temp = {};
    while (bytes < end)
    {
        bytes += ProcessWavChunk(result, &temp, bytes);
    }
}

void DeleteAudioFile(WAVFile* audioFile)
{
    switch (audioFile->type)
    {
    case AudioFileType_WAV:
    {
        if (audioFile->samples)
        {
            free(audioFile->samples);
        }

    } break;
    
    case AudioFileType_FLAC:
    {
        if (audioFile->samples)
        {
            drflac_free(audioFile->samples, 0);
        }

    } break;
    
    case AudioFileType_MP3:
    {
        if (audioFile->samples)
        {
            drmp3_free(audioFile->samples, 0);
        }

    } break;

    default:
        break;
    }

    audioFile->samples = 0;
}

void ReadAudioFile(AudioFileType fileType, WAVFile* result, void* buffer, size_t bufferSize)
{
    switch (fileType)
    {
    case AudioFileType_WAV:
        ReadWAV(result, buffer);
        break;
        
    case AudioFileType_FLAC:
    {
        uint32_t channelCount;
        uint32_t sampleRate;
        uint64_t pcmFrameCount;
        result->samples = drflac_open_memory_and_read_pcm_frames_f32(buffer, bufferSize, &channelCount, &sampleRate, &pcmFrameCount, 0);
        
        result->channelCount = (uint16_t)channelCount;
        result->sampleRate = sampleRate;
        result->samplesPerChannel = pcmFrameCount;

    } break;
    
    case AudioFileType_MP3:
    {
        drmp3_config config;
        uint64_t pcmFrameCount;
        result->samples = drmp3_open_memory_and_read_pcm_frames_f32(buffer, bufferSize, &config, &pcmFrameCount, 0);

        result->channelCount = (uint16_t)config.channels;
        result->sampleRate = config.sampleRate;
        result->samplesPerChannel = pcmFrameCount;

    } break;
    
    default:
        *result = {};
        break;
    }
}