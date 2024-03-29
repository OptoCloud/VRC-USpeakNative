#include "uspeaklite.h"

#include "helpers.h"
#include "uspeakvolume.h"
#include "uspeakresampler.h"

#include "fmt/core.h"
#include "libnyquist/Decoders.h"
#include "libnyquist/Encoders.h"
#include "internal/scopedspinlock.h"

constexpr std::size_t USPEAK_HEADERSIZE = sizeof(std::int32_t) + sizeof(std::uint32_t);
constexpr std::size_t USPEAK_BUFFERSIZE = 1022;

USpeakNative::USpeakLite::USpeakLite()
    : m_run(true)
    , m_lock(false)
    , m_opusCodec(std::make_shared<USpeakNative::OpusCodec::OpusCodec>(48000, 1, USpeakNative::OpusCodec::OpusFrametime::Frametime_20ms))
    , m_frameQueue()
    , m_processingThread(&USpeakNative::USpeakLite::processingLoop, this)
    , m_bandMode(USpeakNative::OpusCodec::BandMode::Opus48k)
    , m_currentScale(1.f)
    , m_runningScale(1.f)
    , m_targetRms(1.f)
{
    fmt::print("[USpeakNative] Made by OptoCloud\n");
    if (!m_opusCodec->init()) {
        throw std::exception("Failed to initialize codec!");
    }
    fmt::print("[USpeakNative] Initialized!\n");
}

USpeakNative::USpeakLite::~USpeakLite()
{
    m_run.store(false, std::memory_order::relaxed);
    if (m_processingThread.joinable()) {
        m_processingThread.join();
    }

    fmt::print("[USpeakNative] Destroyed!\n");
}

USpeakNative::OpusCodec::BandMode USpeakNative::USpeakLite::bandMode() const
{
    return m_bandMode;
}

std::size_t USpeakNative::USpeakLite::getAudioFrame(std::int32_t playerId, std::uint32_t packetTime, std::span<std::byte> buffer)
{
    USpeakNative::Internal::ScopedSpinLock l(m_lock);

    if (m_frameQueue.empty() || buffer.size() < 1022) {
        return 0;
    }

    USpeakNative::Helpers::ConvertToBytes<std::int32_t>(buffer.data(), 0, playerId); // Set 4 bytes for playerId
    USpeakNative::Helpers::ConvertToBytes<std::uint32_t>(buffer.data(), 4, packetTime); // Set 4 bytes for packetTime

    std::size_t sizeWritten = 8;

    for (int i = 0; i < 3; i++) {
        std::span<const std::byte> frameData = m_frameQueue.front().encodedData();

        if (sizeWritten + frameData.size() > buffer.size()) {
            break;
        }

        memcpy(buffer.data() + sizeWritten, frameData.data(), frameData.size());
        sizeWritten += frameData.size();

        m_frameQueue.pop();

        if (m_frameQueue.empty()) {
            break;
        }
    }

    return sizeWritten;
}

bool USpeakNative::USpeakLite::encodePacket(const USpeakPacket& packet, std::vector<std::byte>& dataOut)
{
    std::size_t sampleSize = m_opusCodec->sampleSize();
    std::uint16_t frameIndex = 0;

    // Make sure the number of samples is a multiple of the codec sample size
    std::size_t nSamples = packet.audioSamples.size();
    std::size_t nSamplesFrames = nSamples / sampleSize;
    std::size_t wholeSampleFrames = nSamplesFrames * sampleSize;
    if (wholeSampleFrames != nSamples) {
        fmt::print("[USpeakNative] AudioPacket audio has incorrect padding size! (Should be padded to %llu samples)\n", sampleSize);
        return false;
    }

    // Copy over header
    dataOut.resize(USPEAK_HEADERSIZE);
    USpeakNative::Helpers::ConvertToBytes(dataOut.data(), 0, packet.playerId);
    USpeakNative::Helpers::ConvertToBytes(dataOut.data(), 4, packet.packetTime);

    std::size_t dataOffset = USPEAK_HEADERSIZE;

    USpeakNative::USpeakFrameContainer container;

    // Encode
    auto it_a = packet.audioSamples.begin();
    auto it_end = packet.audioSamples.end();
    while (it_a != it_end) {
        auto it_b = it_a + sampleSize;

        dataOffset += USpeakNative::USpeakFrameContainer::WriteContainer(dataOut, dataOffset, m_opusCodec->encodeFloat(std::span<const float>(it_a, it_b), m_bandMode), frameIndex++);

        it_a = it_b;
    }

    return true;
}

