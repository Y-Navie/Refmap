#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "Analysis.h"

namespace ids
{
inline constexpr const char* bars    = "bars";      // 4 / 8 / 16
inline constexpr const char* matchOn = "matchOn";
inline constexpr const char* amount  = "amount";
inline constexpr const char* listen  = "listen";    // Mix / Reference
inline constexpr const char* output  = "output";
}

// Everything the GUI needs to draw, copied out in one go
struct Snapshot
{
    refmap::Profile mix, ref;
    std::vector<float> target;
    std::array<float, refmap::numEq> gains {};
    juce::String refName, status;
    double refStartSec = 0, refLenSec = 0;
    int version = -1;
};

class RefmapProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    enum CaptureState { idle = 0, armed, recording, done, analysing };

    RefmapProcessor();
    ~RefmapProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    using AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Refmap"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- GUI side ----
    void loadReference (const juce::File&);
    void armCapture();
    void cancelCapture();
    void clearMix();
    int  getCaptureState() const       { return capState.load(); }
    float getCaptureProgress() const;  // 0..1
    int  getCaptureBars() const        { return capBars.load(); }
    double getHostBpm() const          { return hostBpm.load(); }
    int  getVersion() const            { return version.load(); }
    void copySnapshot (Snapshot&);
    bool hasReferenceAudio();

    juce::AudioProcessorValueTreeState apvts;
    juce::AudioFormatManager formats;

private:
    struct RefAudio : juce::ReferenceCountedObject
    {
        using Ptr = juce::ReferenceCountedObjectPtr<RefAudio>;
        juce::AudioBuffer<float> buf;   // reference section at host rate
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void startReferenceJob (const juce::File&);
    void startMixJob();
    void updateMatch();               // call with dataLock held
    void setStatus (const juce::String&);
    int  barsSetting() const;

    double sr = 44100.0;
    std::atomic<double> hostSr { 44100.0 }, hostBpm { 140.0 };
    std::atomic<int> hostBeatsPerBar { 4 };

    // capture
    juce::AudioBuffer<float> captureBuf;
    std::atomic<int> capState { idle }, capLen { 0 }, capPos { 0 }, capBars { 8 };

    // shared analysis data
    juce::CriticalSection dataLock;
    refmap::Profile mixProfile, refProfile;
    std::vector<float> target;
    std::array<float, refmap::numEq> gains {};
    juce::String refName, status, refPath;
    double refStartSec = 0, refLenSec = 0;
    std::atomic<int> version { 0 };
    int lastRefBars = -1;
    double lastRefSr = 0;

    // match EQ (audio thread)
    std::array<std::atomic<float>, refmap::numEq> eqTarget;
    float eqCur[refmap::numEq] {};
    refmap::Biquad eq[refmap::numEq];
    juce::SmoothedValue<float> outSm, abSm, refGainSm;
    std::atomic<float> refGainDb { 0.0f };

    // reference playback
    juce::SpinLock refLock;
    RefAudio::Ptr refAudio;
    juce::int64 refPos = 0;
    juce::AudioBuffer<float> refScratch;

    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withNumberOfThreads (1).withThreadName ("Refmap analysis") };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RefmapProcessor)
};
