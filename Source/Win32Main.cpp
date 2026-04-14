//TODO:
//  - Open source, publish
//  - Make work as CLAP plugin
//  - Load ogg files?

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Dwmapi.lib")

#include "WAV.cpp"
#include "WASAPISampler.cpp"

//#include PLATFORM
#include <Windows.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <dwmapi.h>

#pragma warning( push )
#pragma warning( disable : 4456 )
#include "pffft/pffft.h"
#include "pffft/pffft.c"
#pragma warning( pop )

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb/stb_rect_pack.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <float.h>

struct AudioWaveBar
{
    uint32_t minY;
    uint32_t maxY;
};

struct ReferenceInfo
{
    uint32_t bucketCount;
    float averageDb;
    float* lowerAmplitudes;
    float* upperAmplitudes;
};

struct Win32State
{
    bool running;

    uint32_t renderWidth;
    uint32_t renderHeight;

    HBITMAP frame_bitmap;
    HDC frame_device_context;

    HWND window;
    uint32_t* pixels;

    uint32_t scissorMinX;
    uint32_t scissorMinY;
    uint32_t scissorMaxX;
    uint32_t scissorMaxY;

    bool lmbDown;
    bool lmbRelease;
    int16_t mouseX;
    int16_t mouseY;
    int16_t mouseScroll;

    uint32_t titleOriginX;
    uint32_t titleOriginY;

    uint32_t* gradientLookup;
    ReferenceInfo reference;
    AudioWaveBar* audioWaveBars;
    int32_t referenceListOffsetY;
    uint32_t targetReferenceCount;
    wchar_t targetReferences[1024][MAX_PATH];

    uint32_t topBarHeight;
    uint32_t bottomBarHeight;

    SamplerState sampler;
    uint64_t lastSamplerTick = 0;
    bool isPlaying;
    bool isSettingTime;
    uint64_t audioWriterIndex;
    uint64_t audioPlayingIndex;
    WAVFile wav;

    bool referenceSelectorOpen;
};

Win32State* win32State;

int utf8Codepoints[] =
{
    32,    33,
    34,    35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
    45,    46,    47,    48,    49,    50,    51,    52,    53,    54,    55,
    56,    57,    58,    59,    60,    61,    62,    63,    64,    65,    66,
    67,    68,    69,    70,    71,    72,    73,    74,    75,    76,    77,
    78,    79,    80,    81,    82,    83,    84,    85,    86,    87,    88,
    89,    90,    91,    92,    93,    94,    95,    96,    97,    98,    99,
    100,   101,   102,   103,   104,   105,   106,   107,   108,   109,   110,
    111,   112,   113,   114,   115,   116,   117,   118,   119,   120,   121,
    122,   123,   124,   125,   126
};

stbtt_packedchar packedCharacters[_countof(utf8Codepoints)] = {};
uint32_t fontImageWidth = 512 * 4;
uint32_t fontImageHeight = 512 * 4;
uint8_t* fontPixels = 0;
int32_t charMinX = 0;
int32_t charMaxX = 0;
int32_t charMinY = 0;
int32_t charMaxY = 0;

#define HexColor(hex) { (uint8_t)(((hex) >> 16) & 0xFF), (uint8_t)(((hex) >> 8) & 0xFF), (uint8_t)((hex) & 0xFF) }

uint8_t colors[][3] =
{
    HexColor(0x230f66),
    HexColor(0x3c1c96),
    HexColor(0x4820b7),
    HexColor(0x5627da),
    HexColor(0x5a2bed),
    HexColor(0x775bf7),
    HexColor(0x988afb),
    HexColor(0xbbb5fd),
    HexColor(0xd8d6fe),
    HexColor(0xeae9fe),
    HexColor(0xf3f3ff)
};

const float MIN_DB = -200.0f;
const float MIN_AMP = powf(10.0f, MIN_DB / 20.0f);
const float MIN_POWER = MIN_AMP * MIN_AMP;
const float VISUALIZER_MIN_DB = -100.0f;

#define ArrayCount(array) (sizeof(array) / sizeof(array[0]))

#include <intrin.h>
#define Assert(cond) do { if (!(cond)) __debugbreak(); } while (0);

void Warning(const char* message)
{
    MessageBoxA(0, message, "Warning!", MB_OK | MB_ICONEXCLAMATION);
}

float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

uint8_t Lerp(uint8_t a, uint8_t b, float t)
{
    return a + (uint8_t)((float)(b - a) * t);
}

AudioFileType GetAudioFileTypeFromPath(wchar_t* filepath)
{
    AudioFileType aft = AudioFileType_Unknown;
    wchar_t* extension = PathFindExtensionW(filepath);
    
    if (wcscmp(extension, L".wav") == 0 || wcscmp(extension, L".wave") == 0)
    {
        aft = AudioFileType_WAV;
    }
    else if (wcscmp(extension, L".flac") == 0 || wcscmp(extension, L".oga") == 0)
    {
        aft = AudioFileType_FLAC;
    }
    else if (wcscmp(extension, L".mp3") == 0)
    {
        aft = AudioFileType_MP3;
    }

    return aft;
}

uint32_t PackColor(uint8_t r, uint8_t g, uint8_t b)
{
    return (b & 0xFF) | (g & 0xFF) << 8 | (r & 0xFF) << 16;
}

void UnpackColor(uint32_t packed, uint8_t* outR, uint8_t* outG, uint8_t* outB)
{
    *outR = (uint8_t)((packed >> 16) & 0xFF);
    *outG = (uint8_t)((packed >> 8) & 0xFF);
    *outB = (uint8_t)(packed & 0xFF);
}

void DrawRect(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY,
    uint8_t r, uint8_t g, uint8_t b)
{
    if (minX < win32State->scissorMinX)
    {
        minX = win32State->scissorMinX;
    }
    
    if (minY < win32State->scissorMinY)
    {
        minY = win32State->scissorMinY;
    }
    
    if (maxX > win32State->scissorMaxX)
    {
        maxX = win32State->scissorMaxX;
    }
    
    if (maxY > win32State->scissorMaxY)
    {
        maxY = win32State->scissorMaxY;
    }
    
    for (uint32_t py = minY; py < maxY; py++)
    {
        for (uint32_t px = minX; px < maxX; px++)
        {
            uint32_t val = (b & 0xFF) | (g & 0xFF) << 8 | (r & 0xFF) << 16;
            win32State->pixels[py * win32State->renderWidth + px] = val;
        }
    }
}

int GetNextUTF8Codepoint(const char* text, int *codepointSize)
{
    const char *ptr = text;
    int codepoint = '?';
    *codepointSize = 1;
    if (text == 0) return codepoint;

    if (0xf0 == (0xf8 & ptr[0]))
    {
        // 4 byte UTF-8 codepoint
        if (((ptr[1] & 0xC0) ^ 0x80) || ((ptr[2] & 0xC0) ^ 0x80) || ((ptr[3] & 0xC0) ^ 0x80)) { return codepoint; } // 10xxxxxx checks
        codepoint = ((0x07 & ptr[0]) << 18) | ((0x3f & ptr[1]) << 12) | ((0x3f & ptr[2]) << 6) | (0x3f & ptr[3]);
        *codepointSize = 4;
    }
    else if (0xe0 == (0xf0 & ptr[0]))
    {
        // 3 byte UTF-8 codepoint */
        if (((ptr[1] & 0xC0) ^ 0x80) || ((ptr[2] & 0xC0) ^ 0x80)) { return codepoint; } // 10xxxxxx checks
        codepoint = ((0x0f & ptr[0]) << 12) | ((0x3f & ptr[1]) << 6) | (0x3f & ptr[2]);
        *codepointSize = 3;
    }
    else if (0xc0 == (0xe0 & ptr[0]))
    {
        // 2 byte UTF-8 codepoint
        if ((ptr[1] & 0xC0) ^ 0x80) { return codepoint; } // 10xxxxxx checks
        codepoint = ((0x1f & ptr[0]) << 6) | (0x3f & ptr[1]);
        *codepointSize = 2;
    }
    else if (0x00 == (0x80 & ptr[0]))
    {
        codepoint = ptr[0];
        *codepointSize = 1;
    }

    return codepoint;
}

//TODO: New line and such
void MeasureText(char* utf8String, float* outX, float* outY)
{
    float xWriter = 0.0f;
    float yWriter = 0.0f;

    for (size_t i = 0; utf8String[i];)
    {
        int advance;
        int codepoint = GetNextUTF8Codepoint(&utf8String[i], &advance);

        uint32_t characterIndex = 0;
        while (utf8Codepoints[characterIndex] != codepoint)
        {
            characterIndex++;

            if (characterIndex >= _countof(utf8Codepoints))
            {
                characterIndex = UINT32_MAX;
                break;
            }
        }

        if (characterIndex == UINT32_MAX)
        {
            i += advance;
            continue;
        }
        
        stbtt_aligned_quad quad;
        stbtt_GetPackedQuad(packedCharacters, fontImageWidth, fontImageHeight, characterIndex, &xWriter, &yWriter, &quad, 0);

        i += advance;
    }

    *outX = xWriter;
    *outY = yWriter;
}

