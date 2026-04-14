#include "WASAPISampler.hpp"

#include "Platform.hpp"

class MMNotificationClientCallback : public IMMNotificationClient
{
public:
    SamplerState* samplerState;

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, _COM_Outptr_ void __RPC_FAR* __RPC_FAR* object) override
    {
        if (IID_IUnknown == riid)
        {
            *object = (IUnknown*)this;
        }
        else if (__uuidof(IMMNotificationClient) == riid)
        {
            *object = (IMMNotificationClient*)this;
        }
        else
        {
            *object = NULL;
            return E_NOINTERFACE;
        }
        return S_OK;
    }

    virtual ULONG STDMETHODCALLTYPE AddRef() override
    {
	    return 1;
    }

    virtual ULONG STDMETHODCALLTYPE Release() override
    {
	    return 1;
    }
    
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR /*pwstrDeviceId*/, DWORD /*dwNewState*/) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*pwstrDeviceId*/) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole /*role*/, LPCWSTR /*pwstrDefaultDeviceId*/)
    {
        if (flow == eRender)
        {
            samplerState->needReset = true;
        }
        return S_OK;
    }
};

void SamplerCreateDevice(SamplerState* state, uint16_t channelCount, uint32_t sampleRate)
{
    state->needReset = false;

    IMMDevice* audioDevice;
    if (FAILED(state->deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &audioDevice)))
    {
        Warning("Failed to get the default audio endpoint!");
    }

    state->deviceEnumerator->Release();

    if (FAILED(audioDevice->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, 0, (LPVOID*)(&state->audioClient))))
    {
        Warning("Failed to activate the device!");
    }
    
    audioDevice->Release();

    // WAVEFORMATEX* defaultMixFormat = NULL;
    // hr = audioClient->GetMixFormat(&defaultMixFormat);
    // assert(hr == S_OK);

    state->channelCount = channelCount;
    state->sampleRate = sampleRate;
    state->bytesPerSample = 4;

    WAVEFORMATEX mixFormat = {};
    mixFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    mixFormat.nChannels = state->channelCount;
    mixFormat.nSamplesPerSec = state->sampleRate;//defaultMixFormat->nSamplesPerSec;
    mixFormat.wBitsPerSample = state->bytesPerSample * 8;
    mixFormat.nBlockAlign = (mixFormat.nChannels * mixFormat.wBitsPerSample) / 8;
    mixFormat.nAvgBytesPerSec = mixFormat.nSamplesPerSec * mixFormat.nBlockAlign;
    
    REFERENCE_TIME requestedSoundBufferDuration = (REFERENCE_TIME)(10000000 / 2);
    DWORD initStreamFlags =
        (AUDCLNT_STREAMFLAGS_RATEADJUST
        | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
        | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);
    if (FAILED(state->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, initStreamFlags, requestedSoundBufferDuration, 0, &mixFormat, 0)))
    {
        Warning("Failed to initialize the audio client!");
        return;
    }

    if (FAILED(state->audioClient->GetService(__uuidof(IAudioRenderClient), (LPVOID*)(&state->audioRenderClient))))
    {
        Warning("Failed to get audio render client service!");
        return;
    }

    if (FAILED(state->audioClient->GetBufferSize(&state->bufferSize)))
    {
        Warning("Failed to get audio renderer buffer size!");
        return;
    }

    if (FAILED(state->audioClient->GetService(__uuidof(IAudioClock), (void**)&state->audioClock)))
    {
        Warning("Failed to get audio clock service!");
        return;
    }

    if (FAILED(state->audioClock->GetFrequency(&state->clockFrequency)))
    {
        Warning("Failed to get audio clock frequency!");
        return;
    }
}

void SamplerCloseDevice(SamplerState* state)
{
    state->audioClient->Stop();
    state->audioRenderClient->Release();
    state->audioClient->Release();
}

void SamplerInitialize(SamplerState* state)
{
    (*state) = {};

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (LPVOID*)(&state->deviceEnumerator))))
    {
        Warning("Failed to create a MMDeviceEnumerator!");
    }

    state->notifyCallback = new MMNotificationClientCallback();
    state->notifyCallback->samplerState = state;
    
    state->deviceEnumerator->RegisterEndpointNotificationCallback(state->notifyCallback);

    SamplerCreateDevice(state, 2, 48000);
}

void SamplerSetNewFormat(SamplerState* state, uint16_t channelCount, uint32_t sampleRate)
{
    SamplerCloseDevice(state);
    SamplerCreateDevice(state, channelCount, sampleRate);
}

SamplerOutput SamplerBeginOutput(SamplerState* state)
{
    if (state->needReset)
    {
        SamplerCloseDevice(state);
        SamplerCreateDevice(state, state->channelCount, state->sampleRate);
    }
    
    UINT32 bufferPadding;
    if (FAILED(state->audioClient->GetCurrentPadding(&bufferPadding)))
    {
        Warning("GetCurrentPadding failed!");
    }

    UINT32 frameCount = state->bufferSize - bufferPadding;

    float* buffer;
    if (state->audioRenderClient->GetBuffer(frameCount, (BYTE**)(&buffer)) != S_OK)
    {
        Warning("GetBuffer failed!");
    }

    SamplerOutput output;
    output.frameCount = frameCount;
    output.buffer = buffer;

    return output;
}

void SamplerEndOutput(SamplerState* state, SamplerOutput output)
{
    if (state->audioRenderClient->ReleaseBuffer(output.frameCount, 0) != S_OK)
    {
        Warning("ReleaseBuffer failed!");
    }
}

uint64_t SamplerGetCurrentTick(SamplerState* state)
{
    UINT64 audioFramesPlayed = 0;
    
    //Note: WASAPI gives you the exact QPC time the frame count was read. 
    //You can use this for ultra-precise micro-interpolation, but for standard sync, 
    //audioFramesPlayed is what you need.
    if (FAILED(state->audioClock->GetPosition(&audioFramesPlayed, 0)))
    {
        Warning("Audio clock GetPosition fail!");
    }

    return audioFramesPlayed;
}

void SamplerPause(SamplerState* state)
{
    state->audioClient->Stop();
}

void SamplerContinue(SamplerState* state)
{
    state->audioClient->Start();
}

void SamplerClear(SamplerState* state)
{
    state->audioClient->Reset();
}