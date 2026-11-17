#include "PluginEditor.h"

namespace
{
constexpr int W = 980, H = 700;
juce::Font font (float h, bool bold = false) { return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain)); }

juce::String fmtFreq (double f)
{
    return f >= 1000.0 ? juce::String (f / 1000.0, f >= 10000.0 ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (f)) + " Hz";
}
juce::String fmtTime (double s)
{
    const int t = (int) s;
    return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
}
juce::String num (float v, int dec) { return dec == 0 ? juce::String (juce::roundToInt (v)) : juce::String (v, dec); }
juce::String signedDb (float v, int dec = 1) { return (v >= 0 ? "+" : "") + num (v, dec); }
juce::Colour severity (float absDb)
{
    if (absDb < 1.0f) return colours::good;
    if (absDb < 3.0f) return colours::ref.interpolatedWith (colours::good, 0.25f);
    return colours::bad;
}
}

// ====================================================================== SpectrumView

float SpectrumView::xFor (double f) const
{
    return (float) (std::log (f / refmap::gridLo) / std::log (refmap::gridHi / refmap::gridLo)) * (float) getWidth();
}
double SpectrumView::freqFor (float x) const
{
    return refmap::gridLo * std::pow (refmap::gridHi / refmap::gridLo, juce::jlimit (0.0f, 1.0f, x / (float) getWidth()));
}

