#include "nlohmann/json.hpp"
#include "libnyquist/Decoders.h"
#include "libnyquist/Encoders.h"
#include "uspeaklite.h"
#include "base64.h"

#include <iostream>
#include <fstream>

void InsertAudio(std::vector<float>& target, std::uint32_t posMs, std::span<const float> source) {
    std::uint32_t targetIndex = posMs * 48;

    std::size_t requiredSpace = targetIndex + source.size();
    if (target.size() < requiredSpace) {
        target.resize(requiredSpace);
    }

    for (std::uint32_t i = 0; i < source.size(); i++) {
        target[targetIndex + i] = (target[targetIndex + i] + source[i]) / 2.f;
    }
}

int main(int argc, char**argv) {
    if (argc != 2) {
        printf("Usage: USpeakTest.exe [path_to_photon_log]");
        return EXIT_FAILURE;
    }

    std::fstream ifs(argv[1], std::ios::in);
    if (!ifs.is_open()) {
        printf("failed to open logfile!");
        return EXIT_FAILURE;
    }

    USpeakNative::USpeakLite uSpeak;

    std::uint32_t startMs = UINT32_MAX;
    std::uint32_t endMs = 0;
    std::vector<std::byte> rawData;
    std::vector<USpeakNative::USpeakPacket> packets;

    // std::int32_t firstId = 0;

    auto json = nlohmann::json::parse(ifs);
    for (const auto& entry : json) {
        if (entry["patch_name"] != "OnEventPatch") continue;

        auto& eventData = entry["patch_args"]["eventData"];
        if (eventData["Code"] != 1.f) continue;

        auto& customData = eventData["CustomData"];
        if (customData["type"] != "System.Byte[]") continue;

        auto err = macaron::Base64::Decode(customData["data"], rawData);
        if (err != "") {
            printf("Error: %s\n", err.c_str());
            continue;
        }

        USpeakNative::USpeakPacket packet;
        if (!uSpeak.decodePacket(rawData, packet)) {
            printf("Decoding error!\n");
            return EXIT_FAILURE;
        }

        std::vector<std::byte> rawDataCpy;
        if (!uSpeak.encodePacket(packet, rawDataCpy)) {
            printf("Encoding error!\n");
            return EXIT_FAILURE;
        }/*
        if (rawData != rawDataCpy) {
            printf("DIFFERENT!\n");
            return EXIT_FAILURE;
        }*/
/*
        if (firstId == 0) {
            firstId = packet.playerId;
            printf("PlayerID: %i\n", firstId);
        } else if (packet.playerId != firstId) continue;
*/
        if (packet.packetTime < startMs) {
            startMs = packet.packetTime;
        } else if (packet.packetTime > endMs) {
            endMs = packet.packetTime;
        }

        packets.push_back(std::move(packet));
    }

    nqr::AudioData data;
    data.samples.reserve(5000000);

    for (const auto& packet : packets) {
        InsertAudio(data.samples, packet.packetTime - startMs, packet.audioSamples);
    }

    nqr::EncoderParams params;
    params.channelCount = 1;
    params.dither = nqr::DitherType::DITHER_NONE;
    params.targetFormat = nqr::PCMFormat::PCM_FLT;

    data.channelCount = 1;
    data.lengthSeconds = (float)(endMs - startMs) / 1000.f;
    data.sampleRate = 48000;
    data.sourceFormat = nqr::PCMFormat::PCM_FLT;

    printf("Length is %f seconds!\n", data.lengthSeconds);
    fflush(stdout);

    nqr::encode_opus_to_disk(params, &data, "test.ogg");
}
