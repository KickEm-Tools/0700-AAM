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
    /// Order matches AudIO::Filter<T>::Type and the stored `type` value of a filter envelope.
    enum FilterType
    {
        LowPass = 0,
        HighPass,
        BandPass
    };

    /// Stereo state-variable filter (TPT / Zavalishin topology, 12 dB/oct) processed one
    /// stereo sample at a time.
    ///
    /// Reconstructed from how Kick's synth voices use it (see drum_voice.cpp):
    ///   filter.setSampleRate( sr )
    ///   filter.setFilterType( toEnum<FilterType>( type ) )
    ///   filter.setResonance( r )                            r is 0..127 as stored by the UI (0..1 also accepted)
    ///   filter.setCutoff( Filter::freqRange.convertTo0to1( hz ) + modulation )
    ///   filter.process( stereoOneSampleBuffer )             in place
    class Filter
    {
    public:
        /// Hz range of the cutoff parameter stored by the UI. Skewed so the 0..1 position feels logarithmic.
        inline static const juce::NormalisableRange<float> freqRange{ 20.f, 20000.f, 0.f, 0.25f };

        void setSampleRate( double SampleRate ) noexcept
        {
            sampleRate = SampleRate > 0.0 ? SampleRate : 44100.0;
            dirty      = true;
        }

        void setFilterType( FilterType T ) noexcept { type = T; }
        FilterType getFilterType() const noexcept { return type; }

        void setResonance( float Resonance ) noexcept
        {
            auto r = Resonance > 1.f ? Resonance / 127.f : Resonance;
            r      = juce::jlimit( 0.f, 1.f, r );
            if( r != resonance )
            {
                resonance = r;
                dirty     = true;
            }
        }

        /// 0..1 position in freqRange. Values outside are clamped.
        void setCutoff( float Normalised ) noexcept
        {
            auto c = juce::jlimit( 0.f, 1.f, Normalised );
            if( c != cutoff )
            {
                cutoff = c;
                dirty  = true;
            }
        }
        float getCutoff() const noexcept { return cutoff; }

        void reset() noexcept
        {
            for( auto& s : state )
                s = {};
        }

        /// Filters every channel of `Buffer` in place (at most 2 channels).
        void process( juce::AudioBuffer<float>& Buffer ) noexcept
        {
            if( dirty ) recalculate();

            const auto numCh = juce::jmin( 2, Buffer.getNumChannels() );
            const auto n     = Buffer.getNumSamples();

            for( int ch = 0; ch < numCh; ++ch )
            {
                auto* d = Buffer.getWritePointer( ch );
                auto& s = state[(size_t)ch];
                for( int i = 0; i < n; ++i )
                    d[i] = tick( s, d[i] );
            }
        }

        float processSample( int Channel, float In ) noexcept
        {
            if( dirty ) recalculate();
            return tick( state[(size_t)juce::jlimit( 0, 1, Channel )], In );
        }

    private:
        struct State
        {
            float ic1eq{ 0.f }, ic2eq{ 0.f };
        };

        void recalculate() noexcept
        {
            const auto hz = freqRange.convertFrom0to1( cutoff );
            const auto fc = juce::jlimit( 10.0, 0.45 * sampleRate, (double)hz );
            g             = (float)std::tan( juce::MathConstants<double>::pi * fc / sampleRate );
            k             = 2.f - 1.9f * resonance; // damping: 2 (no resonance) .. 0.1 (strong resonance)
            a1            = 1.f / ( 1.f + g * ( g + k ) );
            dirty         = false;
        }

        inline float tick( State& s, float in ) noexcept
        {
            const auto v1 = a1 * ( s.ic1eq + g * ( in - s.ic2eq ) );
            const auto v2 = s.ic2eq + g * v1;
            s.ic1eq       = 2.f * v1 - s.ic1eq;
            s.ic2eq       = 2.f * v2 - s.ic2eq;

            switch( type )
            {
                case HighPass: return in - k * v1 - v2;
                case BandPass: return v1;
                case LowPass:
                default: return v2;
            }
        }

        double     sampleRate{ 44100.0 };
        float      cutoff{ 1.f }, resonance{ 0.f };
        float      g{ 1.f }, k{ 2.f }, a1{ 0.25f };
        bool       dirty{ true };
        FilterType type{ LowPass };
        State      state[2];
    };
} // namespace mt
