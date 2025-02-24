#ifndef AUDIOOUTPUTREENCODER
#define AUDIOOUTPUTREENCODER

extern "C" {
#include "libavcodec/avcodec.h"
};

#include "spdifencoder.h"
#include "audiooutputsettings.h"

#define INBUFSIZE 131072
#define OUTBUFSIZE INBUFSIZE

class AudioOutputDigitalEncoder
{

  public:
    AudioOutputDigitalEncoder(void);
    ~AudioOutputDigitalEncoder();

    bool   Init(AVCodecID codec_id, int bitrate, int samplerate, int channels);
    int Encode(void *input, int len, AudioFormat format);
    int GetFrames(void *ptr, int maxlen);
    int    Buffered(void) const
    // assume 32 bit samples = 4 byte
    { return m_inlen / 4 / m_avContext->channels; }
    void    clear();

  private:
    void   Reset(void);
    static void *realloc(void *ptr, int old_size, int new_size);

    AVCodecContext *m_avContext         {nullptr};
    uint8_t        *m_outbuf            {nullptr};
    int             m_outSize           {0};
    // m_inbuf  = 6 channel data converted to S32 or FLT samples interleaved
    uint8_t        *m_inbuf             {nullptr};
    // m_framebuf = 1 frame, deinterleaved into planar format
    uint8_t        *m_framebuf          {nullptr};
    int             m_inSize            {0};
    int             m_outlen            {0};
    // m_inlen = number of bytes available in m_inbuf
    int             m_inlen             {0};
    int             m_samplesPerFrame   {0};
    SPDIFEncoder   *m_spdifEnc          {nullptr};
    AVFrame        *m_frame             {nullptr};
};

#endif
