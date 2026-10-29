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
    /// Opt-in measurement tap on the engine's master output, for tests (not in the original
    /// module). Enabled by setting KICK_AUDIO_PROBE=<file.wav> in the environment (relative to the
    /// working directory): every block AudIO renders is written to that WAV (32-bit float, one
    /// channel per output), and once per second of audio a line is logged:
    ///
    ///     AudioProbe: t=12s peak=-6.1dBFS rms=-21.4dBFS
    ///
    /// so a script can assert that sound happened without listening. The audio thread only
    /// copies into a lock-free FIFO; a background thread does the file I/O and the logging.
    /// Unset (as on a unit), no probe is created and the audio callback is unchanged.
    class AudioProbe : private juce::Thread
    {
    public:
        static std::unique_ptr<AudioProbe> createFromEnvironment( double SampleRate, int NumChannels );

        AudioProbe( const juce::File& File, double SampleRate, int NumChannels );
        ~AudioProbe() override;

        /// Audio thread. Never blocks; drops audio if the writer falls behind (logged).
        void push( const float* const* Channels, int NumChannels, int NumSamples ) noexcept;

    private:
        void run() override;
        void drain();
        void logSecond();

        juce::File                                 file;
        const double                               sampleRate;
        const int                                  numChannels;
        std::unique_ptr<juce::AudioFormatWriter>   writer;
        juce::AbstractFifo                         fifo;
        juce::AudioBuffer<float>                   ring;
        juce::AudioBuffer<float>                   scratch;
        std::atomic<int>                           dropped{ 0 };

        // Per-second statistics (background thread only).
        int64_t framesInSecond{ 0 };
        int64_t secondsLogged{ 0 };
        float   peak{ 0.f };
        double  sumSquares{ 0.0 };

        JUCE_DECLARE_NON_COPYABLE( AudioProbe )
    };

    /// Opt-in replacement for the device input, for tests. Enabled by setting KICK_AUDIO_INPUT
    /// to one of:
    ///
    ///     silence       digital silence (no microphone, no feedback through the speakers)
    ///     device        the real device input, no replacement (the same as unset)
    ///     tone          1 kHz sine at -12 dBFS, continuous
    ///     tone:<hz>     sine at that frequency, -12 dBFS
    ///     pulse         1 kHz bursts of 50 ms every 500 ms (120 BPM quarter notes), -6 dBFS
    ///     pulse:<ms>    the same bursts every <ms> milliseconds (e.g. pulse:700, a period that
    ///                   is not a divisor of the bar, so a loop's repeat is visible in the output)
    ///     <file>        a WAV/AIFF/FLAC file, looped, resampled to the engine rate
    ///
    /// The source replaces whatever the audio device delivers on every input channel, so the
    /// sampler and looper can be tested without a microphone or permission. Unset (as on a
    /// unit), the device input is used and nothing is created.
    class TestInput
    {
    public:
        static std::unique_ptr<TestInput> createFromEnvironment( double SampleRate );

        /// Audio thread: fill NumChannels x NumSamples.
        void render( float* const* Channels, int NumChannels, int NumSamples ) noexcept;

    private:
        enum class Kind { silence, tone, pulse, file };
        TestInput() = default;

        Kind                     kind{ Kind::tone };
        double                   sampleRate{ 48000.0 };
        double                   frequency{ 1000.0 };
        double                   pulsePeriod{ 0.5 }; // seconds
        double                   phase{ 0.0 };
        int64_t                  position{ 0 };
        juce::AudioBuffer<float> data; // file contents at sampleRate
    };
} // namespace mt
