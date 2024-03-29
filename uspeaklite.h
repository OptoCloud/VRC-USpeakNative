#ifndef USPEAK_USPEAKLITE_H
#define USPEAK_USPEAKLITE_H

#include "uspeakpacket.h"
#include "uspeakframecontainer.h"
#include "opuscodec/opuscodec.h"
#include "opuscodec/bandmode.h"

#include <span>
#include <queue>
#include <memory>
#include <atomic>
#include <thread>
#include <cstdint>

namespace USpeakNative {

namespace OpusCodec { class OpusCodec; }

class USpeakLite
{
public:
    USpeakLite();
    ~USpeakLite();

    USpeakNative::OpusCodec::BandMode bandMode() const;
    std::size_t getAudioFrame(std::int32_t playerId, std::uint32_t packetTime, std::span<std::byte> buffer);

    bool encodePacket(const USpeakNative::USpeakPacket& packet, std::vector<std::byte>& dataOut);
    bool decodePacket(std::span<const std::byte> dataIn, USpeakNative::USpeakPacket& packetOut);

    bool streamFile(std::string_view filename);
private:
    void processingLoop();

    std::atomic_bool m_run;
    std::atomic_bool m_lock;
    std::shared_ptr<OpusCodec::OpusCodec> m_opusCodec;
    std::queue<USpeakFrameContainer> m_frameQueue;
    std::thread m_processingThread;
    USpeakNative::OpusCodec::BandMode m_bandMode;

    float m_currentScale;
    float m_runningScale;
    float m_targetRms;
};

}

#endif // USPEAK_USPEAKLITE_H
