#pragma once
// Refmap analysis core: loudness, spectrum, band balance, stereo width, match-EQ fitting.
// Pure functions, no JUCE GUI — safe to run on a background thread.
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace refmap
{
constexpr int    gridPoints = 192;          // log-frequency grid for spectra
constexpr double gridLo = 20.0, gridHi = 20000.0;
constexpr int    numBands = 6;
inline const char* bandNames[numBands] = { "SUB", "BASS", "LOW MID", "MID", "PRESENCE", "AIR" };
inline const double bandEdges[numBands + 1] = { 20, 60, 150, 500, 2000, 6000, 20000 };

constexpr int numEq = 10;                    // octave-spaced peaking bands for the match EQ
inline const double eqFreqs[numEq] = { 31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
constexpr double eqQ = 1.41;
constexpr float  eqMaxDb = 9.0f;

inline double gridFreq (int i) { return gridLo * std::pow (gridHi / gridLo, i / (double) (gridPoints - 1)); }

// ------------------------------------------------------------------ biquad (raw, no allocation)
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] {}, z2[2] {};

    static Biquad peak (double fs, double f, double q, double gainDb)
    {
        Biquad b;
        const double A = std::pow (10.0, gainDb / 40.0), w = 2.0 * juce::MathConstants<double>::pi * f / fs;
        const double al = std::sin (w) / (2.0 * q), c = std::cos (w), a0 = 1.0 + al / A;
        b.b0 = (1.0 + al * A) / a0; b.b1 = -2.0 * c / a0; b.b2 = (1.0 - al * A) / a0;
        b.a1 = -2.0 * c / a0;       b.a2 = (1.0 - al / A) / a0;
        return b;
    }
    static Biquad highShelf (double fs, double f, double q, double gainDb)
    {
        Biquad b;
        const double A = std::pow (10.0, gainDb / 40.0), w = 2.0 * juce::MathConstants<double>::pi * f / fs;
        const double al = std::sin (w) / (2.0 * q), c = std::cos (w), sA = 2.0 * std::sqrt (A) * al;
        const double a0 = (A + 1) - (A - 1) * c + sA;
        b.b0 = A * ((A + 1) + (A - 1) * c + sA) / a0;
        b.b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
        b.b2 = A * ((A + 1) + (A - 1) * c - sA) / a0;
        b.a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
        b.a2 = ((A + 1) - (A - 1) * c - sA) / a0;
        return b;
    }
    static Biquad highPass (double fs, double f, double q)
    {
        Biquad b;
        const double w = 2.0 * juce::MathConstants<double>::pi * f / fs;
        const double al = std::sin (w) / (2.0 * q), c = std::cos (w), a0 = 1.0 + al;
        b.b0 = (1 + c) / 2 / a0; b.b1 = -(1 + c) / a0; b.b2 = (1 + c) / 2 / a0;
        b.a1 = -2 * c / a0;      b.a2 = (1 - al) / a0;
        return b;
    }
    void setCoeffs (const Biquad& o) { b0 = o.b0; b1 = o.b1; b2 = o.b2; a1 = o.a1; a2 = o.a2; }
    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }

    inline float process (float x, int ch)
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return (float) y;
    }
    double magDb (double f, double fs) const
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * f / fs;
        const std::complex<double> z = std::polar (1.0, -w), z2c = z * z;
        const auto num = b0 + b1 * z + b2 * z2c, den = 1.0 + a1 * z + a2 * z2c;
        return 20.0 * std::log10 (std::abs (num / den) + 1e-20);
    }
};

// ------------------------------------------------------------------ profile
struct Profile
{
    bool  valid = false;
    float lufs = -70, peakDb = -100, plr = 0, corr = 1, width = 0, shortMax = -70;
    std::vector<float> spec;                       // mid spectrum, dB, loudness-normalised, on grid
    std::vector<float> side;                       // side spectrum, same normalisation
    std::array<float, numBands> bandDb {};         // band energy relative to total, dB
    std::array<float, numBands> bandWidth {};      // side share per band, %

