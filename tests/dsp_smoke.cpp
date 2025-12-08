#include "common/DspPipeline.h"
#include "common/Logging.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using krkrspeed::DspConfig;
using krkrspeed::DspPipeline;

namespace {

std::vector<std::uint8_t> generateSine(std::size_t frames, std::uint32_t channels, std::uint32_t sampleRate) {
    constexpr double frequency = 440.0; // A4 test tone
    constexpr double pi = 3.14159265358979323846;
    std::vector<std::int16_t> samples(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const auto value = static_cast<std::int16_t>(std::sin(2.0 * pi * frequency * t) * 32767.0);
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            samples[i * channels + ch] = value;
        }
    }

    std::vector<std::uint8_t> bytes(samples.size() * sizeof(std::int16_t));
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

bool runScenario(float ratio, std::uint32_t channels) {
    constexpr std::uint32_t sampleRate = 48000;
    constexpr std::size_t frames = sampleRate / 2; // 0.5 seconds
    const auto input = generateSine(frames, channels, sampleRate);

    DspConfig config{};
    DspPipeline pipeline(sampleRate, channels, config);
    const auto output = pipeline.process(input.data(), input.size(), ratio);

    const double expectedFrames = static_cast<double>(frames) / ratio;
    const double actualFrames = static_cast<double>(output.size()) / (sizeof(std::int16_t) * channels);
    const double error = std::abs(actualFrames - expectedFrames) / expectedFrames;

    std::cout << "Ratio " << ratio << " channels " << channels << ": expected " << expectedFrames
              << " frames, got " << actualFrames << " (" << error * 100.0 << "% error)\n";
    KRKR_LOG_INFO("dsp_smoke ratio=" + std::to_string(ratio) + " channels=" + std::to_string(channels) +
                  " expected_frames=" + std::to_string(expectedFrames) + " actual_frames=" +
                  std::to_string(actualFrames) + " error=" + std::to_string(error * 100.0) + "%");

    // Allow generous tolerance for the naive fallback resampler.
    return error < 0.25; // 25% tolerance is acceptable for smoke test
}

} // namespace

int main() {
    KRKR_LOG_INFO("Starting dsp_smoke scenarios");
    bool ok = true;
    ok &= runScenario(0.75f, 1);
    ok &= runScenario(1.5f, 1);
    ok &= runScenario(2.0f, 2);
    if (!ok) {
        KRKR_LOG_ERROR("dsp_smoke failed tolerance checks");
    } else {
        KRKR_LOG_INFO("dsp_smoke completed successfully");
    }
    return ok ? 0 : 1;
}