void SpectrumView::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (b, 8.0f);

    const float split = b.getHeight() * 0.66f;
    auto top = b.withHeight (split).reduced (0, 8);
    auto bot = b.withTrimmedTop (split + 4).withTrimmedBottom (8);
    constexpr float topLo = -36.0f, topHi = 12.0f, botRange = 12.0f;
    auto yTop = [&] (float db) { return top.getBottom() - (juce::jlimit (topLo, topHi, db) - topLo) / (topHi - topLo) * top.getHeight(); };
    auto yBot = [&] (float db) { return bot.getCentreY() - juce::jlimit (-botRange, botRange, db) / botRange * bot.getHeight() * 0.5f; };

    // grid
    g.setFont (font (10.0f));
    for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        const float x = xFor (f);
        g.setColour (colours::outline.withAlpha (0.7f));
        g.drawVerticalLine ((int) x, top.getY(), bot.getBottom());
        g.setColour (colours::textDim);
        g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f), (int) x + 3, (int) top.getBottom() - 14, 40, 12, juce::Justification::left);
    }
    g.setColour (colours::outline);
    g.drawHorizontalLine ((int) (split + 2), b.getX() + 6, b.getRight() - 6);
    g.drawHorizontalLine ((int) bot.getCentreY(), b.getX(), b.getRight());
    for (float d : { -6.0f, 6.0f })
    {
        g.setColour (colours::outline.withAlpha (0.4f));
        g.drawHorizontalLine ((int) yBot (d), b.getX(), b.getRight());
    }
    // band labels
    g.setColour (colours::textDim.withAlpha (0.5f));
    for (int k = 0; k < refmap::numBands; ++k)
    {
        const float x0 = xFor (refmap::bandEdges[k]), x1 = xFor (refmap::bandEdges[k + 1]);
        g.drawText (refmap::bandNames[k], (int) x0, (int) top.getY(), (int) (x1 - x0), 12, juce::Justification::centred);
    }

    const auto& mix = snap.mix;
    const auto& ref = snap.ref;
    if (! mix.valid && ! ref.valid)
    {
        g.setColour (colours::textDim);
        g.setFont (font (15.0f));
        g.drawText ("Drop a reference track here (or LOAD REFERENCE), then CAPTURE a loop of your mix",
                    getLocalBounds().reduced (20), juce::Justification::centred);
        return;
    }

    auto curve = [&] (const std::vector<float>& v, auto yMap, float offset = 0.0f, const std::vector<float>* add = nullptr, float addScale = 1.0f)
    {
        juce::Path p;
        for (int i = 0; i < refmap::gridPoints; ++i)
        {
            float val = v[(size_t) i] + offset;
            if (add) val += (*add)[(size_t) i] * addScale;
            const float x = xFor (refmap::gridFreq (i)), y = yMap (val);
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        return p;
    };

    if (ref.valid)
    {
        auto p = curve (ref.spec, yTop);
        juce::Path fill (p);
        fill.lineTo (b.getRight(), top.getBottom()); fill.lineTo (0, top.getBottom()); fill.closeSubPath();
        g.setColour (colours::ref.withAlpha (0.12f));
        g.fillPath (fill);
        g.setColour (colours::ref);
        g.strokePath (p, juce::PathStrokeType (2.0f));
    }
    if (mix.valid)
    {
        g.setColour (colours::mix);
        g.strokePath (curve (mix.spec, yTop), juce::PathStrokeType (2.0f));
    }

    const bool matchOn = proc.apvts.getRawParameterValue (ids::matchOn)->load() > 0.5f;
    const float amount = proc.apvts.getRawParameterValue (ids::amount)->load() * 0.01f;

    if (mix.valid && ref.valid)
    {
        // EQ response on the grid
        std::vector<float> eqResp ((size_t) refmap::gridPoints);
        for (int i = 0; i < refmap::gridPoints; ++i) eqResp[(size_t) i] = refmap::eqResponseDb (snap.gains, refmap::gridFreq (i));

        if (matchOn && amount > 0.0f)
        {
            juce::Path dashed;
            const float dashes[] = { 5.0f, 4.0f };
            juce::PathStrokeType (1.5f).createDashedStroke (dashed, curve (mix.spec, yTop, 0.0f, &eqResp, amount), dashes, 2);
            g.setColour (colours::text.withAlpha (0.8f));
            g.fillPath (dashed);
        }

        // difference: what the mix is missing (+) or has too much of (-)
        auto tp = curve (snap.target, yBot);
        juce::Path area (tp);
        area.lineTo (b.getRight(), bot.getCentreY()); area.lineTo (0, bot.getCentreY()); area.closeSubPath();
        g.saveState();
        g.reduceClipRegion (b.withBottom (bot.getCentreY()).toNearestInt());
        g.setColour (colours::good.withAlpha (0.22f)); g.fillPath (area);
        g.restoreState();
        g.saveState();
        g.reduceClipRegion (b.withTop (bot.getCentreY()).toNearestInt());
        g.setColour (colours::bad.withAlpha (0.22f)); g.fillPath (area);
        g.restoreState();
        g.setColour (colours::accent);
        g.strokePath (tp, juce::PathStrokeType (1.5f));

        g.setColour (matchOn ? colours::text : colours::textDim.withAlpha (0.6f));
        juce::Path eqPath;
        for (int i = 0; i < refmap::gridPoints; ++i)
        {
            const float x = xFor (refmap::gridFreq (i)), y = yBot (eqResp[(size_t) i] * (matchOn ? amount : 1.0f));
            if (i == 0) eqPath.startNewSubPath (x, y); else eqPath.lineTo (x, y);
        }
        g.strokePath (eqPath, juce::PathStrokeType (matchOn ? 2.0f : 1.0f));
    }

    g.setFont (font (10.5f));
    g.setColour (colours::textDim);
    g.drawText ("GREEN = boost to match ref    RED = cut to match ref    WHITE = match EQ", (int) b.getX() + 8, (int) bot.getY(), 420, 12, juce::Justification::left);
    g.drawText ("+12", (int) b.getRight() - 34, (int) bot.getY(), 28, 12, juce::Justification::right);
    g.drawText ("-12", (int) b.getRight() - 34, (int) bot.getBottom() - 12, 28, 12, juce::Justification::right);

    // legend
    auto leg = juce::Rectangle<int> ((int) b.getRight() - 210, (int) top.getY() + 16, 200, 14);
    g.setFont (font (11.0f, true));
    g.setColour (colours::ref); g.drawText ("REFERENCE", leg.removeFromLeft (80), juce::Justification::right);
    leg.removeFromLeft (14);
    g.setColour (colours::mix); g.drawText ("YOUR MIX", leg.removeFromLeft (70), juce::Justification::left);

    // hover readout
    if (hoverX >= 0 && mix.valid && ref.valid)
    {
        const double f = freqFor ((float) hoverX);
        const float r = refmap::interpGrid (ref.spec, f), m = refmap::interpGrid (mix.spec, f);
        g.setColour (colours::text.withAlpha (0.35f));
        g.drawVerticalLine (hoverX, b.getY(), b.getBottom());
        juce::String s = fmtFreq (f) + "   mix " + signedDb (m - r) + " dB vs ref";
        auto box = juce::Rectangle<int> (juce::jlimit (4, getWidth() - 214, hoverX + 8), (int) top.getY() + 34, 210, 20);
        g.setColour (colours::bg.withAlpha (0.9f)); g.fillRoundedRectangle (box.toFloat(), 4.0f);
        g.setColour (colours::text); g.setFont (font (12.0f));
        g.drawText (s, box, juce::Justification::centred);
    }
}