    juce::String toString() const
    {
        juce::StringArray s;
        auto add = [&s] (float v) { s.add (juce::String (v, 3)); };
        add (lufs); add (peakDb); add (plr); add (corr); add (width); add (shortMax);
        for (auto v : bandDb) add (v);
        for (auto v : bandWidth) add (v);
        for (auto v : spec) add (v);
        for (auto v : side) add (v);
        return s.joinIntoString (",");
    }
    static Profile fromString (const juce::String& str)
    {
        Profile p;
        juce::StringArray s; s.addTokens (str, ",", {});
        if (s.size() != 6 + 2 * numBands + 2 * gridPoints) return p;
        int k = 0;
        auto next = [&] { return s[k++].getFloatValue(); };
        p.lufs = next(); p.peakDb = next(); p.plr = next(); p.corr = next(); p.width = next(); p.shortMax = next();
        for (auto& v : p.bandDb) v = next();
        for (auto& v : p.bandWidth) v = next();
        p.spec.resize (gridPoints); p.side.resize (gridPoints);
        for (auto& v : p.spec) v = next();
        for (auto& v : p.side) v = next();
        p.valid = true;
        return p;
    }
};

// ------------------------------------------------------------------ K-weighting (BS.1770)
struct KWeight
{
    Biquad shelf, hp;
    explicit KWeight (double fs)
    {
        shelf = Biquad::highShelf (fs, 1681.97, 0.7072, 3.9998);
        hp    = Biquad::highPass  (fs, 38.135, 0.5003);
    }
    inline float process (float x, int ch) { return hp.process (shelf.process (x, ch), ch); }
};

// 400 ms block energies of K-weighted audio, hop 100 ms (75 % overlap)
inline std::vector<double> kBlocks (const juce::AudioBuffer<float>& buf, double fs, int hopMs = 100, int blockMs = 400)
{
    const int nCh = std::min (2, buf.getNumChannels()), n = buf.getNumSamples();
    KWeight kw (fs);
    const int hop = (int) (fs * hopMs / 1000.0), sub = blockMs / hopMs;
    std::vector<double> hopE;
    double acc = 0; int cnt = 0;
    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < nCh; ++c)
        {
            const float y = kw.process (buf.getSample (c, i), c);
            acc += (double) y * y;
        }
        if (++cnt == hop) { hopE.push_back (acc / hop); acc = 0; cnt = 0; }
    }
    if (nCh == 1) for (auto& e : hopE) e *= 2.0;   // treat mono as dual-mono
    std::vector<double> blocks;
    for (size_t i = 0; i + (size_t) sub <= hopE.size(); ++i)
    {
        double e = 0;
        for (int j = 0; j < sub; ++j) e += hopE[i + (size_t) j];
        blocks.push_back (e / sub);
    }
    return blocks;
}

inline float energyToLufs (double e) { return (float) (-0.691 + 10.0 * std::log10 (e + 1e-20)); }

inline float integratedLufs (const std::vector<double>& blocks)
{
    double sum = 0; int n = 0;
    for (auto e : blocks) if (energyToLufs (e) > -70.0f) { sum += e; ++n; }
    if (n == 0) return -70.0f;
    const float rel = energyToLufs (sum / n) - 10.0f;
    sum = 0; n = 0;
    for (auto e : blocks) if (energyToLufs (e) > rel) { sum += e; ++n; }
    return n ? energyToLufs (sum / n) : -70.0f;
}

