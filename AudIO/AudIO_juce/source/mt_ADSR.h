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
    /// Linear AHDSR envelope, one value per call.
    ///
    /// Reconstructed from how Kick's synth voices use it (see drum_voice.cpp):
    ///   setSampleRate( sr )                         once, in prepareToPlay
    ///   setParameters( a, h, d, r, s )              per block; a/h/d/r are lengths IN SAMPLES, s is a 0..1 level
    ///   gate( 1 ) / gate( 0 )                       note on / note off
    ///   float v = env();                            next sample, 0..1
    ///
    /// Parameters can change while the envelope runs; they take effect at the next segment boundary.
    class ADSR
    {
    public:
        enum class Stage
        {
            Idle,
            Attack,
            Hold,
            Decay,
            Sustain,
            Release
        };

        void setSampleRate( double SampleRate ) noexcept { sampleRate = SampleRate > 0.0 ? SampleRate : 44100.0; }
        double getSampleRate() const noexcept { return sampleRate; }

        void setParameters( float AttackSamples, float HoldSamples, float DecaySamples, float ReleaseSamples, float SustainLevel ) noexcept
        {
            attack  = AttackSamples > 0.f ? AttackSamples : 0.f;
            hold    = HoldSamples > 0.f ? HoldSamples : 0.f;
            decay   = DecaySamples > 0.f ? DecaySamples : 0.f;
            release = ReleaseSamples > 0.f ? ReleaseSamples : 0.f;
            // The UI stores sustain as 0..127 while the model defaults it to 1.0; accept both.
            auto s  = SustainLevel > 1.f ? SustainLevel / 127.f : SustainLevel;
            sustain = s < 0.f ? 0.f : ( s > 1.f ? 1.f : s );
        }

        void gate( int On ) noexcept
        {
            if( On )
            {
                stage      = Stage::Attack;
                startLevel = level;
                pos        = 0.f;
            }
            else if( stage != Stage::Idle && stage != Stage::Release )
            {
                stage      = Stage::Release;
                startLevel = level;
                pos        = 0.f;
            }
        }

        void reset() noexcept
        {
            stage = Stage::Idle;
            level = 0.f;
            pos   = 0.f;
        }

        bool  isActive() const noexcept { return stage != Stage::Idle; }
        Stage getStage() const noexcept { return stage; }
        float getLevel() const noexcept { return level; }

        float operator()() noexcept
        {
            for( ;; )
            {
                switch( stage )
                {
                    case Stage::Idle:
                        return 0.f;

                    case Stage::Attack:
                        if( pos >= attack )
                        {
                            level = 1.f;
                            stage = Stage::Hold;
                            pos   = 0.f;
                            continue;
                        }
                        level = startLevel + ( 1.f - startLevel ) * ( pos / attack );
                        pos += 1.f;
                        return level;

                    case Stage::Hold:
                        if( pos >= hold )
                        {
                            stage = Stage::Decay;
                            pos   = 0.f;
                            continue;
                        }
                        level = 1.f;
                        pos += 1.f;
                        return level;

                    case Stage::Decay:
                        if( pos >= decay )
                        {
                            level = sustain;
                            stage = Stage::Sustain;
                            continue;
                        }
                        level = 1.f + ( sustain - 1.f ) * ( pos / decay );
                        pos += 1.f;
                        return level;

                    case Stage::Sustain:
                        level = sustain;
                        return level;

                    case Stage::Release:
                        if( pos >= release )
                        {
                            level = 0.f;
                            stage = Stage::Idle;
                            return level;
                        }
                        level = startLevel * ( 1.f - ( pos / release ) );
                        pos += 1.f;
                        return level;
                }
            }
        }

    private:
        double sampleRate{ 44100.0 };
        float  attack{ 0.f }, hold{ 0.f }, decay{ 0.f }, release{ 0.f }, sustain{ 1.f };
        float  level{ 0.f }, startLevel{ 0.f }, pos{ 0.f };
        Stage  stage{ Stage::Idle };
    };
} // namespace mt