//TODO: New line and such
void RenderText(char* utf8String, uint32_t x, uint32_t y)
{
    float xWriter = (float)x;
    float yWriter = (float)y;

    for (size_t i = 0; utf8String[i];)
    {
        int advance;
        int codepoint = GetNextUTF8Codepoint(&utf8String[i], &advance);

        int characterIndex = 0;
        while (utf8Codepoints[characterIndex] != codepoint)
        {
            characterIndex++;

            if (characterIndex >= _countof(utf8Codepoints))
            {
                characterIndex = -1;
                break;
            }
        }

        if (characterIndex == -1)
        {
            i += advance;
            continue;
        }
        
        stbtt_aligned_quad quad;
        stbtt_GetPackedQuad(packedCharacters, fontImageWidth, fontImageHeight, characterIndex, &xWriter, &yWriter, &quad, 0);

        for (float iy = quad.y0; iy < quad.y1; iy += 1.0f)
        {
            for (int32_t px = (int32_t)roundf(quad.x0); px < (int32_t)roundf(quad.x1); px++)
            {
                int32_t py = (int32_t)roundf(yWriter - (iy - yWriter));

                if (px > (int32_t)win32State->scissorMaxX || px < (int32_t)win32State->scissorMinX ||
                    py > (int32_t)win32State->scissorMaxY || py < (int32_t)win32State->scissorMinY)
                {
                    continue;
                }
                
                float u =
                    ((float)(px - quad.x0) / (float)(quad.x1 - quad.x0) * (quad.s1 - quad.s0) + quad.s0) * (float)fontImageWidth;
                float v =
                    ((float)(iy - quad.y0) / (float)(quad.y1 - quad.y0) * (quad.t1 - quad.t0) + quad.t0) * (float)fontImageHeight;

                uint16_t u0 = (uint16_t)u;
                uint16_t u1 = u0 + 1;
                uint16_t v0 = (uint16_t)v;
                uint16_t v1 = v0 + 1;

                uint8_t br00 = fontPixels[v0 * fontImageWidth + u0];
                uint8_t br10 = fontPixels[v0 * fontImageWidth + u1];
                uint8_t br01 = fontPixels[v1 * fontImageWidth + u0];
                uint8_t br11 = fontPixels[v1 * fontImageWidth + u1];

                float xt = v - (float)v0;
                uint8_t blended0 = Lerp(br00, br01, xt);
                uint8_t blended1 = Lerp(br10, br11, xt);

                uint8_t fontBrightness = Lerp(blended0, blended1, u - (float)u0);

                uint8_t r;
                uint8_t g;
                uint8_t b;
                UnpackColor(win32State->pixels[py * win32State->renderWidth + px], &r, &g, &b);

                float t = (float)fontBrightness / 255.0f;
                r = Lerp(r, colors[10][0], t);
                g = Lerp(g, colors[10][1], t);
                b = Lerp(b, colors[10][2], t);

                win32State->pixels[py * win32State->renderWidth + px] = PackColor(r, g, b);
            }
        }

        i += advance;
    }
}

void RenderCenteredText(char* utf8String, uint32_t x, uint32_t y)
{
    float sizeX;
    float sizeY;
    MeasureText(utf8String, &sizeX, &sizeY);
    RenderText(utf8String, x - (uint32_t)sizeX / 2, y);
}

void rgb2hsv(uint8_t r, uint8_t g, uint8_t b, float* outH, float* outS, float* outV)
{
    float fR = (float)r / 255.0f; 
    float fG = (float)g / 255.0f; 
    float fB = (float)b / 255.0f; 

    float fCMax = max(max(fR, fG), fB);
    float fCMin = min(min(fR, fG), fB);
    float fDelta = fCMax - fCMin;

    if(fDelta > 0)
    {
        if(fCMax == fR)
        {
            *outH = 60.0f * (fmodf(((fG - fB) / fDelta), 6.0f));
        }
        else if(fCMax == fG)
        {
            *outH = 60.0f * (((fB - fR) / fDelta) + 2.0f);
        }
        else if(fCMax == fB)
        {
            *outH = 60.0f * (((fR - fG) / fDelta) + 4.0f);
        }

        if(fCMax > 0)
        {
            *outS = fDelta / fCMax;
        }
        else
        {
            *outS = 0.0f;
        }

        *outV = fCMax;
    }
    else
    {
        *outH = 0.0f;
        *outS = 0.0f;
        *outV = fCMax;
    }

    if(*outH < 0.0f)
    {
        *outH = 360.0f + (*outH);
    }
}

void HSVtoRGB(float& fR, float& fG, float& fB, float& fH, float& fS, float& fV)
{
    float fC = fV * fS; // Chroma
    float fHPrime = fmodf(fH / 60.0f, 6);
    float fX = fC * (1 - fabsf(fmodf(fHPrime, 2.0f) - 1));
    float fM = fV - fC;
    
    if(0.0f <= fHPrime && fHPrime < 1.0f)
    {
        fR = fC;
        fG = fX;
        fB = 0.0f;
    }
    else if(1 <= fHPrime && fHPrime < 2)
    {
      fR = fX;
      fG = fC;
      fB = 0;
    } else if(2 <= fHPrime && fHPrime < 3) {
      fR = 0;
      fG = fC;
      fB = fX;
    } else if(3 <= fHPrime && fHPrime < 4) {
      fR = 0;
      fG = fX;
      fB = fC;
    } else if(4 <= fHPrime && fHPrime < 5) {
      fR = fX;
      fG = 0;
      fB = fC;
    } else if(5 <= fHPrime && fHPrime < 6) {
      fR = fC;
      fG = 0;
      fB = fX;
    } else {
      fR = 0;
      fG = 0;
      fB = 0;
    }
    
    fR += fM;
    fG += fM;
    fB += fM;
}

void HSVLerp(uint8_t aR, uint8_t aG, uint8_t aB, uint8_t bR, uint8_t bG, uint8_t bB, float t,
    uint8_t* outR, uint8_t* outG, uint8_t* outB)
{
    float aH;
    float aS;
    float aV;
    rgb2hsv(aR, aG, aB, &aH, &aS, &aV);
    
    float bH;
    float bS;
    float bV;
    rgb2hsv(bR, bG, bB, &bH, &bS, &bV);

    float resultH = Lerp(aH, bH, t);
    float resultS = Lerp(aS, bS, t);
    float resultV = Lerp(aV, bV, t);

    float fR;
    float fG;
    float fB;
    HSVtoRGB(fR, fG, fB, resultH, resultS, resultV);

    *outR = (uint8_t)(fR * 255.0f);
    *outG = (uint8_t)(fG * 255.0f);
    *outB = (uint8_t)(fB * 255.0f);
}

