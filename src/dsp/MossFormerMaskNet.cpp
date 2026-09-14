// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MossFormerMaskNet.h"

#ifdef STP_ENABLE_MOSSFORMER
#include <onnxruntime_cxx_api.h>
#endif

MossFormerMaskNet& MossFormerMaskNet::instance()
{
    // ORT objects deliberately live for the process lifetime. Destruction of
    // a Session after ORT's own registries have started tearing down is not a
    // supported ordering and used to make short-lived validator/test hosts
    // crash after all assertions had passed. Engine destructors still join
    // the loader via finishLoad(), so no code is running when a module unloads.
    static auto* inst = new MossFormerMaskNet;
    return *inst;
}

MossFormerMaskNet::~MossFormerMaskNet()
{
    finishLoad();
}

void MossFormerMaskNet::finishLoad()
{
    const std::lock_guard<std::mutex> lock (loaderMutex);
    if (loader.joinable())
        loader.join();
}

void MossFormerMaskNet::resetForTests()
{
    finishLoad();
    const std::lock_guard<std::mutex> lock (loaderMutex);
    loaderStarted = false;
    session.reset();
    loadState_.store (LoadState::idle, std::memory_order_release);
}

#ifdef STP_ENABLE_MOSSFORMER

namespace
{
/** Process-wide ORT environment. The 1.22 universal2 dylib initialises its
    telemetry from the first Env; doing that on a background thread races and
    crashes, so the Env is created eagerly on the first call, which for every
    entry point in this code base is the host's main thread. */
Ort::Env& sharedEnv()
{
    // Deliberately leaked: the static-destruction order would tear the Env
    // down before the Session that still references it.
    static Ort::Env* env = new Ort::Env (ORT_LOGGING_LEVEL_WARNING,
                                         "SyncTrackPrep-MossFormerDynamic");
    return *env;
}
} // namespace

struct MossFormerMaskNet::Session
{
    std::unique_ptr<Ort::Session> run;

    explicit Session (const juce::File& model)
    {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads (4);
        opts.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
        run = std::make_unique<Ort::Session> (sharedEnv(), model.getFullPathName().toStdString().c_str(), opts);
    }
};

void MossFormerMaskNet::requestLoad (const juce::File& modelFile)
{
    // Force the ORT environment (and its one-shot telemetry init) to come up
    // on the calling thread — the host's main thread. The 1.22 universal2
    // dylib crashes when its telemetry races from a background thread.
    sharedEnv();
    const std::lock_guard<std::mutex> lock (loaderMutex);
    if (loadState_.load (std::memory_order_acquire) == LoadState::failed)
        return;
    if (loaderStarted)
        return;
    loaderStarted = true;
    loadState_.store (LoadState::loading, std::memory_order_release);
    loader = std::thread ([this, modelFile] { loaderLoop (modelFile); });
}

void MossFormerMaskNet::loaderLoop (juce::File modelFile)
{
    try
    {
        session = std::make_unique<Session> (modelFile);
    }
    catch (const Ort::Exception& e)
    {
        const std::lock_guard<std::mutex> lock (errorMutex);
        errorMessage = juce::String ("ORT session: ") + e.what();
        loadState_.store (LoadState::failed, std::memory_order_release);
        return;
    }
    loadState_.store (LoadState::warming, std::memory_order_release);

    // Warm both steady-state shapes so the first realtime window and the
    // first DOP window never pay allocator/optimisation costs.
    std::vector<float> feats ((size_t) 2 * 496 * 180);
    std::vector<float> mask;
    if (! run (feats.data(), 2, 16, mask) || ! run (feats.data(), 2, 496, mask))
    {
        const std::lock_guard<std::mutex> lock (errorMutex);
        if (errorMessage.isEmpty())
            errorMessage = "warm-up inference failed";
        loadState_.store (LoadState::failed, std::memory_order_release);
        return;
    }
    loadState_.store (LoadState::ready, std::memory_order_release);
}

bool MossFormerMaskNet::waitReady (int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto s = loadState_.load (std::memory_order_acquire);
        if (s == LoadState::ready || s == LoadState::failed)
            return s == LoadState::ready;
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    return loadState_.load (std::memory_order_acquire) == LoadState::ready;
}

bool MossFormerMaskNet::run (const float* feats, int batch, int frames,
                             std::vector<float>& maskOut)
{
    if (session == nullptr)
        return false;
    try
    {
        auto memoryInfo = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t, 3> dims { (int64_t) batch, (int64_t) frames, 180 };
        auto input = Ort::Value::CreateTensor<float> (memoryInfo, const_cast<float*> (feats),
                                                      (size_t) batch * frames * 180,
                                                      dims.data(), dims.size());
        const char* inputNames[] = { "feats" };
        const char* outputNames[] = { "mask" };
        auto outputs = session->run->Run (Ort::RunOptions { nullptr },
                                          inputNames, &input, 1, outputNames, 1);
        if (outputs.empty() || ! outputs[0].IsTensor())
            return false;
        auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 3 || shape[0] != batch || shape[1] != frames || shape[2] != 961)
            return false;
        maskOut.resize ((size_t) batch * frames * 961);
        std::memcpy (maskOut.data(), outputs[0].GetTensorData<float>(),
                     maskOut.size() * sizeof (float));
        return true;
    }
    catch (const Ort::Exception&)
    {
        return false;
    }
}

#else // !STP_ENABLE_MOSSFORMER — engine unavailable: inert stubs

struct MossFormerMaskNet::Session {};

void MossFormerMaskNet::requestLoad (const juce::File&) {}
bool MossFormerMaskNet::waitReady (int) { return false; }
bool MossFormerMaskNet::run (const float*, int, int, std::vector<float>&) { return false; }

#endif