bool USpeakNative::USpeakLite::decodePacket(std::span<const std::byte> dataIn, USpeakPacket& packetOut)
{
    if (dataIn.size() <= USPEAK_HEADERSIZE) {
        fmt::print("[USpeakNative] Audioframe too small!\n");
        return false;
    }

    // Copy over header
    packetOut.playerId = USpeakNative::Helpers::ConvertFromBytes<std::int32_t>(dataIn.data(), 0);
    packetOut.packetTime = USpeakNative::Helpers::ConvertFromBytes<std::uint32_t>(dataIn.data(), 4);
    packetOut.audioSamples.clear();

    // Get all the audio packets, and decode them into float32 samples
    std::size_t dataOffset = USPEAK_HEADERSIZE;

    std::vector<std::byte> buffer;

    // Decode
    while (dataOffset < dataIn.size()) {
        std::uint16_t packetIndex;
        dataOffset += USpeakNative::USpeakFrameContainer::ReadContainer(buffer, packetIndex, dataIn.subspan(dataOffset));

        auto opusData = m_opusCodec->decodeFloat(buffer, m_bandMode);

        if (opusData.size() > 0) {
            packetOut.audioSamples.insert(packetOut.audioSamples.end(), opusData.begin(), opusData.end());
        }
    }

    USpeakNative::AutoLevel(packetOut.audioSamples, USpeakNative::GetRMS(packetOut.audioSamples), m_targetRms, m_currentScale, m_runningScale);

    return true;
}

bool USpeakNative::USpeakLite::streamFile(std::string_view filename)
{
    fmt::print("[USpeakNative] Loading: {}\n", filename);

    USpeakNative::Internal::ScopedSpinLock l(m_lock);

    try {
        nqr::NyquistIO loader;
        nqr::AudioData fileData;

        loader.Load(&fileData, std::string(filename));

        if (fileData.channelCount == 0 || fileData.channelCount > 2) {
            fmt::print("[USpeakNative] Invalid channelcount: {}\n", fileData.channelCount);
            return false;
        }

        std::vector<float> swapBuffer;
        swapBuffer.reserve(fileData.samples.size());

        // Convert to mono
        if (fileData.channelCount == 2) {
            fmt::print("[USpeakNative] Converting to mono...\n");
            swapBuffer.resize(fileData.samples.size() / 2);

            nqr::StereoToMono(fileData.samples.data(), swapBuffer.data(), fileData.samples.size());

            std::swap(fileData.samples, swapBuffer);
            fileData.channelCount = 1;
        }

        std::size_t sampleSize = m_opusCodec->sampleSize();
        std::uint16_t frameIndex = 0;

        // Make sure the number of samples is a multiple of the codec sample size
        std::size_t nSamples = fileData.samples.size();
        std::size_t nSamplesFrames = nSamples / sampleSize;
        std::size_t wholeSampleFrames = nSamplesFrames * sampleSize;
        if (wholeSampleFrames != nSamples) {
            fileData.samples.resize(wholeSampleFrames + sampleSize);
        }

        // Adjust gain to keep sound between 1 and -1, but dont clip the audio
        fmt::print("[USpeakNative] Normalizing gain...\n");
        NormalizeGain(fileData.samples);

        // Encode
        fmt::print("[USpeakNative] Encoding...\n");
        auto it_a = fileData.samples.begin();
        auto it_end = fileData.samples.end();
        while (it_a != it_end) {
            auto it_b = it_a + sampleSize;

            USpeakNative::USpeakFrameContainer container;
            bool ok = container.fromData(m_opusCodec->encodeFloat(std::span<float>(it_a, it_b), m_bandMode), frameIndex++);

            if (ok) {
                m_frameQueue.push(std::move(container));
            }

            it_a = it_b;
        }

        fmt::print("[USpeakNative] Loaded!\n");
    } catch (const std::exception& ex) {
        fmt::print("[USpeakNative] Failed to read file: {}\n", ex.what());
        return false;
    } catch (const std::string& ex) {
        fmt::print("[USpeakNative] Failed to read file: {}\n", ex);
        return false;
    } catch (const char* ex) {
        fmt::print("[USpeakNative] Failed to read file: {}\n", ex);
        return false;
    } catch (...) {
        fmt::print("[USpeakNative] Failed to read file: Unknown error\n");
        return false;
    }

    return true;
}

void USpeakNative::USpeakLite::processingLoop()
{
    while (m_run) {

    }
}
