// SPDX-License-Identifier: AGPL-3.0-or-later
#include "NoiseSuppressor.h"
#include <cmath>

void NoiseSuppressor::setParams (const Params& p)
{
    const bool wasEnabled = params.enabled;
    params = p;
    if (sampleRate > 0.0)
    {
        hpfL.setCutoffFrequency (params.hpfHz);
        hpfR.setCutoffFrequency (params.hpfHz);
    }
    // Toggling mid-playback must not leak the previous path's buffered state
    // into the new one.
    if (wasEnabled != params.enabled)
        reset();
}

void NoiseSuppressor::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
    maxBlock = (int) juce::jmax ((juce::uint32) 64, spec.maximumBlockSize);

    juce::dsp::ProcessSpec mono { sampleRate, spec.maximumBlockSize, 1 };
    hpfL.prepare (mono);
    hpfR.prepare (mono);
    hpfL.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    hpfR.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    hpfL.setCutoffFrequency (params.hpfHz);
    hpfR.setCutoffFrequency (params.hpfHz);
    hpfL.setResonance (1.0f / std::sqrt (2.0f));
    hpfR.setResonance (1.0f / std::sqrt (2.0f));

    // sqrt of a periodic Hann, applied on both analysis and synthesis, so the
    // effective window is a periodic Hann. That satisfies COLA exactly at
    // hop = fftSize/2: w(n) + w(n + N/2) == 1.
    //
    // Applying a full Hann twice does not: w²(n) + w²(n + N/2) = 0.5 + 0.5cos²,
    // which ripples between 0.5 and 1.0. That cost 5.8 dB of level and
    // amplitude-modulated the denoised signal at fs/hop (187.5 Hz at 48 kHz).
    window.resize ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
    {
        const float hann = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) fftSize);
        window[(size_t) i] = std::sqrt (hann);
    }

    const int nBins = fftSize / 2 + 1;
    noiseMag.assign ((size_t) nBins, juce::Decibels::decibelsToGain (params.noiseFloorDb));
    magSmooth.assign ((size_t) nBins, juce::Decibels::decibelsToGain (params.noiseFloorDb));
    gainSmooth.assign ((size_t) nBins, 1.0f);
    inFifoL.assign ((size_t) fftSize, 0.0f);
    inFifoR.assign ((size_t) fftSize, 0.0f);
    const int outCap = fftSize + maxBlock * 2;
    outFifoL.assign ((size_t) outCap, 0.0f);
    outFifoR.assign ((size_t) outCap, 0.0f);
    olaL.assign ((size_t) fftSize, 0.0f);
    olaR.assign ((size_t) fftSize, 0.0f);
    fftTime.assign ((size_t) fftSize, 0.0f);
    frameR.assign ((size_t) fftSize, 0.0f);
    fftWork.assign ((size_t) (fftSize * 2), 0.0f);
    reset();
}

void NoiseSuppressor::reset()
{
    hpfL.reset();
    hpfR.reset();
    std::fill (inFifoL.begin(), inFifoL.end(), 0.0f);
    std::fill (inFifoR.begin(), inFifoR.end(), 0.0f);
    std::fill (outFifoL.begin(), outFifoL.end(), 0.0f);
    std::fill (outFifoR.begin(), outFifoR.end(), 0.0f);
    std::fill (olaL.begin(), olaL.end(), 0.0f);
    std::fill (olaR.begin(), olaR.end(), 0.0f);
    std::fill (gainSmooth.begin(), gainSmooth.end(), 1.0f);
    fifoWrite = hopCounter = outRead = outAvail = 0;
    framesSeen = 0;
    frameMin = 1.0e9f;
    adaptNoise = true;
    std::fill (noiseMag.begin(), noiseMag.end(), juce::Decibels::decibelsToGain (params.noiseFloorDb));
    std::fill (magSmooth.begin(), magSmooth.end(), juce::Decibels::decibelsToGain (params.noiseFloorDb));
}

float NoiseSuppressor::binHz (int bin) const
{
    return (float) bin * (float) sampleRate / (float) fftSize;
}

