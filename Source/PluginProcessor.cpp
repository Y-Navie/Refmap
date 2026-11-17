#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
const int barChoices[] = { 4, 8, 16 };
constexpr double minCaptureBpm = 90.0;   // capture buffer is sized for 16 bars of 4/4 at this tempo
}

RefmapProcessor::RefmapProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    formats.registerBasicFormats();
    for (auto& g : eqTarget) g.store (0.0f);
    status = "Load a reference track, then capture a loop of your mix.";
    startTimerHz (10);
}

RefmapProcessor::~RefmapProcessor()
{
    stopTimer();
    alive->store (false);
    pool.removeAllJobs (true, 10000);
}

juce::AudioProcessorValueTreeState::ParameterLayout RefmapProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto db  = [] (float v, int) { return String (v, 1) + " dB"; };
    auto pct = [] (float v, int) { return String (roundToInt (v)) + " %"; };

    l.add (std::make_unique<AudioParameterChoice> (ParameterID { ids::bars, 1 }, "Loop Length",
            StringArray { "4 bars", "8 bars", "16 bars" }, 1));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { ids::matchOn, 1 }, "Match EQ", false));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { ids::amount, 1 }, "Match Amount",
            NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction (pct)));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { ids::listen, 1 }, "Listen",
            StringArray { "Mix", "Reference" }, 0));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { ids::output, 1 }, "Output",
            NormalisableRange<float> (-24.0f, 12.0f, 0.01f), 0.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction (db)));
    return l;
}

bool RefmapProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return out == layouts.getMainInputChannelSet();
}

int RefmapProcessor::barsSetting() const
{
    return barChoices[juce::jlimit (0, 2, (int) apvts.getRawParameterValue (ids::bars)->load())];
}

void RefmapProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    hostSr.store (sr);

    const int cap = (int) std::ceil (16 * 4 * 60.0 / minCaptureBpm * sr);
    if (captureBuf.getNumSamples() != cap)
    {
        if (capState.load() == analysing) pool.removeAllJobs (true, 10000);
        captureBuf.setSize (2, cap, false, true, false);
        capState.store (idle);
    }
    refScratch.setSize (2, std::max (1, samplesPerBlock), false, true, false);

    for (auto& b : eq) b.reset();
    for (auto& c : eqCur) c = 1000.0f;    // force coefficient refresh on first block
    outSm.reset (sr, 0.05);
    outSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (apvts.getRawParameterValue (ids::output)->load()));
    abSm.reset (sr, 0.03);
    abSm.setCurrentAndTargetValue (apvts.getRawParameterValue (ids::listen)->load() > 0.5f ? 1.0f : 0.0f);
    refGainSm.reset (sr, 0.1);
    refGainSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (refGainDb.load()));
    refPos = 0;
}

void RefmapProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int nCh = std::min (2, getTotalNumInputChannels());
    const int n = buffer.getNumSamples();
    if (nCh == 0 || n == 0) return;

    // ---- host transport ----
    bool playing = false;
    juce::Optional<double> ppq;
    double bpm = hostBpm.load();
    int bpbNum = 4, bpbDen = 4;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            ppq = pos->getPpqPosition();
            if (auto b = pos->getBpm()) { bpm = *b; hostBpm.store (bpm); }
            if (auto ts = pos->getTimeSignature()) { bpbNum = std::max (1, ts->numerator); bpbDen = std::max (1, ts->denominator); }
        }
    hostBeatsPerBar.store (bpbNum);
    const double samplesPerQuarter = sr * 60.0 / std::max (20.0, bpm);
    const double barPpq = bpbNum * 4.0 / bpbDen;

    // ---- loop capture (pre-EQ input) ----
    int st = capState.load();
    if (st == armed && playing)
    {
        int offset = 0;
        if (ppq)
        {
            const double nextBar = std::ceil (*ppq / barPpq - 1.0e-6) * barPpq;
            offset = (int) std::llround ((nextBar - *ppq) * samplesPerQuarter);
        }
        if (offset < n)
        {
            const int wanted = (int) std::llround (capBars.load() * barPpq * samplesPerQuarter);
            capLen.store (std::min (wanted, captureBuf.getNumSamples()));
            capPos.store (0);
            capState.store (st = recording);
            const int cnt = std::min (n - offset, capLen.load());
            for (int c = 0; c < 2; ++c)
                captureBuf.copyFrom (c, 0, buffer, std::min (c, nCh - 1), offset, cnt);
            capPos.store (cnt);
        }
    }
    else if (st == recording)
    {
        if (! playing)
        {
            capState.store (armed);           // transport stopped mid-capture: start over on next play
            capPos.store (0);
        }
        else
        {
            const int p = capPos.load(), cnt = std::min (n, capLen.load() - p);
            if (cnt > 0)
                for (int c = 0; c < 2; ++c)
                    captureBuf.copyFrom (c, p, buffer, std::min (c, nCh - 1), 0, cnt);
            capPos.store (p + std::max (0, cnt));
            if (p + cnt >= capLen.load()) capState.store (done);
        }
    }

    // ---- match EQ ----
    const bool  matchOn = apvts.getRawParameterValue (ids::matchOn)->load() > 0.5f;
    const float amount  = apvts.getRawParameterValue (ids::amount)->load() * 0.01f;
    bool anyEq = false;
    for (int k = 0; k < refmap::numEq; ++k)
    {
        const float want = matchOn ? eqTarget[(size_t) k].load() * amount : 0.0f;
        if (eqCur[k] != want)
        {
            eqCur[k] = std::abs (want - eqCur[k]) < 0.02f || eqCur[k] > 100.0f ? want : eqCur[k] + 0.3f * (want - eqCur[k]);
            eq[k].setCoeffs (refmap::Biquad::peak (sr, std::min (refmap::eqFreqs[k], sr * 0.45), refmap::eqQ, eqCur[k]));
        }
        anyEq |= eqCur[k] != 0.0f;
    }
    if (anyEq)
    {
        for (int c = 0; c < nCh; ++c)
        {
            auto* d = buffer.getWritePointer (c);
            for (int i = 0; i < n; ++i)
            {
                float x = d[i];
                for (auto& b : eq) x = b.process (x, c);
                d[i] = x;
            }
        }
    }
    else
        for (auto& b : eq) b.reset();

    // ---- A/B with level-matched reference ----
    abSm.setTargetValue (apvts.getRawParameterValue (ids::listen)->load() > 0.5f ? 1.0f : 0.0f);
    refGainSm.setTargetValue (juce::Decibels::decibelsToGain (refGainDb.load()));
    if (abSm.isSmoothing() || abSm.getTargetValue() > 0.0f)
    {
        const juce::SpinLock::ScopedTryLockType lock (refLock);
        const auto* ra = lock.isLocked() ? refAudio.get() : nullptr;
        const int len = ra != nullptr ? ra->buf.getNumSamples() : 0;

        juce::int64 pos = refPos;
        if (len > 0 && playing && ppq) pos = (juce::int64) std::llround (*ppq * samplesPerQuarter);
        for (int i = 0; i < n; ++i)
        {
            const float ab = abSm.getNextValue(), rg = refGainSm.getNextValue();
            float r[2] = { 0, 0 };
            if (len > 0)
            {
                const int idx = (int) (((pos + i) % len + len) % len);
                r[0] = ra->buf.getSample (0, idx);
                r[1] = ra->buf.getSample (1, idx);
            }
            if (nCh == 1) r[0] = 0.5f * (r[0] + r[1]);
            for (int c = 0; c < nCh; ++c)
            {
                auto* d = buffer.getWritePointer (c);
                d[i] = d[i] * (1.0f - ab) + r[c] * rg * ab;
            }
        }
        refPos = pos + n;
    }
    else
        refPos += n;

    // ---- output ----
    outSm.setTargetValue (juce::Decibels::decibelsToGain (apvts.getRawParameterValue (ids::output)->load()));
    if (outSm.isSmoothing() || std::abs (outSm.getTargetValue() - 1.0f) > 1.0e-6f)
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = outSm.getNextValue();
            for (int c = 0; c < nCh; ++c) buffer.getWritePointer (c)[i] *= g;
        }
    }
    for (int c = nCh; c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);
}

// ====================================================================== message-thread side

void RefmapProcessor::timerCallback()
{
    if (capState.load() == done)
    {
        capState.store (analysing);
        startMixJob();
    }

    // re-pick the reference section if loop length / sample rate / tempo changed
    juce::String path;
    bool stale = false;
    {
        const juce::ScopedLock l (dataLock);
        path = refPath;
        stale = path.isNotEmpty() && pool.getNumJobs() == 0
             && (lastRefBars != barsSetting() || std::abs (lastRefSr - hostSr.load()) > 1.0);
    }
    if (stale) startReferenceJob (juce::File (path));
}

