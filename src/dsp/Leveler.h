// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <vector>

#include <juce_dsp/juce_dsp.h>

/** Dual-timescale auto-gain: fast for speech, slow for scene loudness. */
class Leveler
{
public:
    struct Params
    {
        bool enabled = true;
        float targetDb = -18.0f;
        float strength = 0.65f;    // 0..1
        float maxGainDb = 16.0f;
        float maxAttenDb = 12.0f;
        float gateDb = -46.0f;
        float fastTauSec = 0.04f;
        float slowTauSec = 3.0f;   // scene leveling (0–14 vs 15+)
        float attackSec = 0.15f;   // gain-down
        float releaseSec = 0.55f;  // gain-up toward scene target
    };

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParams (const Params& p) { params = p; }

    void process (juce::AudioBuffer<float>& buffer);

    float getLastGainDb() const { return lastGainDb; }

    /** Scene level as the downstream peak compressor sees it, in dB (floor -100).
        slowEnv tracks the input-side scene; the applied gain is what the leveler
        actually delivered, so the product is the post-leveler scene level the
        compressor should reference its threshold against. */
    float getSlowEnvDb() const
    {
        return juce::Decibels::gainToDecibels (slowEnv * gain, -100.0f);
    }

    /** Per-sample post-gain scene level for the block just processed.
        The compressor and expander run on the same block right after the
        leveler and index this stream sample-by-sample, so the scene threshold
        stream is continuous and block-size independent. */
    const std::vector<float>& sceneStream() const { return sceneStreamBuf; }

    /** Guarantees n valid entries in the scene stream (grows only outside the
        audio thread's normal path: prepare() sizes it for the declared block). */
    void ensureSceneStream (int n)
    {
        if ((int) sceneStreamBuf.size() < n)
            sceneStreamBuf.resize ((size_t) n,
                                   juce::Decibels::gainToDecibels (slowEnv * gain, -100.0f));
        const float cur = juce::Decibels::gainToDecibels (slowEnv * gain, -100.0f);
        for (int i = 0; i < n; ++i)
            sceneStreamBuf[(size_t) i] = cur;
    }

    /**
     * Noise-aware gating.
     *
     * A steady noise floor sits above the level gate and looks exactly like
     * quiet dialogue to a loudness chaser, so without this the leveler lifts
     * room tone by its full max-gain and undoes the denoiser (metric C
     * conflict). Dialogue carries syllabic amplitude modulation; a steady
     * floor does not. The discriminator measures the fast envelope's
     * deviation from its own short-time mean:
     *
     *   dev = |fastEnv - fastAvg| / fastAvg,  smoothed into modAvg
     *
     * Steady broadband noise hovers around 2-3% deviation; syllabic speech
     * swings 30-100%. Whisper is amplitude-modulated noise and stays on the
     * dialogue side of the line (a ZCR-based detector would fail exactly
     * there). SlowEnv is deliberately NOT the reference: it lags seconds
     * behind a loud-to-soft transition and would keep the frame classified
     * as content long after the speech ended.
     */
    static constexpr float fastAvgTauSec = 0.20f;  ///< mean level the deviation is measured against
    static constexpr float modTauSec = 0.12f;     ///< smoothing of the modulation estimate
    static constexpr float modEnterThr = 0.08f;  ///< below: steady, gate engages
    static constexpr float modExitThr = 0.15f;   ///< above: modulated, gate releases
    static constexpr float noiseHoldTauSec = 0.35f; ///< gain drift toward unity while gated

    /**
     * The modulation gate only engages for low-modulation AND low-level
     * content. A sustained vowel has almost no envelope modulation (mod
     * ~0.02) but sits at dialogue level; without the level condition the
     * gate kills its gain mid-phrase. Real air noise escapes both conditions
     * (it modulates ~0.2), which is the known metric-C gap.
     */
    static constexpr float quietFloorDb = -30.0f;

private:
    Params params;
    double sampleRate = 48000.0;
    float fastEnv = 0.0f;
    float slowEnv = 0.0f;
    float gain = 1.0f;
    std::vector<float> sceneStreamBuf;
    float lastGainDb = 0.0f;

    float fastAvg = 0.0f;
    float modAvg = 0.0f;
    bool noiseHold = false;
};
