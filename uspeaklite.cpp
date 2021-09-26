#include "uspeaklite.h"

#include "uspeakvolume.h"
#include "uspeakresampler.h"

#include "fmt/core.h"
#include "libnyquist/Decoders.h"
#include "libnyquist/Encoders.h"
#include "internal/scopedspinlock.h"

constexpr std::size_t USPEAK_BUFFERSIZE = 1022;

constexpr void SetPacketId(std::span<std::uint8_t> packet, std::int32_t packetId) {
    *((std::int32_t*)(packet.data() + 0)) = packetId;
}
constexpr std::int32_t GetPacketSenderId(std::span<const std::uint8_t> packet) {
    return *((std::int32_t*)(packet.data() + 0));
}
constexpr void SetPacketServerTime(std::span<std::uint8_t> packet, std::int32_t packetTime) {
   *((std::int32_t*)(packet.data() + 4)) = packetTime;
}
constexpr std::int32_t GetPacketServerTime(std::span<const std::uint8_t> packet) {
    return *((std::int32_t*)(packet.data() + 4));
}

struct PlayerData {
    int sampleIndex;
    std::uint32_t startTicks;
    std::vector<float> framesToSave;
};

#include <mutex>
std::mutex uspeakPlayersLock;
std::unordered_map<std::uint8_t, PlayerData> uspeakPlayers;

USpeakNative::USpeakLite::USpeakLite()
    : m_run(true)
    , m_lock(false)
    , m_opusCodec(std::make_shared<USpeakNative::OpusCodec::OpusCodec>(48000, 24000, USpeakNative::OpusCodec::OpusDelay::Delay_20ms))
    , m_frameQueue()
    , m_processingThread(&USpeakNative::USpeakLite::processingLoop, this)
    , m_lastBandMode()
    , m_bandMode(USpeakNative::OpusCodec::BandMode::Opus48k)
    , m_recFreq(USpeakNative::OpusCodec::BandModeFrequency(m_bandMode))
    , m_ind(0)
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

int USpeakNative::USpeakLite::audioFrequency() const
{
    return m_recFreq;
}

USpeakNative::OpusCodec::BandMode USpeakNative::USpeakLite::bandMode() const
{
    return m_bandMode;
}

std::size_t USpeakNative::USpeakLite::getAudioFrame(std::int32_t actorNr, std::int32_t packetTime, std::span<std::uint8_t> buffer)
{
    USpeakNative::Internal::ScopedSpinLock l(m_lock);

    if (m_frameQueue.empty() || buffer.size() < 1022) {
        return 0;
    }

    SetPacketId(buffer, actorNr);
    SetPacketServerTime(buffer, packetTime);

    std::size_t sizeWritten = 6; // TODO: REEEEEEEEEEEEE WHAT THE FUCK

    for (int i = 0; (i < 3) && !m_frameQueue.empty(); i++) {
        const auto& frameData = m_frameQueue.front().encodedData(); // SOMEWHERE IN HERE 2 blank bytes get written

        fmt::print("sizeWritten = {}\n", sizeWritten);

        memcpy(buffer.data() + sizeWritten, frameData.data(), frameData.size()); // maybe the data contains the blank bytes???????????????????????
        sizeWritten += frameData.size();

        fmt::print("sizeWrittenAfter = {}\n", sizeWritten);

        m_frameQueue.pop_front();
    }

    fmt::print("Size of frame: {}\n", sizeWritten);

    return sizeWritten + 2; // TODO: WHAT THE FUCKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK
}

std::vector<std::uint8_t> USpeakNative::USpeakLite::recodeAudioFrame(std::span<const std::uint8_t> dataIn)
{
    // Get packet senderId and serverTicks for sender
    std::int32_t senderId = GetPacketSenderId(dataIn);
    std::int32_t serverTicks = GetPacketServerTime(dataIn);

    // Get or create a uspeakplayer object to hold audio data from this player
    std::scoped_lock l(uspeakPlayersLock);
    auto it = uspeakPlayers.find(senderId);
    if (it == uspeakPlayers.end()) {
        fmt::print("[USpeakNative] Recording: uSpeaker[{}]\n", senderId);
        PlayerData data;
        data.sampleIndex = 0;
        data.startTicks = serverTicks;
        data.framesToSave.reserve(512000);
        it = uspeakPlayers.emplace(senderId, std::move(data)).first;
    }

    // Get all the audio packets, and decode them into float32 samples
    std::size_t offset = 8;
    while (offset < dataIn.size()) {
        USpeakNative::USpeakFrameContainer container;
        if (!container.decode(dataIn, offset)) {
            fmt::print("[USpeakNative] Failed to decode audio packet!\n");
            return {};
        }

        auto hmm = m_opusCodec->decodeFloat(container.decodedData(), USpeakNative::OpusCodec::BandMode::Opus48k);

        offset += container.encodedSize();

        it->second.framesToSave.insert(it->second.framesToSave.end(), hmm.begin(), hmm.end());
    }

    // If buffer has been filled, encode and save the audio data
    if (it->second.framesToSave.size() >= 500000) {

        // Server ticks are in ms so just calculate the time elapsed by getting the diff / 1000
        auto dur = (double)(serverTicks - it->second.startTicks) / 1000.;
        it->second.startTicks = serverTicks;

        fmt::print("[USpeakNative] Saving {} seconds from uSpeaker[{}]\n", dur, senderId);

        nqr::AudioData data;
        data.channelCount = 1;            // Single channel
        data.sampleRate = 24000;          // 24k bitrate
        data.sourceFormat = nqr::PCM_FLT; // PulseCodeModulation_FLoaT
        data.lengthSeconds = dur;         // amount of seconds elapsed
        data.frameSize = 32;              // bits per sample
        data.samples = it->second.framesToSave; // audio data

        // Encode and save to ogg files
        nqr::encode_opus_to_disk({ 1, nqr::PCM_FLT, nqr::DITHER_NONE }, &data, fmt::format("test-{}-{}.ogg", senderId, it->second.sampleIndex++));

        it->second.framesToSave.resize(0);
    }

    return std::vector<std::uint8_t>(dataIn.begin(), dataIn.end());
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

        // Resample to 24k
        if (fileData.sampleRate != 24000) {
            fmt::print("[USpeakNative] Resampling to 24k...\n");
            swapBuffer.resize(0);

            USpeakNative::Resample(fileData.samples, fileData.sampleRate, swapBuffer, 24000);

            std::swap(fileData.samples, swapBuffer);
            fileData.sampleRate = 24000;
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
            bool ok = container.fromData(m_opusCodec->encodeFloat(std::span<float>(it_a, it_b), USpeakNative::OpusCodec::BandMode::Opus48k), frameIndex++);

            if (ok) {
                m_frameQueue.push_back(std::move(container));
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

}