// ====================================================================== editor

RefmapEditor::RefmapEditor (RefmapProcessor& p) : AudioProcessorEditor (p), proc (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (spectrum);

    loadBtn.setTooltip ("Pick a reference track. Refmap finds its loudest section (the drop) matching your loop length.");
    loadBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a reference track", juce::File(),
                                                       proc.formats.getWildcardForAllFormats());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult().existsAsFile()) proc.loadReference (fc.getResult());
                              });
    };
    addAndMakeVisible (loadBtn);

    captureBtn.setColour (juce::TextButton::buttonColourId, colours::mix.withAlpha (0.18f));
    captureBtn.setColour (juce::TextButton::textColourOffId, colours::mix);
    captureBtn.setTooltip ("Arms the capture. When the transport plays, Refmap records the chosen number of bars from the next bar line.");
    captureBtn.onClick = [this]
    {
        const int s = proc.getCaptureState();
        if (s == RefmapProcessor::armed || s == RefmapProcessor::recording) proc.cancelCapture();
        else proc.armCapture();
    };
    addAndMakeVisible (captureBtn);

    clearBtn.setTooltip ("Forget the captured loop");
    clearBtn.onClick = [this] { proc.clearMix(); };
    addAndMakeVisible (clearBtn);

    barsBox.addItemList (proc.apvts.getParameter (ids::bars)->getAllValueStrings(), 1);
    barsAtt = std::make_unique<CA> (proc.apvts, ids::bars, barsBox);
    barsBox.setTooltip ("Loop length to capture, and the length of the reference section to compare against");
    addAndMakeVisible (barsBox);

    matchBtn.setClickingTogglesState (true);
    matchBtn.setColour (juce::TextButton::buttonOnColourId, colours::accent);
    matchBtn.setTooltip ("Applies an EQ that pulls your mix's tonal balance toward the reference");
    matchAtt = std::make_unique<BA> (proc.apvts, ids::matchOn, matchBtn);
    addAndMakeVisible (matchBtn);

    auto setListen = [this] (int v)
    {
        auto* prm = proc.apvts.getParameter (ids::listen);
        prm->beginChangeGesture(); prm->setValueNotifyingHost (prm->convertTo0to1 ((float) v)); prm->endChangeGesture();
    };
    abMix.onClick = [setListen] { setListen (0); };
    abRef.onClick = [setListen] { setListen (1); };
    abMix.setColour (juce::TextButton::buttonOnColourId, colours::mix);
    abRef.setColour (juce::TextButton::buttonOnColourId, colours::ref);
    abRef.setTooltip ("Plays the reference section instead of your mix, level-matched to your loop's loudness");
    addAndMakeVisible (abMix);
    addAndMakeVisible (abRef);

    setupKnob (amount, ids::amount, "AMOUNT", colours::accent);
    setupKnob (output, ids::output, "OUTPUT", colours::text);

    setSize (W, H);
    startTimerHz (20);
    timerCallback();
}

