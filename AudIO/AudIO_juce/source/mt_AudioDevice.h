/*
 * Copyright (c) 2022 MUSIC Tribe
 *
 * This file is part of AudIO.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

namespace mt
{
    /// AudIO audio device backed by juce::AudioDeviceManager.
    ///
    /// AudIO processes fixed-size blocks (the memory-pool block, 480 samples). The host
    /// audio device may deliver a different callback size, so this class re-blocks through
    /// small input/output FIFOs. When the sizes match there is no extra latency.
    ///
    /// Must be created, started and stopped on the JUCE message thread, which is what
    /// Kick's MTAudioManager does.
    class AudioDevice : public AudIO::IAudioDevice,
                        private juce::AudioIODeviceCallback
    {
    public:
        AudioDevice( uint32_t NumInputs, uint32_t NumOutputs, const juce::String& PreferredDeviceName = {} );
        ~AudioDevice() override;

        juce::AudioDeviceManager& getJuceAudioDeviceMgr() noexcept { return deviceManager; }

        // AudIO::IAudioDevice
        void start( uint32_t samplerate, uint32_t blocksize ) override;
        void resume() override;
        void pause() override;
        void stop() override;

    private:
        // juce::AudioIODeviceCallback
        void audioDeviceIOCallbackWithContext( const float* const* inputChannelData, int numInputChannels, float* const* outputChannelData, int numOutputChannels, int numSamples, const juce::AudioIODeviceCallbackContext& context ) override;
        void audioDeviceAboutToStart( juce::AudioIODevice* device ) override;
        void audioDeviceStopped() override;
        void audioDeviceError( const juce::String& errorMessage ) override;

        void allocateFifos( int DeviceBufferSize );

        juce::AudioDeviceManager deviceManager;
        juce::String             preferredName;
        const uint32_t           numIns;
        const uint32_t           numOuts;

        uint32_t          chunk{ 0 }; // AudIO block size we process in.
        std::atomic<bool> running{ false };
        bool              callbackAdded{ false };

        // Re-blocking FIFOs (one ring per channel).
        juce::AudioBuffer<float> inRing, outRing;

        // Opt-in test tooling (KICK_AUDIO_PROBE, KICK_AUDIO_INPUT), see mt_AudioDeviceProbe.h.
        // Both stay null unless the variable is set.
        std::unique_ptr<AudioProbe> probe;
        std::unique_ptr<TestInput>  testInput;
        int                      ringSize{ 0 };
        int                      inWrite{ 0 }, inAvail{ 0 };
        int                      outWrite{ 0 }, outRead{ 0 }, outAvail{ 0 };

        JUCE_DECLARE_NON_COPYABLE( AudioDevice )
    };
} // namespace mt
