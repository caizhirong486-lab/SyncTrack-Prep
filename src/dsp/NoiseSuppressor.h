// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>

/**
 * HPF + spectral denoise v2 (STFT/OLA).
 * Speech-band protection reduces dialogue distortion (Clean mode).
 */
class NoiseSuppressor
{
public:
    static constexpr int fftOrder = 9;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int hopSize = fftSize / 2;

    struct Params
    {
        bool enabled = false;
        float amount = 0.5f;         // 0..1 overall
        float hpfHz = 80.0f;
        float noiseFloorDb = -60.0f;
        float overSubtract = 1.1f;   // keep modest to avoid musical noise
        float speechProtect = 0.7f;  // 0..1 how much to protect 300–4kHz
    };

    /** Noise-floor tracker constants.
        A hard per-bin minimum locks onto the lower edge of the Rayleigh
        fluctuation, far below the mean, and then barely subtracts anything.
        So: smooth the magnitude, track it asymmetrically (down fast, up only on
        noise-like frames), and scale by a bias factor to recover the mean. */
    static constexpr float magSmoothCoef = 0.3f;
    static constexpr float noiseDownCoef = 0.3f;
    static constexpr float noiseUpCoef = 0.08f;
    static constexpr float noiseBias = 2.0f;
    /** Frames of straight adoption at startup, so the estimate does not spend
        seconds creeping up from the initial guess. */
    static constexpr int noiseInitFrames = 16;
    /** A frame counts as noise while it stays within this ratio of the running
        minimum frame level. Without the gate, a sustained vowel or tone is
        learned as noise and then subtracted from itself. */
    static constexpr float noiseFrameRatio = 2.0f;
    /** Slow upward drift of the running minimum, so it can follow a rising
        room tone instead of latching onto the quietest moment forever. */
    static constexpr float frameMinRise = 1.0005f;
    /** Near-silence is a missing signal, not a quiet one: frames at or below
        this level never update the running minimum. -50 dB covers dialogue
        gaps and breath intakes, which sit far below any noise floor worth
        estimating; without this, the quietest inter-phrase gap anchors the
        gate and the real noise section never gets to raise the estimate. */
    static constexpr float silenceFloorDb = -50.0f;

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParams (const Params& p);

    void process (juce::AudioBuffer<float>& buffer);

    /** Algorithmic delay of the STFT path.
        The first emitted sample is the oldest sample of the first completed
        frame, which is fftSize - 1 samples behind the input.
        This is reported even when the denoiser is disabled: the disabled path
        runs a dry delay line of the same length, so toggling Denoise never
        changes the reported latency and hosts do not need to re-compensate. */
    static constexpr int latencyWhenEnabled = fftSize - 1;

    int getLatencySamples() const { return latencyWhenEnabled; }

private:
    Params params;
    double sampleRate = 48000.0;
    int maxBlock = 512;

    juce::dsp::StateVariableTPTFilter<float> hpfL, hpfR;
    juce::dsp::FFT fft { fftOrder };

    std::vector<float> window;
    std::vector<float> noiseMag;
    std::vector<float> magSmooth;  // per-bin short EMA feeding the tracker
    std::vector<float> gainSmooth; // per-bin temporal smooth
    std::vector<float> inFifoL, inFifoR;
    std::vector<float> outFifoL, outFifoR;
    std::vector<float> olaL, olaR;
    std::vector<float> fftTime, frameR, fftWork;
    int fifoWrite = 0;
    int hopCounter = 0;
    int outRead = 0;
    int outAvail = 0;
    int framesSeen = 0;
    float frameMin = 1.0e9f;
    bool adaptNoise = true;

    void processFrame (float* timeL, float* timeR);
    void spectralGain();
    float binHz (int bin) const;
};