RefmapEditor::~RefmapEditor() { setLookAndFeel (nullptr); }

void RefmapEditor::setupKnob (Knob& k, const juce::String& id, const juce::String& name, juce::Colour c)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
    k.slider.setColour (juce::Slider::rotarySliderFillColourId, c);
    k.slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    k.att = std::make_unique<SA> (proc.apvts, id, k.slider);
    k.label.setText (name, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.setFont (font (11.0f, true));
    k.label.setColour (juce::Label::textColourId, colours::textDim);
    addAndMakeVisible (k.slider);
    addAndMakeVisible (k.label);
}

void RefmapEditor::timerCallback()
{
    if (proc.getVersion() != snap.version)
    {
        proc.copySnapshot (snap);
        spectrum.repaint();
        repaint();
    }
    const bool listenRef = proc.apvts.getRawParameterValue (ids::listen)->load() > 0.5f;
    abMix.setToggleState (! listenRef, juce::dontSendNotification);
    abRef.setToggleState (listenRef, juce::dontSendNotification);
    updateCaptureButton();
    spectrum.repaint();   // cheap; keeps the match-EQ preview in sync with the knobs
}

void RefmapEditor::updateCaptureButton()
{
    const int s = proc.getCaptureState();
    const int bars = s == RefmapProcessor::idle ? (int) barsBox.getText().getIntValue() : proc.getCaptureBars();
    juce::String t;
    switch (s)
    {
        case RefmapProcessor::armed:     t = "ARMED - PRESS PLAY"; break;
        case RefmapProcessor::recording: t = "CAPTURING " + juce::String (juce::jmin (bars, 1 + (int) (proc.getCaptureProgress() * bars))) + " / " + juce::String (bars); break;
        case RefmapProcessor::done:
        case RefmapProcessor::analysing: t = "ANALYSING..."; break;
        default:                         t = "CAPTURE " + juce::String (bars) + " BARS"; break;
    }
    if (captureBtn.getButtonText() != t) captureBtn.setButtonText (t);
    captureBtn.setToggleState (s == RefmapProcessor::recording, juce::dontSendNotification);
    if (s != lastState) { lastState = s; repaint (headerArea); }
    if (s == RefmapProcessor::recording) repaint (headerArea);
}

void RefmapEditor::resized()
{
    auto r = getLocalBounds().reduced (16, 0);
    headerArea = r.removeFromTop (62);
    {
        auto h = headerArea.reduced (0, 14);
        h.removeFromLeft (128);
        loadBtn.setBounds (h.removeFromLeft (150));
        clearBtn.setBounds (h.removeFromRight (70));
        h.removeFromRight (8);
        captureBtn.setBounds (h.removeFromRight (200));
        h.removeFromRight (8);
        barsBox.setBounds (h.removeFromRight (96).reduced (0, 2));
    }
    statusArea = r.removeFromTop (24);
    r.removeFromTop (6);
    auto mid = r.removeFromTop (300);
    statsArea = mid.removeFromRight (290);
    mid.removeFromRight (14);
    spectrum.setBounds (mid);
    r.removeFromTop (12);
    mapArea = r.removeFromTop (214);
    r.removeFromTop (8);
    controlsArea = r.removeFromTop (r.getHeight() - 8);

    auto c = controlsArea;
    matchBtn.setBounds (c.removeFromLeft (120).withSizeKeepingCentre (120, 34));
    c.removeFromLeft (10);
    auto k = c.removeFromLeft (80);
    amount.label.setBounds (k.removeFromTop (14));
    amount.slider.setBounds (k);

    auto ko = c.removeFromRight (80);
    output.label.setBounds (ko.removeFromTop (14));
    output.slider.setBounds (ko);
    c.removeFromRight (20);
    auto ab = c.removeFromRight (240).withSizeKeepingCentre (240, 34);
    abMix.setBounds (ab.removeFromLeft (90));
    ab.removeFromLeft (6);
    abRef.setBounds (ab);
}

void RefmapEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::bg);

    // header
    g.setColour (colours::text);
    g.setFont (font (24.0f, true));
    g.drawText ("REFMAP", headerArea.withWidth (124), juce::Justification::centredLeft);
    {
        auto info = headerArea.withTrimmedLeft (128 + 150 + 12).withTrimmedRight (70 + 8 + 200 + 8 + 96 + 12);
        g.setFont (font (13.0f, true));
        if (snap.ref.valid)
        {
            g.setColour (colours::ref);
            g.drawText (snap.refName, info.removeFromTop (headerArea.getHeight() / 2 + 2).withTrimmedTop (12), juce::Justification::bottomLeft, true);
            g.setFont (font (11.5f));
            g.setColour (colours::textDim);
            g.drawText ("drop @ " + fmtTime (snap.refStartSec) + " - " + fmtTime (snap.refStartSec + snap.refLenSec)
                        + "   (" + juce::String (proc.getHostBpm(), 1) + " BPM)", info, juce::Justification::topLeft, true);
        }
        else
        {
            g.setColour (colours::textDim);
            g.drawText ("No reference loaded", info, juce::Justification::centredLeft, true);
        }
    }
    if (proc.getCaptureState() == RefmapProcessor::recording)
    {
        auto bar = captureBtn.getBounds().translated (0, 38).withHeight (3);
        g.setColour (colours::outline); g.fillRect (bar);
        g.setColour (colours::mix); g.fillRect (bar.withWidth ((int) (bar.getWidth() * proc.getCaptureProgress())));
    }

    // status
    g.setFont (font (12.5f));
    g.setColour (colours::textDim);
    g.drawText (snap.status, statusArea, juce::Justification::centredLeft, true);

    paintStats (g, statsArea);
    paintMap (g, mapArea);

    if (dragOver)
    {
        g.setColour (colours::ref.withAlpha (0.15f));
        g.fillAll();
        g.setColour (colours::ref);
        g.drawRect (getLocalBounds(), 3);
        g.setFont (font (22.0f, true));
        g.drawText ("Drop to load reference", getLocalBounds(), juce::Justification::centred);
    }
}

