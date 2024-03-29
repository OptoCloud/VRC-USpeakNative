#ifndef USPEAK_OPUSCODEC_H
#define USPEAK_OPUSCODEC_H

#include "opusframetime.h"
#include "bandmode.h"

#include <vector>
#include <array>
#include <span>

class OpusEncoder;
class OpusDecoder;

namespace USpeakNative::OpusCodec {

class OpusCodec
{
public:
    OpusCodec(int sampleRate, int channels, USpeakNative::OpusCodec::OpusFrametime frametime);
    ~OpusCodec();

    bool init();

    std::size_t sampleSize() noexcept;
    std::span<const std::byte> encodeFloat(std::span<const float> samples, USpeakNative::OpusCodec::BandMode mode);
    std::span<const float> decodeFloat(std::span<const std::byte> data, USpeakNative::OpusCodec::BandMode mode);
private:
    void destroyCodecs();

    OpusEncoder* m_encoder;
    OpusDecoder* m_decoder;
    int m_sampleRate;
    int m_channels;
    USpeakNative::OpusCodec::OpusFrametime m_frametime;
    std::size_t m_frameSize;
    std::array<std::byte, 1024> m_encodeBuffer;
    std::array<float, 4096> m_decodeBuffer;
};

}

#endif // USPEAK_OPUSCODEC_H