// ------------------------------------------------------------------ main analysis
inline Profile analyse (const juce::AudioBuffer<float>& buf, double fs)
{
    Profile p;
    const int n = buf.getNumSamples(), nCh = std::min (2, buf.getNumChannels());
    if (n < (int) fs || nCh == 0) return p;
    const float* L = buf.getReadPointer (0);
    const float* R = buf.getReadPointer (nCh > 1 ? 1 : 0);

    // loudness
    const auto blocks = kBlocks (buf, fs);
    p.lufs = integratedLufs (blocks);
    // short-term max (3 s = 30 hops of 100 ms, built from 400 ms blocks every 100 ms -> approximate)
    {
        const auto st = kBlocks (buf, fs, 100, 3000);
        for (auto e : st) p.shortMax = std::max (p.shortMax, energyToLufs (e));
    }

    // peak, correlation
    double sLL = 0, sRR = 0, sLR = 0; float pk = 0;
    for (int i = 0; i < n; ++i)
    {
        pk = std::max ({ pk, std::abs (L[i]), std::abs (R[i]) });
        sLL += (double) L[i] * L[i]; sRR += (double) R[i] * R[i]; sLR += (double) L[i] * R[i];
    }
    p.peakDb = juce::Decibels::gainToDecibels (pk, -100.0f);
    p.plr    = p.peakDb - p.lufs;
    p.corr   = (float) (sLR / std::sqrt (sLL * sRR + 1e-30));

    // spectrum: averaged power of mid and side, FFT 8192, Hann, 50 % overlap
    constexpr int order = 13, N = 1 << order, half = N / 2;
    juce::dsp::FFT fft (order);
    std::vector<float> win ((size_t) N), fm ((size_t) N * 2), fsd ((size_t) N * 2);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * i / (N - 1));
    std::vector<double> pm ((size_t) half + 1, 0.0), ps ((size_t) half + 1, 0.0);
    int frames = 0;
    for (int start = 0; start + N <= n; start += N / 2)
    {
        for (int i = 0; i < N; ++i)
        {
            const float l = L[start + i], r = R[start + i];
            fm[(size_t) i]  = 0.5f * (l + r) * win[(size_t) i];
            fsd[(size_t) i] = 0.5f * (l - r) * win[(size_t) i];
        }
        fft.performRealOnlyForwardTransform (fm.data(), true);
        fft.performRealOnlyForwardTransform (fsd.data(), true);
        for (int k = 0; k <= half; ++k)
        {
            pm[(size_t) k] += (double) fm[(size_t) (2 * k)] * fm[(size_t) (2 * k)] + (double) fm[(size_t) (2 * k + 1)] * fm[(size_t) (2 * k + 1)];
            ps[(size_t) k] += (double) fsd[(size_t) (2 * k)] * fsd[(size_t) (2 * k)] + (double) fsd[(size_t) (2 * k + 1)] * fsd[(size_t) (2 * k + 1)];
        }
        ++frames;
    }
    if (frames == 0) return p;
    const double binHz = fs / N;

    // band energies / widths
    double total = 0;
    for (int k = 1; k <= half; ++k) { const double f = k * binHz; if (f >= 20 && f <= 20000) total += pm[(size_t) k] + ps[(size_t) k]; }
    double mTot = 0, sTot = 0;
    for (int b = 0; b < numBands; ++b)
    {
        double m = 0, s = 0;
        for (int k = 1; k <= half; ++k)
        {
            const double f = k * binHz;
            if (f >= bandEdges[b] && f < bandEdges[b + 1]) { m += pm[(size_t) k]; s += ps[(size_t) k]; }
        }
        mTot += m; sTot += s;
        p.bandDb[(size_t) b]    = (float) (10.0 * std::log10 ((m + s) / (total + 1e-30) + 1e-20));
        p.bandWidth[(size_t) b] = (float) (100.0 * s / (m + s + 1e-30));
    }
    p.width = (float) (100.0 * sTot / (mTot + sTot + 1e-30));

    // 1/6-octave smoothed spectra on the log grid, normalised so spectra at different levels line up
    auto toGrid = [&] (const std::vector<double>& pw)
    {
        std::vector<float> out ((size_t) gridPoints);
        const double bw = std::pow (2.0, 1.0 / 12.0);    // +-1/12 oct = 1/6 oct window
        for (int i = 0; i < gridPoints; ++i)
        {
            const double f = gridFreq (i);
            int k0 = (int) std::floor (f / bw / binHz), k1 = (int) std::ceil (f * bw / binHz);
            k0 = juce::jlimit (1, half, k0); k1 = juce::jlimit (k0, half, k1);
            double s = 0;
            for (int k = k0; k <= k1; ++k) s += pw[(size_t) k];
            s /= (k1 - k0 + 1) * (double) frames;
            // tilt +3 dB/oct (pink-referenced) makes a balanced mix look roughly flat
            out[(size_t) i] = (float) (10.0 * std::log10 (s + 1e-30) + 10.0 * std::log10 (f / 1000.0));
        }
        return out;
    };
    p.spec = toGrid (pm);
    p.side = toGrid (ps);
    // normalise on mean mid level 100 Hz – 10 kHz
    double ref = 0; int cnt = 0;
    for (int i = 0; i < gridPoints; ++i) { const double f = gridFreq (i); if (f >= 100 && f <= 10000) { ref += p.spec[(size_t) i]; ++cnt; } }
    ref /= std::max (1, cnt);
    for (auto& v : p.spec) v -= (float) ref;
    for (auto& v : p.side) v -= (float) ref;

    p.valid = true;
    return p;
}