float CubicInterpolate(float p0, float p1, float p2, float p3, float t)
{
    float t2 = t * t;
    float t3 = t2 * t;

    return 0.5f *
    (
        (2.0f * p1) +
        (-p0 + p2) * t +
        (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
        (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3
    );
}

float GetFreqDecibels(uint32_t bucketIndex, uint32_t bucketCount,
    float* amplitudes, uint32_t sampleRate, int fftWindowSize, float minFrequency, float maxFrequency)
{
    float binWidthHz = (float)sampleRate / (float)fftWindowSize;

    float tLeft = (float)bucketIndex / (float)bucketCount;
    float freqLeft = minFrequency * powf((maxFrequency / minFrequency), tLeft);
    float tRight = (float)(bucketIndex + 1) / (float)bucketCount;
    float freqRight = minFrequency * powf((maxFrequency / minFrequency), tRight);

    float exactBinStart = freqLeft / binWidthHz;
    float exactBinEnd   = freqRight / binWidthHz;
    
    int binStartIndex = (int)(exactBinStart);
    int binEndIndex   = (int)(exactBinEnd);

    if (binStartIndex < 1)
    {
        binStartIndex = 1; // Skip DC offset
    }
    if (binEndIndex > fftWindowSize / 2)
    {
        binEndIndex = fftWindowSize / 2;
    }
    if (binStartIndex > binEndIndex)
    {
        binStartIndex = binEndIndex;
    }

    float binsInPixel = exactBinEnd - exactBinStart;

    float totalPower = 0.0f;
    if (binsInPixel < 1.0f)
    {
        //This pixel sits inside a single bin. We interpolate with the next bin 
        //to create a smooth slope instead of a flat stair-step.
        
        float centerBin = (exactBinStart + exactBinEnd) * 0.5f;
        int b1 = (int)centerBin;

        //We need 4 points for a cubic spline: b0, b1, b2, b3
        int b0 = b1 - 1;
        int b2 = b1 + 1;
        int b3 = b1 + 2;

        if (b0 < 1)
        {
            b0 = 1;
        }
        if (b1 < 1)
        {
            b1 = 1;
        }
        if (b2 > fftWindowSize / 2)
        {
            b2 = fftWindowSize / 2;
        }
        if (b3 > fftWindowSize / 2)
        {
            b3 = fftWindowSize / 2;
        }

        //How far along are we between bin 1 and bin 2? (0.0 to 1.0)
        float fraction = exactBinStart - b1; 
        
        float amp0 = amplitudes[b0];
        float amp1 = amplitudes[b1];
        float amp2 = amplitudes[b2];
        float amp3 = amplitudes[b3];
        
        float interpolatedAmp = CubicInterpolate(amp0, amp1, amp2, amp3, fraction);
        
        //CRITICAL: Cubic splines can "overshoot" and dip below zero if the curve is sharp.
        //We must clamp it so we don't calculate a negative amplitude!
        if (interpolatedAmp < 0.0f)
        {
            interpolatedAmp = 0.0f;
        }

        totalPower = interpolatedAmp * interpolatedAmp;
    }
    else
    {
        //This pixel covers multiple bins. Find the loudest peak to prevent aliasing.
        float maxAmp = 0.0f;
        for (int b = binStartIndex; b < binEndIndex; ++b)
        {
            if (b > fftWindowSize / 2)
            {
                break;
            }

            if (amplitudes[b] > maxAmp)
            {
                maxAmp = amplitudes[b];
            }
        }

        totalPower = maxAmp * maxAmp;
    }

    float safePower = max(totalPower, MIN_POWER);
    float decibels = 10.0f * log10f(safePower);
    
    return decibels;
}

void DeleteReferenceInfo(ReferenceInfo* info)
{
    if (info->lowerAmplitudes)
    {
        free(info->lowerAmplitudes);
    }
    
    if (info->upperAmplitudes)
    {
        free(info->upperAmplitudes);
    }
}

//filepaths are seperated with null, double null for end
void CalculateReferenceCurve(ReferenceInfo* info, wchar_t* filepaths, size_t fileCount, size_t filepathTableWidth,
    int fftWindowSize, float minFrequency, float maxFrequency, uint32_t numFreqBuckets)
{
    DeleteReferenceInfo(info);

    int numBins = fftWindowSize / 2;

    info->bucketCount = numFreqBuckets;
    info->lowerAmplitudes = (float*)malloc(numBins * sizeof(float));
    float* averageAmplitudes = (float*)malloc(numBins * sizeof(float));
    info->upperAmplitudes = (float*)malloc(numBins * sizeof(float));
    
    float* sumDb = (float*)malloc(numBins * sizeof(float));
    ZeroMemory(sumDb, numBins * sizeof(float));
    float* sumDbSquared = (float*)malloc(numBins * sizeof(float));
    ZeroMemory(sumDbSquared, numBins * sizeof(float));

    PFFFT_Setup* fft = pffft_new_setup(fftWindowSize, PFFFT_REAL);
    Assert(fft && "n incorrect number!");
    uint32_t amplitudesCount = fftWindowSize / 2 + 1;
    float* amplitudes = (float*)malloc(amplitudesCount * sizeof(float));

    size_t singleBufferSize = fftWindowSize * sizeof(float);
    singleBufferSize = (singleBufferSize + MALLOC_V4SF_ALIGNMENT) - ((size_t)singleBufferSize & (MALLOC_V4SF_ALIGNMENT - 1));

    void* memory = malloc(singleBufferSize * 3 + MALLOC_V4SF_ALIGNMENT);
    ZeroMemory(memory, singleBufferSize * 3 + MALLOC_V4SF_ALIGNMENT);
    void* alignedMemory = (void *) (((char*)memory + MALLOC_V4SF_ALIGNMENT) - (((size_t)memory) & (MALLOC_V4SF_ALIGNMENT - 1)));
    float* input  = (float*)alignedMemory;
    float* output = (float*)((uint8_t*)alignedMemory + singleBufferSize);
    float* work   = (float*)((uint8_t*)alignedMemory + 2 * singleBufferSize);

    int numChunks = 0;

    for (size_t f = 0; f < fileCount; f++)
    {
        wchar_t* pathStart = &filepaths[f * filepathTableWidth];
        HANDLE file = CreateFileW(pathStart, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
        
        if (file == INVALID_HANDLE_VALUE)
        {
            Warning("Failed to open file!");
        }
        else
        {
            uint32_t sizeHigh;
            uint32_t sizeLow =  GetFileSize(file, (LPDWORD)&sizeHigh);
            uint64_t fileSize = (uint64_t)sizeLow | ((uint64_t)sizeHigh << 32);

            void* fileData = malloc(fileSize);

            uint64_t bytesToRead = fileSize;

            while (bytesToRead > 0)
            {
                uint32_t readSize = (uint32_t)bytesToRead;
                if (bytesToRead > UINT32_MAX)
                {
                    readSize = UINT32_MAX;
                }
                
                DWORD bytesRead;
                if (ReadFile(file, fileData, readSize, &bytesRead, 0) == FALSE)
                {
                    Warning("Failed to read file!");
                    free(fileData);
                    fileData = 0;
                    break;
                }

                bytesToRead -= bytesRead;
            }
            
            if (fileData)
            {
                DrawRect(0, win32State->bottomBarHeight,
                    win32State->renderWidth,
                    win32State->renderHeight - win32State->topBarHeight,
                    0, 0, 0);

                RenderCenteredText("Analyzing:", win32State->renderWidth / 2, win32State->renderHeight / 2 + (charMaxY - charMinY));
                
                int pathLen = (int)wcslen(pathStart);
                int size_needed = WideCharToMultiByte(CP_UTF8, 0, pathStart, pathLen, NULL, 0, NULL, NULL);
                char* utf8String = (char*)malloc(size_needed + 1);
                ZeroMemory(utf8String, size_needed + 1);
                WideCharToMultiByte(CP_UTF8, 0, pathStart, pathLen, utf8String, size_needed, NULL, NULL);

                RenderCenteredText(utf8String, win32State->renderWidth / 2, win32State->renderHeight / 2);
                    
                free(utf8String);
                    
                InvalidateRect(win32State->window, NULL, FALSE);
                UpdateWindow(win32State->window);

                WAVFile wav;
                ReadAudioFile(GetAudioFileTypeFromPath(pathStart), &wav, fileData, fileSize);

                for (size_t offset = 0; offset <= wav.samplesPerChannel - fftWindowSize / 2; offset += fftWindowSize / 4)
                {
                    for (int i = 0; i < fftWindowSize / 2; ++i)
                    {
                        float monoAmp = 0.0f;
                        for (uint32_t c = 0; c < wav.channelCount; c++)
                        {
                            monoAmp += wav.samples[(offset + i) * wav.channelCount + c] / wav.channelCount;
                        }
                        
                        float t = ((float)i / (float)(fftWindowSize / 2 - 1));
                        float hannWindow = 0.5f * (1.0f - cosf(t * (2.0f * (float)M_PI)));
                        input[i] = monoAmp * hannWindow;
                    }
            
                    pffft_transform_ordered(fft, input, output, work, PFFFT_FORWARD);

                    //Handle the special cases first (DC and Nyquist are purely real)
                    //Should be squared and then square rooted but thats equal to absolute value
                    amplitudes[0] = fabsf(output[0]) / fftWindowSize * 4.0f;
                    amplitudes[amplitudesCount - 1] = fabsf(output[1]) / fftWindowSize * 4.0f;

                    //Handle the rest of the bins (extract real and imaginary parts)
                    for (uint32_t i = 1; i < amplitudesCount - 1; ++i)
                    {
                        float re = output[2 * i];
                        float im = output[2 * i + 1];
                        
                        //Amplitude = sqrt((real^2) + (imaginary^2)) (phytagorean theorem)
                        //Multiply with 2 because zero padding halves the power of every frequency
                        //Multiply with 2 again because hann window halves the power of every frequency
                        amplitudes[i] = sqrtf((re * re) + (im * im)) / (fftWindowSize / 2) * 4.0f;
                    }

                    //Now, instead of looping through FFT bins, we loop through our BUCKETS
                    for (uint32_t b = 0; b < numFreqBuckets; ++b)
                    {   
                        float db = GetFreqDecibels(b, numFreqBuckets,
                            amplitudes, wav.sampleRate, fftWindowSize, minFrequency, maxFrequency);
                        
                        sumDb[b] += db;
                        sumDbSquared[b] += (db * db);
                    }
                    
                    numChunks++;
                }

                DeleteAudioFile(&wav);
                free(fileData);
            }

            CloseHandle(file);
        }
    }
    
    if (numChunks > 0)
    {
        for (int b = 1; b < numBins; ++b)
        {
            float avgDb = sumDb[b] / (float)numChunks;

            float avgOfSquares = sumDbSquared[b] / (float)numChunks;
            float variance = avgOfSquares - (avgDb * avgDb);
            
            if (variance < 0.0f)
            {
                variance = 0.0f;
            }
            
            float stdDevDb = sqrtf(variance);

            float upperDb = avgDb + stdDevDb;
            float lowerDb = avgDb - stdDevDb;

            //db = 20 * log10(amp)
            //db / 20 = log10(amp)
            //10^(db / 20) = amp
            averageAmplitudes[b] = powf(10.0f, avgDb / 20.0f);
            info->upperAmplitudes[b]   = powf(10.0f, upperDb / 20.0f);
            info->lowerAmplitudes[b]   = powf(10.0f, lowerDb / 20.0f);
        }
    }

    float totalReferencePower = 0.0f;
    for (int i = 0; i < numBins; i++)
    {
        float amp = averageAmplitudes[i];
        totalReferencePower += amp * amp;
    }

    float avarageReferencePower = totalReferencePower / (float)numBins;
    float safeReferencePower = max(avarageReferencePower, MIN_POWER);
    info->averageDb = 10.0f * log10f(safeReferencePower);

    free(sumDb);
    free(sumDbSquared);

    free(memory);
    pffft_destroy_setup(fft);

    free(averageAmplitudes);

    free(amplitudes);
}

#define TYPE_SERIALIZER(type) void Serialize(FILE* file, bool isWriting, type* value, uint32_t count) \
{ \
    if (isWriting) \
    { \
        size_t writeCount = fwrite(value, sizeof(type), count, file); \
        Assert(writeCount == count); \
    } \
    else \
    { \
        size_t readCount = fread(value, sizeof(type), count, file); \
        Assert(readCount == count); \
    } \
}

TYPE_SERIALIZER(uint32_t)
TYPE_SERIALIZER(float)

void SerializeReferenceInfo(wchar_t* filepath, ReferenceInfo* info, bool isWriting)
{
    const wchar_t* mode = isWriting ? L"wb" : L"rb";

    FILE* file;
    if (_wfopen_s(&file, filepath, mode) != 0)
    {
        Warning("Failed to open reference file!");
        return;
    }
    
    Serialize(file, isWriting, &info->averageDb, 1);
    Serialize(file, isWriting, &info->bucketCount, 1);

    if (!isWriting)
    {
        DeleteReferenceInfo(info);
        info->lowerAmplitudes = (float*)malloc(info->bucketCount * sizeof(float));
        info->upperAmplitudes = (float*)malloc(info->bucketCount * sizeof(float));
    }

    Serialize(file, isWriting, info->lowerAmplitudes, info->bucketCount);
    Serialize(file, isWriting, info->upperAmplitudes, info->bucketCount);

    fclose(file);
}

void CalculateAudioWaveBars(WAVFile* wav, uint32_t renderWidth)
{
    if (win32State->audioWaveBars)
    {
        free(win32State->audioWaveBars);
    }
    
    win32State->audioWaveBars = (AudioWaveBar*)malloc(renderWidth * sizeof(AudioWaveBar));

    float samplesPerPixel = (float)wav->samplesPerChannel / (float)renderWidth;

    uint32_t waveMinPoint = 0;

    float exactSampleIndex = 0.0f;
    for (uint32_t x = 0; x < renderWidth; x++)
    {
        uint64_t minSampleIndex = (uint32_t)exactSampleIndex;
        uint64_t maxSampleIndex = (uint32_t)(exactSampleIndex + samplesPerPixel);

        Assert(minSampleIndex < win32State->wav.samplesPerChannel);

        if (maxSampleIndex > win32State->wav.samplesPerChannel)
        {
            maxSampleIndex = win32State->wav.samplesPerChannel;
        }
        
        float minVal = 0.0f;
        float maxVal = 0.0f;
        for (uint64_t i = minSampleIndex; i < maxSampleIndex; i++)
        {
            float monoAmp = 0.0f;
            for (uint32_t c = 0; c < win32State->wav.channelCount; c++)
            {
                monoAmp += win32State->wav.samples[i * win32State->wav.channelCount + c] / win32State->wav.channelCount;
            }
            
            if (maxVal < monoAmp)
            {
                maxVal = monoAmp;
            }
            
            if (minVal > monoAmp)
            {
                minVal = monoAmp;
            }
        }

        if (minVal < -1.0f)
        {
            minVal = -1.0f;
        }
        
        if (maxVal > 1.0f)
        {
            maxVal = 1.0f;
        }
        
        win32State->audioWaveBars[x].minY =
            (uint32_t)((minVal * 0.5f + 0.5f) * (float)win32State->bottomBarHeight + (float)waveMinPoint);
        win32State->audioWaveBars[x].maxY =
            (uint32_t)((maxVal * 0.5f + 0.5f) * (float)win32State->bottomBarHeight + (float)waveMinPoint);
        
        exactSampleIndex += samplesPerPixel;
    }
}

void HandleTimeSet(int16_t x)
{
    bool needRestart = false;
    if (win32State->isPlaying)
    {
        SamplerPause(&win32State->sampler);
        needRestart = true;
    }
    
    if (x < 0)
    {
        x = 0;
    }

    if ((int16_t)win32State->renderWidth < x)
    {
        x = (int16_t)win32State->renderWidth;
    }
    
    SamplerClear(&win32State->sampler);
    win32State->lastSamplerTick = 0;

    float t = (float)x / (float)win32State->renderWidth;
    uint32_t sampleIndex = (uint32_t)(t * win32State->wav.samplesPerChannel);

    win32State->audioPlayingIndex = sampleIndex;
    win32State->audioWriterIndex = sampleIndex;

    if (needRestart)
    {
        SamplerContinue(&win32State->sampler);
    }
}

static LRESULT CALLBACK WindowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_SIZE:
    {
        if (LOWORD(lparam) == 0 || HIWORD(lparam) == 0)
        {
            //Woah
            break;
        }
        
        win32State->renderWidth = LOWORD(lparam);
        win32State->renderHeight = HIWORD(lparam);

        win32State->scissorMinX = 0;
        win32State->scissorMinY = 0;
        win32State->scissorMaxX = LOWORD(lparam);
        win32State->scissorMaxY = HIWORD(lparam);

        BITMAPINFO frame_bitmap_info;
        frame_bitmap_info.bmiHeader.biSize = sizeof(frame_bitmap_info.bmiHeader);
        frame_bitmap_info.bmiHeader.biPlanes = 1;
        frame_bitmap_info.bmiHeader.biBitCount = 32;
        frame_bitmap_info.bmiHeader.biCompression = BI_RGB;
        frame_bitmap_info.bmiHeader.biWidth  = LOWORD(lparam);
        frame_bitmap_info.bmiHeader.biHeight = HIWORD(lparam);

        if(win32State->frame_bitmap)
        {
            DeleteObject(win32State->frame_bitmap);
        }
        win32State->frame_bitmap = CreateDIBSection(NULL, &frame_bitmap_info, DIB_RGB_COLORS, (void**)&win32State->pixels, 0, 0);
        
        if (win32State->frame_bitmap == 0)
        {
            Warning("Failed to create screen buffer!");
        }
        
        SelectObject(win32State->frame_device_context, win32State->frame_bitmap);

        win32State->topBarHeight = 64;
        win32State->bottomBarHeight = 64;

        int16_t metersMinY = (int16_t)win32State->bottomBarHeight;
        int16_t metersMaxY = HIWORD(lparam) - (int16_t)win32State->topBarHeight;

        if (win32State->gradientLookup)
        {
            free(win32State->gradientLookup);
        }
        
        int16_t gradientCount = metersMaxY - metersMinY;
        win32State->gradientLookup = (uint32_t*)malloc(gradientCount * sizeof(uint32_t));
        for (int16_t y = 0; y < gradientCount; y++)
        {
            float exactColorIndex = ((float)y / (float)gradientCount) * (float)(_countof(colors) - 1);
            uint32_t prevColorIndex = (uint32_t)(exactColorIndex);
            uint32_t nextColorIndex = prevColorIndex + 1;

            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            if (nextColorIndex >= _countof(colors))
            {
                r = colors[_countof(colors) - 1][0];
                g = colors[_countof(colors) - 1][1];
                b = colors[_countof(colors) - 1][2];
            }
            else
            {
                float t = exactColorIndex - (float)prevColorIndex;
                HSVLerp(
                    colors[prevColorIndex][0], colors[prevColorIndex][1], colors[prevColorIndex][2],
                    colors[nextColorIndex][0], colors[nextColorIndex][1], colors[nextColorIndex][2],
                    t,
                    &r, &g, &b);
            }

            uint32_t val = (b & 0xFF) | (g & 0xFF) << 8 | (r & 0xFF) << 16;
            win32State->gradientLookup[y] = val;
        }

        if (win32State->wav.samples)
        {
            CalculateAudioWaveBars(&win32State->wav, LOWORD(lparam));
        }
        
    } break;

    case WM_PAINT:
    {
        static PAINTSTRUCT paint;
        static HDC device_context;
        device_context = BeginPaint(win32State->window, &paint);
        BitBlt(device_context,
               paint.rcPaint.left, paint.rcPaint.top,
               paint.rcPaint.right - paint.rcPaint.left, paint.rcPaint.bottom - paint.rcPaint.top,
               win32State->frame_device_context,
               paint.rcPaint.left, paint.rcPaint.top,
               SRCCOPY);
        EndPaint(win32State->window, &paint);
    } break;

    case WM_SETFOCUS:
    {

    } break;

    case WM_KILLFOCUS:
        break;

    case WM_LBUTTONDOWN:
    {
        win32State->lmbDown = true;

        int16_t x = GET_X_LPARAM(lparam);
        int16_t y = GET_Y_LPARAM(lparam);
        y = (int16_t)win32State->renderHeight - y;

        if (0 <= y && y < (int16_t)win32State->bottomBarHeight)
        {
            win32State->isSettingTime = true;
            HandleTimeSet(x);
            SetCapture(wnd);
        }

    } break;

    case WM_LBUTTONUP:
    {
        win32State->lmbDown = false;
        win32State->lmbRelease = true;

        win32State->isSettingTime = false;
        ReleaseCapture();

    } break;

    case WM_MOUSEMOVE:
    {
        int16_t x = GET_X_LPARAM(lparam);
        int16_t y = GET_Y_LPARAM(lparam);
        y = (int16_t)win32State->renderHeight - y;

        if (win32State->isSettingTime)
        {
            HandleTimeSet(x);
        }

        win32State->mouseX = x;
        win32State->mouseY = y;

    } break;

    case WM_MOUSEWHEEL:
    {
        win32State->mouseScroll += GET_WHEEL_DELTA_WPARAM(wparam);

    } break;

    case WM_KEYDOWN:
    {
        if (wparam == VK_SPACE)
        {
            win32State->isPlaying = !win32State->isPlaying;

            if (win32State->isPlaying)
            {
                SamplerContinue(&win32State->sampler);
            }
            else
            {
                SamplerPause(&win32State->sampler);
            }
        }
        
    } break;
    
    case WM_KEYUP:
    {
    } break;

    case WM_SETCURSOR:
        static HCURSOR cursor = LoadCursor(0, IDC_ARROW);
        SetCursor(cursor);
        return TRUE;

    case WM_DESTROY:
        win32State->running = false;
        break;
    }

    return DefWindowProcW(wnd, msg, wparam, lparam);
}

void DrawReferenceCurveAtX(float falloff, float* prevHeightRatio, int32_t x,
    int32_t renderWidth, int32_t renderHeight, int32_t metersMinY, int32_t metersMaxHeight,
    int b0, int b1, int b2, int b3, float fraction, float* amplitudeArray)
{
    int32_t extend = (int32_t)falloff + 2;

    float amp0 = amplitudeArray[b0];
    float amp1 = amplitudeArray[b1];
    float amp2 = amplitudeArray[b2];
    float amp3 = amplitudeArray[b3];
    
    float interpolatedAmp = CubicInterpolate(amp0, amp1, amp2, amp3, fraction);

    float safePower = max(interpolatedAmp * interpolatedAmp, MIN_POWER);
    float decibels = 10.0f * log10f(safePower);
    
    float yRatio = 1.0f - (decibels / VISUALIZER_MIN_DB);

    float aX = (float)x - 1.0f;
    float aY = (*prevHeightRatio) * metersMaxHeight + (float)metersMinY;
    float bX = (float)x;
    float bY = yRatio * metersMaxHeight + (float) metersMinY;

    int32_t startY = (int32_t)roundf(aY);
    int32_t endY = (int32_t)roundf(bY);

    int32_t yInc = 1;

    if (startY > endY)
    {
        yInc = -1;
        startY += extend;
        endY -= extend;
    }
    else
    {
        startY -= extend;
        endY += extend;
    }

    startY = max(startY, 0);
    endY = min(endY, (int32_t)renderHeight);
    
    int32_t minX = max((int32_t)roundf(aX) - extend, 0);
    int32_t maxX = min((int32_t)roundf(bX) + extend, (int32_t)renderWidth);

    for (int32_t py = startY; py * yInc <= endY * yInc; py += yInc)
    {
        for (int32_t px = minX; px <= maxX; px++)
        {
            float dist = 0.0f;

            //vec2 lineDir = {B.x - A.x, B.y - A.y};
            //vec2 pixelDir = {pixel.x - A.x, pixel.y - A.y};

            float dirX = bX - aX;
            float dirY = bY - aY;
            float pixelDirX = px - aX;
            float pixelDirY = py - aY;
            
            // Length squared of the line segment
            float lineLenSq = dirX * dirX + dirY * dirY;
            
            if (x > 0)
            {
                if (lineLenSq == 0.0f)
                {
                    // A and B are the same point
                    float dx = px - aX;
                    float dy = py - aY;
                    dist = sqrtf(dx * dx + dy * dy); 
                }
                else
                {
                    // Project pixel onto the line, clamping to [0, 1] to stay on the segment
                    float lt = (pixelDirX * dirX + pixelDirY * dirY) / lineLenSq;
                    
                    if (lt < 0.0f)
                    {
                        lt = 0.0f;
                    }
                    if (lt > 1.0f)
                    {
                        lt = 1.0f;
                    }
                    
                    // Find the closest point on the segment
                    //vec2 closestPoint = {A.x + t * lineDir.x, A.y + t * lineDir.y};
                    float closestX = aX + lt * dirX;
                    float closestY = aY + lt * dirY;
                    
                    // Return distance from pixel to closest point
                    float dx = px - closestX;
                    float dy = py - closestY;
                    dist = sqrtf(dx * dx + dy * dy);
                }
            }
            else
            {
                dist = bY - (float)py;
            }
            
            if (dist < falloff)
            {
                size_t index = py * win32State->renderWidth + px;

                uint8_t r;
                uint8_t g;
                uint8_t b;
                UnpackColor(win32State->pixels[index], &r, &g, &b);

                float br = (falloff - dist) / falloff;

                uint8_t brightness = (uint8_t)(br * 255.0f);

                if (r >= 255 - brightness)
                {
                    r = 255;
                }
                else
                {
                    r += brightness;
                }
                
                if (g >= 255 - brightness)
                {
                    g = 255;
                }
                else
                {
                    g += brightness;
                }
                
                if (b >= 255 - brightness)
                {
                    b = 255;
                }
                else
                {
                    b += brightness;
                }

                win32State->pixels[index] = PackColor(r, g, b);
            }
        }
    }
    
    *prevHeightRatio = yRatio;
}

bool UIButton(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY, char* text)
{
    bool press = false;
    float textWidth;
    float textHeight;
    MeasureText(text, &textWidth, &textHeight);
    
    if (minX < win32State->scissorMinX)
    {
        minX = win32State->scissorMinX;
    }
    
    if (minY < win32State->scissorMinY)
    {
        minY = win32State->scissorMinY;
    }
    
    if (maxX > win32State->scissorMaxX)
    {
        maxX = win32State->scissorMaxX;
    }
    
    if (maxY > win32State->scissorMaxY)
    {
        maxY = win32State->scissorMaxY;
    }

    uint8_t r = colors[5][0];
    uint8_t g = colors[5][1];
    uint8_t b = colors[5][2];

    if ((int16_t)minX <= win32State->mouseX && win32State->mouseX <= (int16_t)maxX &&
        (int16_t)minY <= win32State->mouseY && win32State->mouseY <= (int16_t)maxY)
    {
        //win32State->heldButton = btn;

        if (win32State->lmbDown)
        {
            r = colors[4][0];
            g = colors[4][1];
            b = colors[4][2];
        }
        
        if (win32State->lmbRelease)
        {
            press = true;
        }
    }
    
    DrawRect(minX, minY, maxX, maxY, r, g, b);
    RenderText(text,
        minX + (maxX - minX) / 2 - (uint32_t)textWidth / 2,
        minY + (maxY - minY) / 2 - (charMaxY - charMinY) / 2 + 4);

    return press;
}

bool UIButton(uint32_t centerX, uint32_t centerY, char* text)
{
    float textWidth;
    float textHeight;
    MeasureText(text, &textWidth, &textHeight);
    textHeight = (float)(charMaxY - charMinY);
    uint32_t width = (uint32_t)textWidth + 16;
    uint32_t height = (uint32_t)textHeight + 8;
    uint32_t maxX = centerX + width / 2;
    uint32_t minX = maxX - width;
    uint32_t maxY = centerY + height / 2;
    uint32_t minY = maxY - height;
    
    return UIButton(minX, minY, maxX, maxY, text);
}

int WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nShowCmd*/)
{
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == FALSE)
    {
        Warning("Failed to disable windows DPI scaling. App may look blurry.");
    }

    win32State = (Win32State*)malloc(sizeof(Win32State));
    ZeroMemory(win32State, sizeof(Win32State));

    //Load font
    {
        HANDLE file = CreateFileW(L"Roboto_Condensed-Thin.ttf", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
                
        if (file == INVALID_HANDLE_VALUE)
        {
            Warning("Failed to open file!");
        }
        else
        {
            uint32_t sizeHigh;
            uint32_t sizeLow =  GetFileSize(file, (LPDWORD)&sizeHigh);
            uint64_t fileSize = (uint64_t)sizeLow | ((uint64_t)sizeHigh << 32);
    
            void* fileData = malloc(fileSize);
    
            uint64_t bytesToRead = fileSize;
    
            while (bytesToRead > 0)
            {
                uint32_t readSize = (uint32_t)bytesToRead;
                if (bytesToRead > UINT32_MAX)
                {
                    readSize = UINT32_MAX;
                }
                
                DWORD bytesRead;
                if (ReadFile(file, fileData, readSize, &bytesRead, 0) == FALSE)
                {
                    Warning("Failed to read file!");
                    free(fileData);
                    fileData = 0;
                    break;
                }
    
                bytesToRead -= bytesRead;
            }
            
            if (fileData)
            {
                stbtt_fontinfo info;
                stbtt_InitFont(&info, (unsigned char*)fileData, stbtt_GetFontOffsetForIndex((unsigned char*)fileData, 0));
                uint32_t oversample = 4;
                float pixelSize = 32.0f;
                float scale = stbtt_ScaleForPixelHeight(&info, pixelSize);

                stbtt_pack_range packRange = {};
                packRange.font_size = pixelSize;
                packRange.first_unicode_codepoint_in_range = 0;  // if non-zero, then the chars are continuous, and this is the first codepoint
                packRange.array_of_unicode_codepoints = utf8Codepoints;       // if non-zero, then this is an array of unicode codepoints
                packRange.num_chars = _countof(utf8Codepoints);
                packRange.chardata_for_range = packedCharacters; // output

                stbtt_pack_context spc = {};
        
                fontPixels = (uint8_t*)malloc(fontImageWidth * fontImageHeight * sizeof(uint8_t));

                stbtt_PackBegin(&spc, fontPixels, fontImageWidth, fontImageHeight, fontImageWidth, 2, 0);
                stbtt_PackSetOversampling(&spc, oversample, oversample);
                stbtt_PackFontRanges(&spc, (unsigned char*)fileData, 0, &packRange, 1);
                stbtt_PackEnd(&spc);
                
                int x0;
                int x1;
                int y0;
                int y1;
                stbtt_GetFontBoundingBox(&info, &x0, &y0, &x1, &y1);
                charMinX = (uint32_t)(x0 * scale);
                charMaxX = (uint32_t)(x1 * scale);
                charMinY = (uint32_t)(y0 * scale);
                charMaxY = (uint32_t)(y1 * scale);

                free(fileData);
            }
    
            CloseHandle(file);
        }

    }

    {
        WNDCLASSEXW wndClass;
        ZeroMemory(&wndClass, sizeof(wndClass));
        wndClass.cbSize = sizeof(wndClass);
        wndClass.lpfnWndProc = WindowProc;
        wndClass.hInstance = hInstance;
        wndClass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wndClass.hCursor = NULL;
        wndClass.lpszClassName = L"TonalBalanceAnalyzer";
        ATOM atom = RegisterClassExW(&wndClass);
        Assert(atom && "Failed to register window class");

        int width = CW_USEDEFAULT;
        int height = CW_USEDEFAULT;
        DWORD exstyle = WS_EX_APPWINDOW;
        DWORD style = WS_OVERLAPPEDWINDOW;

        win32State->renderWidth  = 1280;
        win32State->renderHeight = 720;
        style &= ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
        RECT rect = { 0, 0, (LONG)win32State->renderWidth, (LONG)win32State->renderHeight };
        AdjustWindowRectEx(&rect, style, FALSE, exstyle);
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;

        const POINT ptZero = { 0, 0 };
        HMONITOR monitor = MonitorFromPoint(ptZero, MONITOR_DEFAULTTOPRIMARY);

        MONITORINFO mInfo;
        mInfo.cbSize = sizeof(mInfo);
        GetMonitorInfoA(monitor, &mInfo);

        int wndX = mInfo.rcWork.left + (mInfo.rcWork.right - mInfo.rcWork.left) / 2 - win32State->renderWidth / 2;
        int wndY = mInfo.rcWork.top + (mInfo.rcWork.bottom - mInfo.rcWork.top) / 2 - win32State->renderHeight / 2;

        win32State->window = CreateWindowExW(
            exstyle, wndClass.lpszClassName, L"TonalBalanceAnalyzer", style,
            wndX, wndY, width, height,
            NULL, NULL, wndClass.hInstance, NULL);
        Assert(win32State->window && "Failed to create window");
    }

    win32State->frame_device_context = CreateCompatibleDC(0);

    win32State->reference.averageDb = NAN;

    ShowWindow(win32State->window, SW_SHOWDEFAULT);
    //By default the loading cursor is shown when loading the window, so we set it to arrow
    SetCursor(LoadCursor(0, IDC_ARROW));
    if (FAILED(CoInitializeEx(0, COINIT_SPEED_OVER_MEMORY | COINIT_MULTITHREADED)))
    {
        Warning("Failed to initialize COM!");
        return -1;
    }
    SamplerInitialize(&win32State->sampler);
    win32State->isPlaying = false;

    win32State->running = true;

    const uint32_t fftWindowSize = 4096;
    PFFFT_Setup* fft = pffft_new_setup(fftWindowSize, PFFFT_REAL);
    Assert(fft && "n incorrect number!");

    size_t singleBufferSize = fftWindowSize * sizeof(float);
    singleBufferSize = (singleBufferSize + MALLOC_V4SF_ALIGNMENT) - ((size_t)singleBufferSize & (MALLOC_V4SF_ALIGNMENT - 1));

    void* memory = malloc(singleBufferSize * 3 + MALLOC_V4SF_ALIGNMENT);

    if (memory == 0)
    {
        Warning("Out of memory!");
        return -1;
    }
    
    void* alignedMemory = (void *) (((char*)memory + MALLOC_V4SF_ALIGNMENT) - (((size_t)memory) & (MALLOC_V4SF_ALIGNMENT - 1)));
    float* input  = (float*)alignedMemory;
    float* output = (float*)((uint8_t*)alignedMemory + singleBufferSize);
    float* work   = (float*)((uint8_t*)alignedMemory + 2 * singleBufferSize);
    float amplitudes[fftWindowSize / 2 + 1] = {};
    
    float integratedLivePower[fftWindowSize / 2 + 1] = {};
    const float INTEGRATION_ATTACK = 0.8f; 
    const float INTEGRATION_RELEASE = 0.1f;
    
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    LARGE_INTEGER cBegin;
    LARGE_INTEGER cEnd;
    QueryPerformanceCounter(&cBegin);

    float dt = 0.0f;

    MSG msg;
    while (true)
    {
        win32State->lmbRelease = false;
        win32State->mouseScroll = 0;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!win32State->running)
        {
            break;
        }
        
        uint64_t currentTick = SamplerGetCurrentTick(&win32State->sampler);

        uint64_t elapsedTick =
            ((currentTick - win32State->lastSamplerTick) * win32State->sampler.sampleRate) / win32State->sampler.clockFrequency;
        win32State->lastSamplerTick = currentTick;

        SamplerOutput audioOut = SamplerBeginOutput(&win32State->sampler);
        ZeroMemory(audioOut.buffer, audioOut.frameCount * sizeof(float));

        for (uint32_t i = 0; i < audioOut.frameCount; ++i)
        {
            bool writtenFromWav = false;
            for (uint32_t c = 0; c < win32State->sampler.channelCount; c++)
            {
                if (win32State->audioWriterIndex >= win32State->wav.samplesPerChannel || win32State->wav.samples == 0)
                {
                    audioOut.buffer[i * 2 + c] = 0.0f;
                }
                else
                {
                    writtenFromWav = true;
                    audioOut.buffer[i * 2 + c] = win32State->wav.samples[win32State->audioWriterIndex * 2 + c];
                }
            }

            if (writtenFromWav)
            {
                win32State->audioWriterIndex++;
            }
        }
        
        SamplerEndOutput(&win32State->sampler, audioOut);

        if (win32State->wav.samples)
        {
            win32State->audioPlayingIndex += elapsedTick;

            if (win32State->audioPlayingIndex >= win32State->wav.samplesPerChannel)
            {
                if (win32State->isPlaying)
                {
                    SamplerPause(&win32State->sampler);
                }
                
                win32State->isPlaying = false;
                win32State->audioPlayingIndex = win32State->wav.samplesPerChannel - 1;
            }

            uint64_t loopCount = fftWindowSize / 2;
            uint64_t startIndex = (uint32_t)win32State->audioPlayingIndex;

            if (startIndex + loopCount >= win32State->wav.samplesPerChannel)
            {
                startIndex = win32State->wav.samplesPerChannel - loopCount;
            }
            
            for (uint32_t i = 0; i < fftWindowSize / 2; ++i)
            {
                float monoAmp = 0.0f;

                for (uint32_t c = 0; c < win32State->wav.channelCount; c++)
                {
                    monoAmp +=
                        win32State->wav.samples[(i + startIndex) * win32State->wav.channelCount + c] / win32State->wav.channelCount;
                }
                
                float t = ((float)i / (float)(fftWindowSize / 2 - 1));
                float hannWindow = 0.5f * (1.0f - cosf(t * (2.0f * (float)M_PI)));

                input[i] = monoAmp * hannWindow;
            }
            
            for (uint32_t i = fftWindowSize / 2; i < fftWindowSize; ++i)
            {
                input[i] = 0.0f;
            }
            
            pffft_transform_ordered(fft, input, output, work, PFFFT_FORWARD);

            //Handle the special cases first (DC and Nyquist are purely real)
            //Should be squared and then square rooted but thats equal to absolute value
            amplitudes[0] = fabsf(output[0]) / fftWindowSize * 4.0f;
            amplitudes[_countof(amplitudes) - 1] = fabsf(output[1]) / fftWindowSize * 4.0f;

            //Handle the rest of the bins (extract real and imaginary parts)
            for (uint32_t i = 1; i < _countof(amplitudes) - 1; ++i)
            {
                float re = output[2 * i];
                float im = output[2 * i + 1];
                
                //Amplitude = sqrt((real^2) + (imaginary^2)) (phytagorean theorem)
                //Multiply with 2 because zero padding halves the power of every frequency
                //Multiply with 2 again because hann window halves the power of every frequency
                amplitudes[i] = sqrtf((re * re) + (im * im)) / (fftWindowSize / 2) * 4.0f;
            }
        }
        
        const uint32_t renderWidth = (uint32_t)win32State->renderWidth;
        const uint32_t renderHeight = (uint32_t)win32State->renderHeight;

        DrawRect(0, 0, renderWidth, renderHeight, 0, 0, 0);
        
        //TopBarDraw
        {
            DrawRect(0, renderHeight - win32State->topBarHeight, renderWidth, renderHeight,
                colors[0][0], colors[0][1], colors[0][2]);

            RenderText("TonalBalanceAnalyzer", win32State->titleOriginX, win32State->titleOriginY);

            uint32_t marginX = 12;

            uint32_t btnHeight = (uint32_t)((float)win32State->topBarHeight * 0.8f);
            uint32_t btnCenterY = renderHeight - (uint32_t)((float)win32State->topBarHeight * 0.5f);
            uint32_t btnMinY = btnCenterY - btnHeight / 2;
            uint32_t btnMaxY = btnMinY + btnHeight;

            win32State->titleOriginX = marginX;
            win32State->titleOriginY = btnMinY + btnHeight / 4;

            uint32_t btnMaxX = renderWidth - marginX;
            for (uint32_t i = 0; i < 2; i++)
            {
                char* text = "Unnamed button";
                switch (i)
                {
                case 0:
                    text = "Load track";
                    break;
                    
                case 1:
                    text = "Load reference";
                    break;
                
                default:
                    break;
                }

                float sizeX = 0.0f;
                float sizeY = 0.0f;
                MeasureText(text, &sizeX, &sizeY);
                uint32_t btnWidth = (uint32_t)sizeX + 16;
                uint32_t btnMinX = btnMaxX - btnWidth;

                if (UIButton(btnMinX, btnMinY, btnMaxX, btnMaxY, text))
                {
                    switch (i)
                    {
                    case 0:
                    {
                        wchar_t filepath[MAX_PATH] = {};
                    
                        OPENFILENAMEW openFileName = {};
                        openFileName.lStructSize = sizeof(openFileName);
                        openFileName.hwndOwner = win32State->window;
                        openFileName.lpstrFilter = L"Supported files (.wav, .wave, .flac, .oga, .mp3)\0*.wav;*.wave;*.flac;*.oga;*.mp3;\0";
                        openFileName.nFilterIndex = 1;
                        openFileName.lpstrFile = filepath;
                        openFileName.nMaxFile = sizeof(filepath);
                        openFileName.nMaxFileTitle = 0;
                        openFileName.lpstrInitialDir = 0;
                        openFileName.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
                    
                        if (GetOpenFileNameW(&openFileName) == TRUE)
                        {
                            if (win32State->isPlaying)
                            {
                                SamplerPause(&win32State->sampler);
                                win32State->isPlaying = false;
                                win32State->audioPlayingIndex = 0;
                                win32State->audioWriterIndex = 0;
                            }
                            
                            DeleteAudioFile(&win32State->wav);

                            HANDLE file = CreateFileW(filepath, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
                    
                            if (file == INVALID_HANDLE_VALUE)
                            {
                                Warning("Failed to open file!");
                            }
                            else
                            {
                                uint32_t sizeHigh;
                                uint32_t sizeLow =  GetFileSize(file, (LPDWORD)&sizeHigh);
                                uint64_t fileSize = (uint64_t)sizeLow | ((uint64_t)sizeHigh << 32);
                        
                                void* fileData = malloc(fileSize);
                        
                                uint64_t bytesToRead = fileSize;
                        
                                while (bytesToRead > 0)
                                {
                                    uint32_t readSize = (uint32_t)bytesToRead;
                                    if (bytesToRead > UINT32_MAX)
                                    {
                                        readSize = UINT32_MAX;
                                    }
                                    
                                    DWORD bytesRead;
                                    if (ReadFile(file, fileData, readSize, &bytesRead, 0) == FALSE)
                                    {
                                        Warning("Failed to read file!");
                                        free(fileData);
                                        fileData = 0;
                                        break;
                                    }
                        
                                    bytesToRead -= bytesRead;
                                }
                                
                                if (fileData)
                                {
                                    ReadAudioFile(GetAudioFileTypeFromPath(filepath), &win32State->wav, fileData, fileSize);
                                    SamplerSetNewFormat(&win32State->sampler, win32State->wav.channelCount, win32State->wav.sampleRate);
                                    CalculateAudioWaveBars(&win32State->wav, renderWidth);
                                    free(fileData);
                                }
                        
                                CloseHandle(file);
                            }
                        }

                    } break;

                    case 1:
                    {
                        win32State->referenceSelectorOpen = !win32State->referenceSelectorOpen;
                        win32State->targetReferenceCount = 0;

                    } break;
                    
                    default:
                        break;
                    }
                }
                
                btnMaxX = btnMinX - marginX;
            }
        }
        
        float totalLivePower = 0.0f;

        for (int i = 0; i < _countof(amplitudes); ++i)
        {
            float instantPower = amplitudes[i] * amplitudes[i];

            if (instantPower > integratedLivePower[i])
            {
                //Multiply with 60 for fps compensation
                integratedLivePower[i] += INTEGRATION_ATTACK * (instantPower - integratedLivePower[i]) * dt * 60.0f;
            }
            else
            {
                //Multiply with 60 for fps compensation
                integratedLivePower[i] += INTEGRATION_RELEASE * (instantPower - integratedLivePower[i]) * dt * 60.0f;
            }
            
            totalLivePower += integratedLivePower[i]; 
        }

        // Calculate the Live Average using the smoothed power
        float averageLivePower = totalLivePower / (float)_countof(amplitudes);
        float safeLivePower = max(averageLivePower, MIN_POWER);
        float liveAverageDb = 10.0f * log10f(safeLivePower);

        float visualOffsetDb = 0.0f;

        if (!isnan(win32State->reference.averageDb))
        {
            if (liveAverageDb > MIN_DB)
            {
                // Because the live average is now inherently slow/smoothed, 
                // the offset can apply instantly without causing lag spikes!
                visualOffsetDb = win32State->reference.averageDb - liveAverageDb;
            }
        }

        const float minFrequency = 20.0f;
        const float maxFrequency = 20000.0f;

        uint32_t metersMinY = win32State->bottomBarHeight;
        uint32_t metersMaxHeight = renderHeight - win32State->topBarHeight - metersMinY;

        if (win32State->referenceSelectorOpen)
        {
            //DrawReferenceSelector

            if (UIButton(renderWidth / 2, renderHeight - win32State->topBarHeight - 48, "Load reference from file"))
            {
                wchar_t filepath[MAX_PATH] = {};
                
                OPENFILENAMEW openFileName = {};
                openFileName.lStructSize = sizeof(openFileName);
                openFileName.hwndOwner = win32State->window;
                openFileName.lpstrFilter = L"TBA reference file (.tbaref)\0*.tbaref;\0";
                openFileName.lpstrInitialDir = L"References\\";
                openFileName.nFilterIndex = 1;
                openFileName.lpstrFile = filepath;
                openFileName.nMaxFile = sizeof(filepath);
                openFileName.nMaxFileTitle = 0;
                openFileName.lpstrInitialDir = 0;
                openFileName.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            
                if (GetOpenFileNameW(&openFileName) == TRUE)
                {
                    SerializeReferenceInfo(filepath, &win32State->reference, false);
                    win32State->referenceSelectorOpen = false;
                }
            }

            if (UIButton(renderWidth / 2, renderHeight - win32State->topBarHeight - 96, "Add target track(s)"))
            {
                uint32_t filepathTableWidth = MAX_PATH;
                uint32_t charsAllocated = MAX_PATH * 1024;
                wchar_t* filepath = (wchar_t*)malloc(charsAllocated * sizeof(wchar_t));
                ZeroMemory(filepath, charsAllocated * sizeof(wchar_t));
            
                OPENFILENAMEW openFileName = {};
                openFileName.lStructSize = sizeof(openFileName);
                openFileName.hwndOwner = win32State->window;
                openFileName.lpstrFilter = L"Supported files (.wav, .wave, .flac, .oga, .mp3)\0*.wav;*.wave;*.flac;*.oga;*.mp3;\0";
                openFileName.nFilterIndex = 1;
                openFileName.lpstrFile = filepath;
                openFileName.nMaxFile = charsAllocated;
                openFileName.nMaxFileTitle = 0;
                openFileName.lpstrInitialDir = 0;
                openFileName.lpstrTitle = L"Open reference track(s)";
                openFileName.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
            
                if (GetOpenFileNameW(&openFileName) == TRUE)
                {
                    size_t firstLength = wcsnlen_s(filepath, charsAllocated);

                    if (filepath[firstLength + 1] == 0)
                    {
                        //Single file
                        wchar_t* current = win32State->targetReferences[win32State->targetReferenceCount++];
                        wcscpy_s(current, firstLength + 1, filepath);
                    }
                    else
                    {
                        //Multiple files
                        wchar_t* directory = filepath;
                        wchar_t* bufferEnd = filepath + charsAllocated;
                        wchar_t* firstFile = &filepath[firstLength + 1];
                        
                        for (wchar_t* i = firstFile; ; )
                        {
                            size_t iLength = wcsnlen_s(i, (size_t)(bufferEnd - i));
                            wchar_t* current = win32State->targetReferences[win32State->targetReferenceCount++];

                            wcscpy_s(current, firstLength + 1, directory);
                            wcscat_s(current, filepathTableWidth, L"\\");
                            wcscat_s(current, filepathTableWidth, i);

                            if (i[iLength + 1] == 0)
                            {
                                break;
                            }

                            i = &i[iLength + 1];
                        }
                    }
                    
                }

                free(filepath);
            }
            
            
            win32State->scissorMinY = win32State->bottomBarHeight + 48 + 12;
            win32State->scissorMaxY = renderHeight - win32State->topBarHeight - 96 - 28;

            if ((int32_t)win32State->scissorMinY <= win32State->mouseY && win32State->mouseY <= (int32_t)win32State->scissorMaxY)
            {
                win32State->referenceListOffsetY -= win32State->mouseScroll;
            }
            
            if (win32State->referenceListOffsetY < 0)
            {
                win32State->referenceListOffsetY = 0;
            }
            
            int32_t areaHeight = win32State->scissorMaxY - win32State->scissorMinY - 24;
            if (win32State->referenceListOffsetY + areaHeight > (int32_t)win32State->targetReferenceCount * 48)
            {
                win32State->referenceListOffsetY = (int32_t)win32State->targetReferenceCount * 48 - areaHeight;
            }

            uint32_t centerX = renderWidth / 2;
            uint32_t centerY = win32State->scissorMaxY + win32State->referenceListOffsetY;
            for (uint32_t i = 0; i < win32State->targetReferenceCount;)
            {
                wchar_t* filepath = win32State->targetReferences[i];

                centerY -= 48;

                int pathLen = (int)wcslen(filepath);
                int size_needed = WideCharToMultiByte(CP_UTF8, 0, filepath, pathLen, NULL, 0, NULL, NULL);
                char* utf8String = (char*)malloc(size_needed + 1);
                ZeroMemory(utf8String, size_needed + 1);
                WideCharToMultiByte(CP_UTF8, 0, filepath, pathLen, utf8String, size_needed, NULL, NULL);
                
                float sizeX;
                float sizeY;
                MeasureText(utf8String, &sizeX, &sizeY);
                RenderText(utf8String,
                    centerX - (uint32_t)sizeX / 2,
                    centerY);

                free(utf8String);

                uint32_t height = (uint32_t)charMaxY - charMinY + 8;
                if (UIButton(centerX + (uint32_t)sizeX / 2 + 12 + height / 2, centerY, "-"))
                {
                    win32State->targetReferenceCount--;
                    if (i < win32State->targetReferenceCount)
                    {
                        uint32_t copySize = MAX_PATH * sizeof(wchar_t) * (win32State->targetReferenceCount - i);
                        memcpy_s(filepath, copySize, win32State->targetReferences[i + 1], copySize);
                    }
                    else
                    {
                        i++;
                    }
                }
                else
                {
                    i++;
                }
            }
            
            win32State->scissorMinY = 0;
            win32State->scissorMaxY = renderHeight;
            
            if (UIButton(renderWidth / 2, win32State->bottomBarHeight + 36, "Calculate"))
            {
                CalculateReferenceCurve(&win32State->reference,
                    (wchar_t*)win32State->targetReferences, win32State->targetReferenceCount, MAX_PATH, 4096, 20.0f, 20000.0f, 2048);
                
                wchar_t filepath[MAX_PATH] = {};

                OPENFILENAMEW openFileName = {};
                openFileName.lStructSize = sizeof(openFileName);
                openFileName.hwndOwner = win32State->window;
                openFileName.lpstrFilter = L"TBA reference file (.tbaref)\0*.tbaref;\0";
                openFileName.nFilterIndex = 1;
                openFileName.lpstrFile = filepath;
                openFileName.nMaxFile = sizeof(filepath);
                openFileName.nMaxFileTitle = 0;
                openFileName.lpstrInitialDir = 0;
                openFileName.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

                if (GetSaveFileNameW(&openFileName) != 0)
                {
                    bool extensionFound = false;

                    for (size_t i = wcslen(filepath) - 1; i >= 0; i--)
                    {
                        if (filepath[i] == '.')
                        {
                            extensionFound = true;
                            break;
                        }
                        
                        if (i == 0)
                        {
                            break;
                        }
                    }

                    if (!extensionFound)
                    {
                        size_t writer = wcslen(filepath);
                        filepath[writer + 0] = '.';
                        filepath[writer + 1] = 't';
                        filepath[writer + 2] = 'b';
                        filepath[writer + 3] = 'a';
                        filepath[writer + 4] = 'r';
                        filepath[writer + 5] = 'e';
                        filepath[writer + 6] = 'f';
                        filepath[writer + 7] = 0;
                    }
                    
                    SerializeReferenceInfo(filepath, &win32State->reference, true);
                }
                
                win32State->referenceSelectorOpen = false;
            }
        }
        else
        {
            //DrawAnalyzer
            float prevLowerHeightRatio = 0.0f;
            float prevUpperHeightRatio = 0.0f;
            for (uint32_t x = 0; x < renderWidth; ++x)
            {
                if (win32State->wav.samples)
                {
                    float decibels = GetFreqDecibels(x, renderWidth, integratedLivePower, win32State->wav.sampleRate,
                        fftWindowSize, minFrequency, maxFrequency);

                    decibels += visualOffsetDb;

                    float targetHeightRatio = 1.0f - (decibels / VISUALIZER_MIN_DB);
                    if (targetHeightRatio < 0.0f)
                    {
                        targetHeightRatio = 0.0f;
                    }
                    if (targetHeightRatio > 1.0f)
                    {
                        targetHeightRatio = 1.0f;
                    }
        
                    uint32_t maxY = 0;
                    if (targetHeightRatio > 0.0f)
                    {
                        maxY = (uint32_t)((float)metersMaxHeight * targetHeightRatio) + metersMinY;
                    }
                    
                    for (uint32_t y = metersMinY; y < maxY; y++)
                    {
                        win32State->pixels[y * win32State->renderWidth + x] = win32State->gradientLookup[y - metersMinY];
                    }
                }

                //DrawReferenceCurves
                float falloff = 1.0f;
                {
                    float t = (float)x / (float)renderWidth;
                    float exactBinStart = t * (win32State->reference.bucketCount - 1);
                    int b1 = (int)exactBinStart;
            
                    int b0 = b1 - 1;
                    int b2 = b1 + 1;
                    int b3 = b1 + 2;

                    float fraction = exactBinStart - (float)b1;

                    if (b0 < 1)
                    {
                        b0 = 1;
                    }
                    if (b1 < 1)
                    {
                        b1 = 1;
                    }
                    if (b2 > (int)win32State->reference.bucketCount - 1)
                    {
                        b2 = win32State->reference.bucketCount - 1;
                    }
                    if (b3 > (int)win32State->reference.bucketCount - 1)
                    {
                        b3 = win32State->reference.bucketCount - 1;
                    }

                    if (win32State->reference.lowerAmplitudes)
                    {
                        DrawReferenceCurveAtX(falloff, &prevLowerHeightRatio, x,
                            renderWidth, renderHeight, metersMinY, metersMaxHeight,
                            b0, b1, b2, b3, fraction, win32State->reference.lowerAmplitudes);
                    }

                    if (win32State->reference.upperAmplitudes)
                    {
                        DrawReferenceCurveAtX(falloff, &prevUpperHeightRatio, x,
                            renderWidth, renderHeight, metersMinY, metersMaxHeight,
                            b0, b1, b2, b3, fraction, win32State->reference.upperAmplitudes);
                    }
                }
            }

            const float freqLines[] =
            {
                20.0f,
                50.0f,
                100.0f,
                200.0f,
                500.0f,
                1000.0f,
                2000.0f,
                5000.0f,
                10000.0f
            };

            // Pre-calculate the denominator since it never changes
            float logRange = log10f(maxFrequency / minFrequency);

            for (uint32_t i = 0; i < _countof(freqLines); i++)
            {
                float frequency = freqLines[i];
                
                float ratio = log10f(frequency / minFrequency) / logRange;
                
                uint32_t xPos = (uint32_t)(ratio * renderWidth);
                
                if (xPos < 0 || xPos >= renderWidth) continue;

                
                for (uint32_t py = win32State->bottomBarHeight; py < renderHeight - win32State->topBarHeight; py++)
                {
                    uint8_t r;
                    uint8_t g;
                    uint8_t b;
                    UnpackColor(win32State->pixels[py * win32State->renderWidth + xPos], &r, &g, &b);

                    r = Lerp(r, 255, 0.5f);
                    g = Lerp(g, 255, 0.5f);
                    b = Lerp(b, 255, 0.5f);
                    
                    uint32_t val = (b & 0xFF) | (g & 0xFF) << 8 | (r & 0xFF) << 16;
                    win32State->pixels[py * win32State->renderWidth + xPos] = val;
                }

                char txt[16];
                sprintf_s(txt, 16, "%.0f hz", frequency);
                RenderText(txt, xPos + 4, win32State->bottomBarHeight + 4);
            }
        }

        //BottomBarDraw
        DrawRect(0, 0, renderWidth, win32State->bottomBarHeight, colors[0][0], colors[0][1], colors[0][2]);

        if (win32State->audioWaveBars)
        {
            for (uint32_t x = 0; x < renderWidth; x++)
            {
                AudioWaveBar* waveBar = &win32State->audioWaveBars[x];
                DrawRect(x, waveBar->minY, x + 1, waveBar->maxY, colors[5][0], colors[5][1], colors[5][2]);
            }
            
            float t = ((float)win32State->audioPlayingIndex / (float)win32State->wav.samplesPerChannel);
            uint32_t x = (uint32_t)((float)renderWidth * t);
            DrawRect(x, 0, x + 1, win32State->bottomBarHeight, colors[10][0], colors[10][1], colors[10][2]);
        }
        
        //Maybe draw a pixel bar at the bottom for it to look smooth?
        //DrawRect(0, win32State->bottomBarHeight, renderWidth, win32State->bottomBarHeight + 1, colors[0][0], colors[0][1], colors[0][2]);

        InvalidateRect(win32State->window, NULL, FALSE);
        UpdateWindow(win32State->window);

        DwmFlush();

        QueryPerformanceCounter(&cEnd);

        dt = (float)(cEnd.QuadPart - cBegin.QuadPart) / (float)(freq.QuadPart);

        if (dt > 0.5f)
        {
            dt = 0.5f;
        }
        
        cBegin = cEnd;
    }
    
    free(memory);
    pffft_destroy_setup(fft);

    SamplerCloseDevice(&win32State->sampler);

    return 0;
}