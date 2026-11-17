#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "RefmapLookAndFeel.h"

// Reference vs mix spectrum (top) and the difference / match-EQ curve (bottom)
class SpectrumView : public juce::Component
{
public:
    SpectrumView (const Snapshot& s, RefmapProcessor& p) : snap (s), proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent& e) override { hoverX = e.x; repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { hoverX = -1; repaint(); }

private:
    float xFor (double f) const;
    double freqFor (float x) const;
    const Snapshot& snap;
    RefmapProcessor& proc;
    int hoverX = -1;
};

class RefmapEditor : public juce::AudioProcessorEditor,
                     public juce::FileDragAndDropTarget,
                     private juce::Timer
{
public:
    explicit RefmapEditor (RefmapProcessor&);
    ~RefmapEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override            { dragOver = false; repaint(); }

private:
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    struct Knob { juce::Slider slider; juce::Label label; std::unique_ptr<SA> att; };

    void timerCallback() override;
    void setupKnob (Knob&, const juce::String& id, const juce::String& name, juce::Colour);
    void updateCaptureButton();
    void paintStats (juce::Graphics&, juce::Rectangle<int>);
    void paintMap (juce::Graphics&, juce::Rectangle<int>);

    RefmapProcessor& proc;
    RefmapLookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };
    Snapshot snap;
    SpectrumView spectrum { snap, proc };
    std::unique_ptr<juce::FileChooser> chooser;
    bool dragOver = false;

    juce::TextButton loadBtn { "LOAD REFERENCE" }, captureBtn { "CAPTURE" }, clearBtn { "CLEAR" }, matchBtn { "MATCH EQ" };
    juce::TextButton abMix { "MIX" }, abRef { "REFERENCE" };
    std::unique_ptr<BA> matchAtt;
    juce::ComboBox barsBox;
    std::unique_ptr<CA> barsAtt;
    Knob amount, output;

    juce::Rectangle<int> headerArea, statusArea, statsArea, mapArea, controlsArea;
    int lastState = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RefmapEditor)
};
