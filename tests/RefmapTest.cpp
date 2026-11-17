// Offline test: synthetic reference + "mix", drives the real processor through a fake host transport.
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"

struct FakeHead : juce::AudioPlayHead
{
    double ppq = 0, bpm = 140; bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing); p.setBpm (bpm); p.setPpqPosition (ppq);
        p.setTimeSignature (TimeSignature { 4, 4 });
        return p;
    }
};

struct Pink
{
    juce::Random rnd { 1234 };
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    float next()
    {
        const float w = rnd.nextFloat() * 2 - 1;
        b0 = 0.99886f * b0 + w * 0.0555179f; b1 = 0.99332f * b1 + w * 0.0750759f;
        b2 = 0.96900f * b2 + w * 0.1538520f; b3 = 0.86650f * b3 + w * 0.3104856f;
        b4 = 0.55000f * b4 + w * 0.5329522f; b5 = -0.7616f * b5 - w * 0.0168980f;
        const float out = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f; b6 = w * 0.115926f;
        return out * 0.11f;
    }
};

static void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

static void printBands (const char* label, const refmap::Profile& m, const refmap::Profile& r)
{
    juce::String s (label);
    for (int k = 0; k < refmap::numBands; ++k)
        s << "  " << refmap::bandNames[k] << " " << juce::String (m.bandDb[(size_t) k] - r.bandDb[(size_t) k], 2);
    std::cout << s << "\n";
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double fs = 48000; const int bs = 512;
    int failures = 0;
    auto check = [&] (bool ok, const juce::String& what) { std::cout << (ok ? "  PASS " : "  FAIL ") << what << "\n"; if (! ok) ++failures; };

    // ---- reference: quiet intro 30 s, loud drop 30 s, quiet outro 20 s; 44.1 kHz to exercise resampling
    const double rfs = 44100;
    auto refFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("refmap_ref.wav");
    {
        Pink pl, pr;
        const int n = (int) (80 * rfs);
        juce::AudioBuffer<float> b (2, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / rfs;
            const float g = (t >= 30 && t < 60) ? 1.0f : 0.1f;
            const float c = pl.next(), s = pr.next();
            b.setSample (0, i, g * (c + 0.3f * s));
            b.setSample (1, i, g * (c - 0.3f * s));
        }
        refFile.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (refFile), rfs, 2, 24, {}, 0));
        w->writeFromAudioSampleBuffer (b, 0, n);
    }

    RefmapProcessor proc;
    FakeHead head;
    proc.setPlayHead (&head);
    proc.setPlayConfigDetails (2, 2, fs, bs);
    proc.prepareToPlay (fs, bs);

    std::cout << "1. load reference\n";
    proc.loadReference (refFile);
    Snapshot s;
    for (int i = 0; i < 100 && ! s.ref.valid; ++i) { pump (50); proc.copySnapshot (s); }
    check (s.ref.valid, "reference analysed");
    std::cout << "     drop start " << s.refStartSec << " s, len " << s.refLenSec << " s, LUFS " << s.ref.lufs << "\n";
    check (s.refStartSec > 29 && s.refStartSec < 47, "loudest section is inside the drop (30-60 s)");
    check (std::abs (s.refLenSec - 8 * 4 * 60 / 140.0) < 0.05, "section length = 8 bars at 140 BPM");

    // ---- mix: same noise family, +6 dB at 60 Hz, -5 dB shelf above 6 kHz, 8 dB quieter, mono-ish
    refmap::Biquad lowBoost = refmap::Biquad::peak (fs, 60, 0.7, 6.0), hiCut = refmap::Biquad::highShelf (fs, 6000, 0.7, -5.0);
    Pink ml, mr;
    auto makeMix = [&] (juce::AudioBuffer<float>& b)
    {
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float c = ml.next(), sd = mr.next();
            for (int ch = 0; ch < 2; ++ch)
            {
                float x = 0.4f * (c + (ch == 0 ? 0.1f : -0.1f) * sd);
                x = hiCut.process (lowBoost.process (x, ch), ch);
                b.setSample (ch, i, x);
            }
        }
    };

    std::cout << "2. capture 8 bars from a mid-bar start\n";
    proc.armCapture();
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> blk (2, bs);
    head.ppq = 2.5;   // middle of bar 1 -> capture should wait until ppq 4
    bool sawRecording = false; double recStartPpq = -1;
    for (int b = 0; b < 2000 && proc.getCaptureState() != RefmapProcessor::done; ++b)
    {
        makeMix (blk);
        const int before = proc.getCaptureState();
        proc.processBlock (blk, midi);
        if (before == RefmapProcessor::armed && proc.getCaptureState() == RefmapProcessor::recording) { sawRecording = true; recStartPpq = head.ppq; }
        head.ppq += bs / fs * head.bpm / 60.0;
    }
    check (sawRecording && recStartPpq <= 4.0 && recStartPpq > 4.0 - bs / fs * 140 / 60.0, "capture started on the bar line (block containing ppq 4)");
    for (int i = 0; i < 100 && proc.getCaptureState() != RefmapProcessor::idle; ++i) pump (50);
    proc.copySnapshot (s);
    check (s.mix.valid, "mix analysed");
    std::cout << "     mix LUFS " << s.mix.lufs << " ref LUFS " << s.ref.lufs << "  width mix " << s.mix.width << " ref " << s.ref.width << "\n";
    printBands ("     band diff (mix-ref):", s.mix, s.ref);
    check (s.mix.bandDb[0] - s.ref.bandDb[0] > 2.0f, "map shows SUB too loud");
    check (s.mix.bandDb[5] - s.ref.bandDb[5] < -2.0f, "map shows AIR too quiet");
    check (std::abs (s.mix.bandDb[3] - s.ref.bandDb[3]) < 1.5f, "MID roughly on target");
    check (s.mix.width < s.ref.width, "mix narrower than ref");
    juce::String gs; for (auto g : s.gains) gs << juce::String (g, 1) << " ";
    std::cout << "     match EQ gains: " << gs << "\n";
    check (s.gains[1] < -2.0f && s.gains[8] > 2.0f, "match EQ cuts 62 Hz and boosts 8 kHz");

    std::cout << "3. apply match EQ 100 % and re-measure\n";
    proc.apvts.getParameter (ids::matchOn)->setValueNotifyingHost (1.0f);
    auto* amt = proc.apvts.getParameter (ids::amount);
    amt->setValueNotifyingHost (amt->convertTo0to1 (100.0f));
    const int n = (int) (14 * fs);
    juce::AudioBuffer<float> out (2, n);
    for (int p = 0; p < n; p += bs)
    {
        makeMix (blk);
        proc.processBlock (blk, midi);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, p, blk, c, 0, std::min (bs, n - p));
    }
    auto after = refmap::analyse (out, fs);
    printBands ("     after EQ (mix-ref):     ", after, s.ref);
    float before = 0, aft = 0;
    for (int k = 0; k < refmap::numBands; ++k) { before += std::abs (s.mix.bandDb[(size_t) k] - s.ref.bandDb[(size_t) k]); aft += std::abs (after.bandDb[(size_t) k] - s.ref.bandDb[(size_t) k]); }
    std::cout << "     total band error " << before << " dB -> " << aft << " dB\n";
    check (aft < before * 0.4f, "match EQ removes most of the tonal gap");

    std::cout << "4. A/B: reference playback, level-matched\n";
    proc.apvts.getParameter (ids::matchOn)->setValueNotifyingHost (0.0f);
    proc.apvts.getParameter (ids::listen)->setValueNotifyingHost (1.0f);
    for (int p = 0; p < n; p += bs)
    {
        makeMix (blk);
        proc.processBlock (blk, midi);
        head.ppq += bs / fs * head.bpm / 60.0;
        for (int c = 0; c < 2; ++c) out.copyFrom (c, p, blk, c, 0, std::min (bs, n - p));
    }
    auto abProf = refmap::analyse (out, fs);
    std::cout << "     A/B ref playback LUFS " << abProf.lufs << " (mix " << s.mix.lufs << ")\n";
    check (std::abs (abProf.lufs - s.mix.lufs) < 1.0f, "reference plays back at the mix's loudness");
    check (std::abs (abProf.bandDb[5] - s.ref.bandDb[5]) < 1.0f, "playback is the reference (AIR matches ref)");

    std::cout << "5. state save / restore\n";
    juce::MemoryBlock mb;
    proc.getStateInformation (mb);
    {
        RefmapProcessor p2;
        p2.setPlayConfigDetails (2, 2, fs, bs);
        p2.prepareToPlay (fs, bs);
        p2.setStateInformation (mb.getData(), (int) mb.getSize());
        Snapshot s2; p2.copySnapshot (s2);
        check (s2.mix.valid && s2.ref.valid && std::abs (s2.gains[8] - s.gains[8]) < 0.05f, "profiles and match EQ restored");
        for (int i = 0; i < 100 && p2.hasReferenceAudio() == false; ++i) pump (50);
        check (p2.hasReferenceAudio(), "reference audio reloaded from disk");

        // GUI snapshot
        if (auto* ed = dynamic_cast<RefmapEditor*> (p2.createEditor()))
        {
            pump (200);
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            auto png = juce::File::getCurrentWorkingDirectory().getChildFile ("screenshot.png");
            png.deleteFile();
            juce::FileOutputStream os (png);
            juce::PNGImageFormat().writeImageToStream (img, os);
            std::cout << "     wrote " << png.getFullPathName() << "\n";
            delete ed;
        }
    }

    std::cout << (failures == 0 ? "ALL PASSED\n" : juce::String (failures) + " FAILED\n");
    refFile.deleteFile();
    return failures == 0 ? 0 : 1;
}