void RefmapEditor::paintStats (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (colours::panel);
    g.fillRoundedRectangle (area.toFloat(), 8.0f);
    auto r = area.reduced (14, 10);

    g.setFont (font (11.0f, true));
    auto hdr = r.removeFromTop (18);
    g.setColour (colours::textDim); g.drawText ("OVERALL", hdr.removeFromLeft (110), juce::Justification::left);
    g.setColour (colours::mix);     g.drawText ("MIX", hdr.removeFromLeft (52), juce::Justification::right);
    g.setColour (colours::ref);     g.drawText ("REF", hdr.removeFromLeft (52), juce::Justification::right);
    g.setColour (colours::textDim); g.drawText ("DIFF", hdr, juce::Justification::right);
    r.removeFromTop (4);

    const auto& m = snap.mix;
    const auto& f = snap.ref;
    struct Row { const char* name; float mv, rv; int dec; float warn; };
    const Row rows[] = {
        { "Loudness LUFS",   m.lufs,     f.lufs,     1, 1.0f },
        { "Max short-term",  m.shortMax, f.shortMax, 1, 1.0f },
        { "Peak dBFS",       m.peakDb,   f.peakDb,   1, 1.0f },
        { "Peak / loudness", m.plr,      f.plr,      1, 1.5f },
        { "Stereo width %",  m.width,    f.width,    0, 5.0f },
        { "Correlation",     m.corr,     f.corr,     2, 0.15f },
    };
    for (auto& row : rows)
    {
        auto line = r.removeFromTop (22);
        g.setFont (font (12.5f));
        g.setColour (colours::text);
        g.drawText (row.name, line.removeFromLeft (110), juce::Justification::left);
        g.setColour (colours::mix);
        g.drawText (m.valid ? num (row.mv, row.dec) : "-", line.removeFromLeft (52), juce::Justification::right);
        g.setColour (colours::ref);
        g.drawText (f.valid ? num (row.rv, row.dec) : "-", line.removeFromLeft (52), juce::Justification::right);
        if (m.valid && f.valid)
        {
            const float d = row.mv - row.rv;
            g.setColour (std::abs (d) < row.warn ? colours::good : colours::bad.interpolatedWith (colours::ref, 0.3f));
            g.setFont (font (12.5f, true));
            g.drawText (signedDb (d, row.dec), line, juce::Justification::right);
        }
    }

    // plain-language summary
    r.removeFromTop (8);
    g.setColour (colours::outline);
    g.drawHorizontalLine (r.getY(), (float) r.getX(), (float) r.getRight());
    r.removeFromTop (8);
    juce::StringArray notes;
    if (m.valid && f.valid)
    {
        const float dl = m.lufs - f.lufs, dp = m.plr - f.plr, dw = m.width - f.width;
        if (dl < -1.0f)      notes.add ("Mix is " + juce::String (-dl, 1) + " LU quieter than the ref (A/B is level-matched).");
        else if (dl > 1.0f)  notes.add ("Mix is " + juce::String (dl, 1) + " LU louder than the ref.");
        if (dp > 1.5f)       notes.add ("More dynamic than the ref (the ref is limited harder).");
        else if (dp < -1.5f) notes.add ("Squashed vs the ref: ease off the limiter.");
        if (dw < -5.0f)      notes.add ("Narrower than the ref overall.");
        else if (dw > 5.0f)  notes.add ("Wider than the ref overall.");
        if (m.corr < 0.2f)   notes.add ("Low correlation: check mono compatibility.");

        // biggest band gaps
        std::vector<std::pair<float, int>> gaps;
        for (int k = 0; k < refmap::numBands; ++k) gaps.push_back ({ m.bandDb[(size_t) k] - f.bandDb[(size_t) k], k });
        std::sort (gaps.begin(), gaps.end(), [] (auto& a, auto& b) { return std::abs (a.first) > std::abs (b.first); });
        juce::String big = "Biggest gaps: ";
        for (int i = 0; i < 2; ++i)
            big << refmap::bandNames[gaps[(size_t) i].second] << " " << signedDb (gaps[(size_t) i].first) << " dB" << (i == 0 ? ",  " : "");
        notes.insert (0, big);
    }
    else
        notes.add (f.valid ? "Capture a loop of your mix to compare." : "Load a reference to compare.");

    g.setFont (font (12.0f));
    g.setColour (colours::text);
    for (auto& n : notes)
    {
        auto h = juce::jmin (r.getHeight(), 30);
        g.drawFittedText (n, r.removeFromTop (h), juce::Justification::topLeft, 2, 1.0f);
        if (r.getHeight() <= 0) break;
    }
}