void RefmapProcessor::setStatus (const juce::String& s)
{
    const juce::ScopedLock l (dataLock);
    status = s;
    ++version;
}

void RefmapProcessor::loadReference (const juce::File& f)
{
    startReferenceJob (f);
}

void RefmapProcessor::startReferenceJob (const juce::File& file)
{
    const int    bars = barsSetting();
    const double bpm  = hostBpm.load(), outSr = hostSr.load();
    const int    bpb  = hostBeatsPerBar.load();
    {
        const juce::ScopedLock l (dataLock);
        lastRefBars = bars; lastRefSr = outSr;     // prevents the timer from re-queuing while this runs
    }
    auto flag = alive;

    pool.addJob ([this, file, bars, bpm, outSr, bpb, flag]
    {
        if (! flag->load()) return;
        setStatus ("Analysing reference: " + file.getFileName() + " ...");

        std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (file));
        if (r == nullptr || r->lengthInSamples < (juce::int64) r->sampleRate)
        {
            setStatus ("Couldn't read " + file.getFileName() + " (WAV / AIFF / FLAC / MP3 / OGG supported)");
            return;
        }
        const double fs = r->sampleRate;
        const juce::int64 total = r->lengthInSamples;

        // pass 1: K-weighted energy every 100 ms over the whole track
        refmap::KWeight kw (fs);
        const int hop = (int) (fs * 0.1);
        std::vector<double> hopE;
        {
            constexpr int chunk = 1 << 16;
            juce::AudioBuffer<float> tmp (2, chunk);
            double acc = 0; int cnt = 0;
            for (juce::int64 p = 0; p < total; p += chunk)
            {
                if (! flag->load()) return;
                const int num = (int) std::min<juce::int64> (chunk, total - p);
                r->read (&tmp, 0, num, p, true, true);
                if (r->numChannels == 1) tmp.copyFrom (1, 0, tmp, 0, 0, num);
                for (int i = 0; i < num; ++i)
                {
                    for (int c = 0; c < 2; ++c) { const float y = kw.process (tmp.getSample (c, i), c); acc += (double) y * y; }
                    if (++cnt == hop) { hopE.push_back (acc / hop); acc = 0; cnt = 0; }
                }
            }
        }

        // pass 2: grab the loudest N-bar window (the drop) and analyse it
        const double lenSec = bars * bpb * 60.0 / std::max (20.0, bpm);
        const int hopsPerWin = std::max (1, (int) std::lround (lenSec / 0.1));
        const juce::int64 start = refmap::loudestSection (hopE, hopsPerWin, hop);
        const int len = (int) std::min<juce::int64> ((juce::int64) std::llround (lenSec * fs), total - start);

        juce::AudioBuffer<float> sec (2, len);
        r->read (&sec, 0, len, start, true, true);
        if (r->numChannels == 1) sec.copyFrom (1, 0, sec, 0, 0, len);
        auto prof = refmap::analyse (sec, fs);
        if (! prof.valid) { setStatus ("Reference too short to analyse."); return; }

        // resample the section to the host rate for A/B playback
        RefAudio::Ptr ra = new RefAudio();
        const double ratio = fs / outSr;
        const int outLen = (int) std::floor (len / ratio) - 4;
        ra->buf.setSize (2, std::max (1, outLen));
        ra->buf.clear();
        if (outLen > 0)
            for (int c = 0; c < 2; ++c)
            {
                juce::LagrangeInterpolator li;
                li.process (ratio, sec.getReadPointer (c), ra->buf.getWritePointer (c), outLen);
            }

        if (! flag->load()) return;
        {
            const juce::ScopedLock l (dataLock);
            refProfile = prof;
            refName = file.getFileNameWithoutExtension();
            refPath = file.getFullPathName();
            refStartSec = (double) start / fs;
            refLenSec = len / fs;
            updateMatch();
            status = "Reference: loudest " + juce::String (bars) + " bars picked. "
                   + (mixProfile.valid ? "Map ready." : "Now capture your loop.");
            ++version;
        }
        {
            const juce::SpinLock::ScopedLockType l (refLock);
            std::swap (refAudio, ra);       // old buffer is released here, off the audio thread
        }
    });
}

