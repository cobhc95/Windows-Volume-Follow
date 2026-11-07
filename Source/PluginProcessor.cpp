#include "PluginProcessor.h"

namespace
{
    constexpr float kFloorGain = 1.0e-5f;   // multiplicative smoothing can't reach 0; ~-100 dB, then snapped to silence

    class StatusEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit StatusEditor (WinVolumeFollowProcessor& p) : AudioProcessorEditor (p), proc (p)
        {
            setSize (360, 170);
            enable.setButtonText ("Follow Windows volume");
            attach = std::make_unique<juce::ButtonParameterAttachment> (*proc.followParam, enable);
            addAndMakeVisible (enable);
            addAndMakeVisible (deviceBox);
            refreshDevices();
            deviceBox.onChange = [this]
            {
                const int idx = deviceBox.getSelectedId() - 2;   // id 1 = system default
                if (refreshing) return;
                proc.setDeviceId (idx >= 0 && idx < ids.size() ? ids[idx] : juce::String());
            };
            startTimerHz (10);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            enable.setBounds (r.removeFromTop (34).reduced (12, 6));
            deviceBox.setBounds (r.removeFromTop (30).reduced (12, 3));
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff1e2227));
            g.setColour (juce::Colours::white);
            g.setFont (15.0f);
            g.drawText (text1, 12, 80, getWidth() - 24, 22, juce::Justification::left);
            g.setColour (juce::Colours::lightgrey);
            g.setFont (13.0f);
            g.drawText (text2, 12, 104, getWidth() - 24, 20, juce::Justification::left);
            g.drawText (text3, 12, 128, getWidth() - 24, 20, juce::Justification::left);
            g.setColour (juce::Colours::grey);
            g.setFont (11.0f);
            g.drawText (juce::String ("Windows Volume Follow  v") + JucePlugin_VersionString + " alpha",
                        12, 148, getWidth() - 24, 16, juce::Justification::right);
        }

    private:
        void refreshDevices()
        {
            const auto list = WindowsVolumeFollower::listRenderDevices();
            juce::StringArray newIds, names;
            for (auto& d : list) { newIds.add (juce::String (d.first.c_str())); names.add (juce::String (d.second.c_str())); }

            const juce::String current = proc.getDeviceId();
            if (! current.isEmpty() && ! newIds.contains (current))   // saved device not currently present: keep it listed
            { newIds.add (current); names.add ("(unavailable device)"); }

            if (newIds == ids && deviceBox.getNumItems() > 0) return;
            refreshing = true;
            ids = newIds;
            deviceBox.clear (juce::dontSendNotification);
            deviceBox.addItem ("System default output", 1);
            for (int i = 0; i < names.size(); ++i) deviceBox.addItem (names[i], i + 2);
            const int sel = current.isEmpty() ? 1 : ids.indexOf (current) + 2;
            deviceBox.setSelectedId (sel, juce::dontSendNotification);
            refreshing = false;
        }

        void timerCallback() override
        {
            if (++tick % 30 == 0 && ! deviceBox.isPopupActive()) refreshDevices();   // pick up plugged/unplugged devices
            auto& f = proc.follower;
            text1 = f.isConnected() ? juce::String (f.getDeviceName().c_str()) : "No output device";
            text2 = f.isMuted() ? "Windows volume: MUTED"
                                : "Windows volume: " + juce::String (f.getDb(), 1) + " dB";
            text3 = proc.followParam->get() ? "Attenuating output to match" : "Bypassed (unity gain)";
            repaint();
        }

        WinVolumeFollowProcessor& proc;
        juce::ToggleButton enable;
        juce::ComboBox deviceBox;
        juce::StringArray ids;
        bool refreshing = false;
        int tick = 0;
        std::unique_ptr<juce::ButtonParameterAttachment> attach;
        juce::String text1, text2, text3;
    };
}

WinVolumeFollowProcessor::WinVolumeFollowProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    addParameter (followParam = new juce::AudioParameterBool ({ "follow", 1 }, "Follow Windows Volume", true));
}

bool WinVolumeFollowProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainInputChannelSet() == l.getMainOutputChannelSet()
        && ! l.getMainOutputChannelSet().isDisabled();
}

void WinVolumeFollowProcessor::prepareToPlay (double sampleRate, int)
{
    smoothed.reset (sampleRate, 0.03);
    smoothed.setCurrentAndTargetValue ((std::max) (kFloorGain, followParam->get() ? follower.getGain() : 1.0f));
}

void WinVolumeFollowProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const float target = followParam->get() ? follower.getGain() : 1.0f;
    smoothed.setTargetValue ((std::max) (kFloorGain, target));

    const int n = buffer.getNumSamples();
    const int ch = buffer.getNumChannels();

    if (! smoothed.isSmoothing())
    {
        const float g = (target <= 0.0f) ? 0.0f : smoothed.getTargetValue();
        if (g != 1.0f)
            buffer.applyGain (g);
        return;
    }

    for (int i = 0; i < n; ++i)
    {
        float g = smoothed.getNextValue();
        if (target <= 0.0f && g < 1.0e-4f) g = 0.0f;
        for (int c = 0; c < ch; ++c)
            buffer.getWritePointer (c)[i] *= g;
    }
}

juce::AudioProcessorEditor* WinVolumeFollowProcessor::createEditor() { return new StatusEditor (*this); }

void WinVolumeFollowProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::MemoryOutputStream s (dest, true);
    s.writeBool (followParam->get());
    s.writeString (deviceId);
}

void WinVolumeFollowProcessor::setStateInformation (const void* data, int size)
{
    juce::MemoryInputStream s (data, (size_t) size, false);
    if (size > 0) *followParam = s.readBool();
    setDeviceId (s.isExhausted() ? juce::String() : s.readString());   // older saves have no device id
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new WinVolumeFollowProcessor(); }