void RefmapEditor::paintMap (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (colours::panel);
    g.fillRoundedRectangle (area.toFloat(), 8.0f);
    auto r = area.reduced (14, 8);

    const int cName = 90, cBal = 300, cWid = 200;
    auto hdr = r.removeFromTop (20);
    g.setFont (font (11.0f, true));
    g.setColour (colours::textDim);
    g.drawText ("BAND", hdr.removeFromLeft (cName), juce::Justification::left);
    g.drawText ("BALANCE vs REF   (-6 ... +6 dB)", hdr.removeFromLeft (cBal), juce::Justification::centred);
    hdr.removeFromLeft (20);
    g.drawText ("WIDTH  mix / ref", hdr.removeFromLeft (cWid), juce::Justification::left);
    g.drawText ("VERDICT", hdr, juce::Justification::left);

    const auto& m = snap.mix;
    const auto& f = snap.ref;
    const bool both = m.valid && f.valid;
    const int rowH = r.getHeight() / refmap::numBands;

    for (int k = 0; k < refmap::numBands; ++k)
    {
        auto line = r.removeFromTop (rowH);
        g.setColour (colours::outline.withAlpha (0.5f));
        g.drawHorizontalLine (line.getY(), (float) line.getX(), (float) line.getRight());

        auto nameR = line.removeFromLeft (cName);
        g.setFont (font (12.5f, true));
        g.setColour (colours::text);
        g.drawText (refmap::bandNames[k], nameR, juce::Justification::centredLeft);
        g.setFont (font (9.5f));
        g.setColour (colours::textDim);
        static const char* ranges[] = { "20-60 Hz", "60-150 Hz", "150-500 Hz", "0.5-2 kHz", "2-6 kHz", "6-20 kHz" };
        g.drawText (ranges[k],
                    nameR.withTrimmedTop (rowH / 2 + 4), juce::Justification::topLeft);

        // balance bar
        auto bal = line.removeFromLeft (cBal).reduced (6, rowH / 2 - 5);
        g.setColour (colours::outline);
        g.fillRoundedRectangle (bal.toFloat(), 3.0f);
        const float cx = (float) bal.getCentreX();
        g.setColour (colours::textDim);
        g.drawVerticalLine ((int) cx, (float) bal.getY() - 3, (float) bal.getBottom() + 3);
        if (both)
        {
            const float d = m.bandDb[(size_t) k] - f.bandDb[(size_t) k];
            const float w = juce::jlimit (-6.0f, 6.0f, d) / 6.0f * bal.getWidth() * 0.5f;
            auto fill = juce::Rectangle<float> (std::min (cx, cx + w), (float) bal.getY(), std::abs (w), (float) bal.getHeight());
            g.setColour (severity (std::abs (d)));
            g.fillRoundedRectangle (fill, 3.0f);
            g.setFont (font (11.5f, true));
            g.drawText (signedDb (d) + " dB", bal.translated (0, 0).withWidth (bal.getWidth() / 2 - 8).translated (d < 0 ? bal.getWidth() / 2 + 8 : 0, 0),
                        d < 0 ? juce::Justification::centredLeft : juce::Justification::centredRight);
        }
        line.removeFromLeft (20);

        // width bars
        auto wid = line.removeFromLeft (cWid).reduced (0, rowH / 2 - 7).withWidth (150);
        auto wm = wid.removeFromTop (wid.getHeight() / 2 - 1);
        wid.removeFromTop (2);
        auto drawW = [&g] (juce::Rectangle<int> rr, float pct, juce::Colour c)
        {
            g.setColour (colours::outline); g.fillRect (rr);
            g.setColour (c); g.fillRect (rr.withWidth ((int) (rr.getWidth() * juce::jlimit (0.0f, 1.0f, pct / 60.0f))));
        };
        if (m.valid) drawW (wm, m.bandWidth[(size_t) k], colours::mix);
        if (f.valid) drawW (wid, f.bandWidth[(size_t) k], colours::ref);

        // verdict
        g.setFont (font (12.5f));
        if (both)
        {
            const float d = m.bandDb[(size_t) k] - f.bandDb[(size_t) k];
            g.setColour (severity (std::abs (d)));
            g.drawText (refmap::bandAdvice (k, d, m.bandWidth[(size_t) k], f.bandWidth[(size_t) k]), line, juce::Justification::centredLeft, true);
        }
        else
        {
            g.setColour (colours::textDim);
            g.drawText ("-", line, juce::Justification::centredLeft);
        }
    }
}

bool RefmapEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (proc.formats.findFormatForFileExtension (juce::File (f).getFileExtension()) != nullptr) return true;
    return false;
}

void RefmapEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    for (auto& f : files)
        if (proc.formats.findFormatForFileExtension (juce::File (f).getFileExtension()) != nullptr)
        {
            proc.loadReference (juce::File (f));
            break;
        }
    repaint();
}