void NoiseSuppressor::spectralGain()
{
    const float amount = juce::jlimit (0.0f, 1.0f, params.amount);
    // Softer curve: Wiener-like, less musical noise / less speech chew
    const float over = params.overSubtract * (0.35f + 0.55f * amount);
    const float floorBase = 0.18f + 0.35f * (1.0f - amount); // never crush to silence
    const float protect = juce::jlimit (0.0f, 1.0f, params.speechProtect);
    const float smooth = 0.35f; // temporal gain smooth

    auto applyBin = [&] (int bin, float& re, float& im)
    {
        const float mag = std::sqrt (re * re + im * im) + 1.0e-12f;
        float& nmag = noiseMag[(size_t) bin];
        float& ms = magSmooth[(size_t) bin];

        ms += magSmoothCoef * (mag - ms);

        // The old rule only adapted when mag < nmag * 1.35, so a floor louder
        // than the -60 dB initial estimate was never learned and the
        // subtraction did nothing at all.
        if (framesSeen < noiseInitFrames)
            nmag = ms;
        else if (ms < nmag)
            nmag += noiseDownCoef * (ms - nmag);
        else if (adaptNoise)
            nmag += noiseUpCoef * (ms - nmag);
        nmag = juce::jmax (nmag, 1.0e-9f);

        const float nEff = nmag * noiseBias;

        // Power spectral subtraction / Wiener
        const float snr = mag / juce::jmax (nEff, 1.0e-12f);
        float g = (snr * snr) / (snr * snr + over); // soft Wiener
        g = juce::jmax (g, 1.0f - over * (nEff / mag));
        g = juce::jlimit (0.0f, 1.0f, g);

        // Protect speech formant band ~250–4000 Hz
        const float f = binHz (bin);
        float speechW = 0.0f;
        if (f >= 250.0f && f <= 4000.0f)
            speechW = 1.0f;
        else if (f > 150.0f && f < 250.0f)
            speechW = (f - 150.0f) / 100.0f;
        else if (f > 4000.0f && f < 6000.0f)
            speechW = 1.0f - (f - 4000.0f) / 2000.0f;

        const float floorG = floorBase + protect * speechW * 0.35f; // higher floor in speech
        g = juce::jmax (g, floorG);
        // Blend toward unity in speech band so amount can't fully strip vowels
        g = g + protect * speechW * (1.0f - g) * 0.45f;

        // Temporal smooth
        float& gs = gainSmooth[(size_t) bin];
        gs += smooth * (g - gs);
        re *= gs;
        im *= gs;
    };

    {
        float re = fftWork[0], im = 0.0f;
        applyBin (0, re, im);
        fftWork[0] = re;
    }
    const int nBins = fftSize / 2;
    for (int k = 1; k < nBins; ++k)
    {
        float re = fftWork[(size_t) (k * 2)];
        float im = fftWork[(size_t) (k * 2 + 1)];
        applyBin (k, re, im);
        fftWork[(size_t) (k * 2)] = re;
        fftWork[(size_t) (k * 2 + 1)] = im;
    }
    {
        float re = fftWork[(size_t) fftSize], im = 0.0f;
        applyBin (nBins, re, im);
        fftWork[(size_t) fftSize] = re;
    }
}

void NoiseSuppressor::processFrame (float* timeL, float* timeR)
{
    // Decide once per frame whether the noise estimate may rise. Frame level is
    // compared with a running minimum: speech and music sit well above it.
    double sum = 0.0;
    for (int i = 0; i < fftSize; ++i)
        sum += (double) timeL[i] * timeL[i] + (double) timeR[i] * timeR[i];
    const float level = (float) std::sqrt (sum / (double) (2 * fftSize)) + 1.0e-12f;
    const float silenceFloor = juce::Decibels::decibelsToGain (silenceFloorDb);

    // Near-silence is a missing signal: it neither adopts the anchor nor
    // drifts it. A silence-padded intro used to lock frameMin at the noise
    // floor, so frameMin * ratio never let the estimate rise and the
    // subtractor never engaged on real material.
    if (level <= silenceFloor)
    {
        /* anchor unchanged */
    }
    else if (level < frameMin)
        frameMin = level;
    else
        frameMin *= frameMinRise;
    adaptNoise = level < frameMin * noiseFrameRatio;

    // Effective window is a periodic Hann summing to 1 at 50% overlap, so no
    // extra overlap-add normalisation is needed.
    auto runChannel = [&] (float* timeIn, std::vector<float>& ola)
    {
        for (int i = 0; i < fftSize; ++i)
            fftWork[(size_t) i] = timeIn[i] * window[(size_t) i];
        for (int i = fftSize; i < fftSize * 2; ++i)
            fftWork[(size_t) i] = 0.0f;

        fft.performRealOnlyForwardTransform (fftWork.data(), true);
        spectralGain();
        fft.performRealOnlyInverseTransform (fftWork.data());

        for (int i = 0; i < fftSize; ++i)
            ola[(size_t) i] += fftWork[(size_t) i] * window[(size_t) i];
    };

    runChannel (timeL, olaL);
    runChannel (timeR, olaR);

    if (framesSeen < noiseInitFrames)
        ++framesSeen;
}