void RefmapProcessor::startMixJob()
{
    auto flag = alive;
    const double fs = sr;
    pool.addJob ([this, flag, fs]
    {
        if (! flag->load()) return;
        const int len = capLen.load();
        juce::AudioBuffer<float> copy (2, len);
        for (int c = 0; c < 2; ++c) copy.copyFrom (c, 0, captureBuf, c, 0, len);
        auto prof = refmap::analyse (copy, fs);

        const juce::ScopedLock l (dataLock);
        if (! prof.valid || prof.lufs < -60.0f)
            status = "Captured silence. Press play with your loop running, then capture again.";
        else
        {
            mixProfile = prof;
            updateMatch();
            status = refProfile.valid ? "Map ready." : "Loop captured. Load a reference to compare.";
        }
        capState.store (idle);
        ++version;
    });
}

void RefmapProcessor::updateMatch()
{
    if (mixProfile.valid && refProfile.valid)
    {
        target = refmap::matchTarget (mixProfile, refProfile);
        gains  = refmap::fitEq (target, hostSr.load());
        refGainDb.store (juce::jlimit (-24.0f, 12.0f, mixProfile.lufs - refProfile.lufs));
    }
    else
    {
        target.assign ((size_t) refmap::gridPoints, 0.0f);
        gains.fill (0.0f);
        refGainDb.store (0.0f);
    }
    for (int k = 0; k < refmap::numEq; ++k) eqTarget[(size_t) k].store (gains[(size_t) k]);
}

void RefmapProcessor::armCapture()
{
    if (capState.load() == analysing) return;
    capBars.store (barsSetting());
    capPos.store (0);
    capState.store (armed);
    setStatus ("Armed: capture starts at the next bar when the transport plays.");
}

void RefmapProcessor::cancelCapture()
{
    if (capState.load() == analysing) return;
    capState.store (idle);
    setStatus ("Capture cancelled.");
}

void RefmapProcessor::clearMix()
{
    const juce::ScopedLock l (dataLock);
    mixProfile = {};
    updateMatch();
    status = "Loop cleared.";
    ++version;
}

float RefmapProcessor::getCaptureProgress() const
{
    const int len = capLen.load();
    return len > 0 ? juce::jlimit (0.0f, 1.0f, capPos.load() / (float) len) : 0.0f;
}

bool RefmapProcessor::hasReferenceAudio()
{
    const juce::SpinLock::ScopedLockType l (refLock);
    return refAudio != nullptr;
}

void RefmapProcessor::copySnapshot (Snapshot& s)
{
    const juce::ScopedLock l (dataLock);
    s.mix = mixProfile; s.ref = refProfile; s.target = target; s.gains = gains;
    s.refName = refName; s.status = status; s.refStartSec = refStartSec; s.refLenSec = refLenSec;
    s.version = version.load();
}

juce::AudioProcessorEditor* RefmapProcessor::createEditor() { return new RefmapEditor (*this); }

void RefmapProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    {
        const juce::ScopedLock l (dataLock);
        state.setProperty ("refPath", refPath, nullptr);
        state.setProperty ("refName", refName, nullptr);
        state.setProperty ("refStart", refStartSec, nullptr);
        state.setProperty ("refLen", refLenSec, nullptr);
        state.setProperty ("mixProfile", mixProfile.valid ? mixProfile.toString() : juce::String(), nullptr);
        state.setProperty ("refProfile", refProfile.valid ? refProfile.toString() : juce::String(), nullptr);
    }
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void RefmapProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    auto state = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (state);

    juce::String path;
    {
        const juce::ScopedLock l (dataLock);
        refPath     = state.getProperty ("refPath").toString();
        refName     = state.getProperty ("refName").toString();
        refStartSec = state.getProperty ("refStart");
        refLenSec   = state.getProperty ("refLen");
        mixProfile  = refmap::Profile::fromString (state.getProperty ("mixProfile").toString());
        refProfile  = refmap::Profile::fromString (state.getProperty ("refProfile").toString());
        updateMatch();                       // EQ is correct immediately, before the reference reloads
        path = refPath;
        lastRefBars = barsSetting(); lastRefSr = hostSr.load();
        status = refProfile.valid ? "Session restored." : status;
        ++version;
    }
    if (path.isNotEmpty())
    {
        const juce::File f (path);
        if (f.existsAsFile()) startReferenceJob (f);
        else setStatus ("Reference file missing: " + path);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RefmapProcessor(); }
