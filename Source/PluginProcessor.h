#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "WindowsVolumeFollower.h"

class WinVolumeFollowProcessor : public juce::AudioProcessor
{
public:
    WinVolumeFollowProcessor();
    ~WinVolumeFollowProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Windows Volume Follow"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    WindowsVolumeFollower follower;
    juce::AudioParameterBool* followParam = nullptr;

    // Empty = system default output.
    juce::String getDeviceId() const { return deviceId; }
    void setDeviceId (const juce::String& id)
    {
        deviceId = id;
        follower.setTargetDevice (id.toWideCharPointer());
    }

private:
    juce::String deviceId;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothed { 1.0f };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WinVolumeFollowProcessor)
};