// ------------------------------------------------------------------ section picker
// returns start sample of the loudest window of `len` samples (the drop), using K-weighted 100 ms hops
inline juce::int64 loudestSection (const std::vector<double>& hopEnergies, int hopsPerWindow, juce::int64 hopSamples)
{
    if ((int) hopEnergies.size() <= hopsPerWindow) return 0;
    double acc = 0, best = -1; size_t bestI = 0;
    for (size_t i = 0; i < hopEnergies.size(); ++i)
    {
        acc += hopEnergies[i];
        if (i >= (size_t) hopsPerWindow) acc -= hopEnergies[i - (size_t) hopsPerWindow];
        if (i + 1 >= (size_t) hopsPerWindow && acc > best) { best = acc; bestI = i + 1 - (size_t) hopsPerWindow; }
    }
    return (juce::int64) bestI * hopSamples;
}

// ------------------------------------------------------------------ match EQ
// target curve = ref - mix, heavily smoothed, faded out at the extremes, clamped
inline std::vector<float> matchTarget (const Profile& mix, const Profile& ref)
{
    std::vector<float> d ((size_t) gridPoints, 0.0f), out ((size_t) gridPoints, 0.0f);
    if (! mix.valid || ! ref.valid) return out;
    for (int i = 0; i < gridPoints; ++i) d[(size_t) i] = ref.spec[(size_t) i] - mix.spec[(size_t) i];
    const double perOct = (gridPoints - 1) / std::log2 (gridHi / gridLo);
    const int r = (int) std::round (perOct / 4.0);        // +-1/4 oct = 1/2 oct smoothing
    for (int i = 0; i < gridPoints; ++i)
    {
        double s = 0; int c = 0;
        for (int j = std::max (0, i - r); j <= std::min (gridPoints - 1, i + r); ++j) { s += d[(size_t) j]; ++c; }
        const double f = gridFreq (i);
        double fade = 1.0;
        if (f < 30)    fade = std::max (0.0, (f - 20) / 10.0);
        if (f > 16000) fade = std::max (0.0, 1.0 - (f - 16000) / 4000.0);
        out[(size_t) i] = juce::jlimit (-eqMaxDb, eqMaxDb, (float) (s / c * fade));
    }
    return out;
}

inline float interpGrid (const std::vector<float>& g, double f)
{
    const double x = std::log (f / gridLo) / std::log (gridHi / gridLo) * (gridPoints - 1);
    const int i = juce::jlimit (0, gridPoints - 2, (int) x);
    const double t = juce::jlimit (0.0, 1.0, x - i);
    return (float) (g[(size_t) i] * (1 - t) + g[(size_t) i + 1] * t);
}

// fit the 10 peaking bands so their summed response hits the target at the band centres
inline std::array<float, numEq> fitEq (const std::vector<float>& target, double fs = 48000.0)
{
    std::array<float, numEq> g {};
    std::array<float, numEq> t {};
    for (int k = 0; k < numEq; ++k) t[(size_t) k] = interpGrid (target, eqFreqs[k]);
    g = t;
    for (int it = 0; it < 40; ++it)
    {
        for (int k = 0; k < numEq; ++k)
        {
            double resp = 0;
            for (int j = 0; j < numEq; ++j) resp += Biquad::peak (fs, eqFreqs[j], eqQ, g[(size_t) j]).magDb (eqFreqs[k], fs);
            g[(size_t) k] = juce::jlimit (-12.0f, 12.0f, g[(size_t) k] + 0.6f * (float) (t[(size_t) k] - resp));
        }
    }
    return g;
}

inline float eqResponseDb (const std::array<float, numEq>& g, double f, double fs = 48000.0)
{
    double r = 0;
    for (int j = 0; j < numEq; ++j) r += Biquad::peak (fs, eqFreqs[j], eqQ, g[(size_t) j]).magDb (f, fs);
    return (float) r;
}

// ------------------------------------------------------------------ advice text
inline juce::String bandAdvice (int b, float diffDb, float mixW, float refW)
{
    juce::String s;
    const float a = std::abs (diffDb);
    if (a < 1.0f)        s = "On target";
    else if (diffDb > 0) s = juce::String (a, 1) + " dB too much";
    else                 s = juce::String (a, 1) + " dB too little";

    if (b == 0 && mixW > 8.0f)                       s << "  |  sub not mono";
    else if (b <= 1 && mixW > refW + 6.0f)           s << "  |  wider than ref";
    else if (b >= 3 && mixW < refW - 8.0f)           s << "  |  narrower than ref";
    else if (b >= 2 && mixW > refW + 10.0f)          s << "  |  wider than ref";
    return s;
}
} // namespace refmap