void NoiseSuppressor::process (juce::AudioBuffer<float>& buffer)
{
    const int numCh = buffer.getNumChannels();
    const int n = buffer.getNumSamples();
    if (numCh <= 0 || n <= 0)
        return;

    auto* L = buffer.getWritePointer (0);
    auto* R = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    // Dry path with the STFT delay, so the reported latency stays constant
    // and toggling Denoise (or the amount) never shifts the track.
    auto runDryDelay = [&] ()
    {
        const int cap = (int) outFifoL.size();
        for (int i = 0; i < n; ++i)
        {
            const float l = L[i];
            const float r = R != nullptr ? R[i] : l;
            const int w = (outRead + outAvail) % cap;
            outFifoL[(size_t) w] = l;
            outFifoR[(size_t) w] = r;
            ++outAvail;

            if (outAvail > latencyWhenEnabled)
            {
                --outAvail;
                L[i] = outFifoL[(size_t) outRead];
                if (R != nullptr)
                    R[i] = outFifoR[(size_t) outRead];
                outRead = (outRead + 1) % cap;
            }
            else
            {
                // Still priming the delay line.
                L[i] = 0.0f;
                if (R != nullptr)
                    R[i] = 0.0f;
            }
        }
    };

    if (! params.enabled)
    {
        runDryDelay();
        return;
    }

    // Light HPF only when denoise engaged (avoid coloring dry path)
    for (int i = 0; i < n; ++i)
    {
        L[i] = hpfL.processSample (0, L[i]);
        if (R != nullptr)
            R[i] = hpfR.processSample (0, R[i]);
    }

    // Near-zero amount: HPF only, but still keep the STFT delay.
    if (params.amount < 0.01f)
    {
        runDryDelay();
        return;
    }

    const int outCap = (int) outFifoL.size();

    for (int i = 0; i < n; ++i)
    {
        inFifoL[(size_t) fifoWrite] = L[i];
        inFifoR[(size_t) fifoWrite] = R != nullptr ? R[i] : L[i];
        fifoWrite = (fifoWrite + 1) % fftSize;
        hopCounter++;

        if (hopCounter >= hopSize)
        {
            hopCounter = 0;
            for (int k = 0; k < fftSize; ++k)
            {
                const int idx = (fifoWrite + k) % fftSize;
                fftTime[(size_t) k] = inFifoL[(size_t) idx];
                frameR[(size_t) k] = inFifoR[(size_t) idx];
            }

            for (int k = 0; k < fftSize - hopSize; ++k)
            {
                olaL[(size_t) k] = olaL[(size_t) (k + hopSize)];
                olaR[(size_t) k] = olaR[(size_t) (k + hopSize)];
            }
            for (int k = fftSize - hopSize; k < fftSize; ++k)
            {
                olaL[(size_t) k] = 0.0f;
                olaR[(size_t) k] = 0.0f;
            }

            processFrame (fftTime.data(), frameR.data());

            for (int k = 0; k < hopSize; ++k)
            {
                const int w = (outRead + outAvail) % outCap;
                outFifoL[(size_t) w] = olaL[(size_t) k];
                outFifoR[(size_t) w] = olaR[(size_t) k];
                ++outAvail;
                if (outAvail > outCap)
                {
                    outRead = (outRead + 1) % outCap;
                    outAvail = outCap;
                }
            }
        }

        if (outAvail > 0)
        {
            L[i] = outFifoL[(size_t) outRead];
            if (R != nullptr)
                R[i] = outFifoR[(size_t) outRead];
            outRead = (outRead + 1) % outCap;
            --outAvail;
        }
    }
}
