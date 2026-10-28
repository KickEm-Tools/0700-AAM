/*
 * Copyright (c) 2021 MUSIC Tribe
 *
 * This file is part of Kick.
 *
 * Kick is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Kick is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with Kick. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// module include files...
#include "audio_voice.h"

namespace mt
{
    /// @brief
    Metronome::Metronome( AudIO::Design& D, const juce::ValueTree& Tree, const juce::String& Name )
        : ISynthObject( D, 1, AudIO::BusType::Other )
        , slice( Tree, nullptr )
    {
        setName( Name.toStdString() );
        DawInserts::setBypassAllEffects( true );
        setBypass( false );

        // Pre-reseve Memory...
        for( auto i = 0; i < getPolyphony(); ++i )
            voices.emplace_back( std::make_unique<AVoice>( master ) );

        ISynthObject::addVoices( voices );
        slice.setReSampleRate( getSampleRate() );
        accentSlice.setReSampleRate( getSampleRate() );
    }

    Metronome::~Metronome()
    {
        noteOffAll();
        setBypass( true );
        voices.clear();
    }

    void Metronome::setSource( juce::String& File )
    {
        auto       path = getAppDirs( MTAssets ).getChildFile( File );
        juce::File accent;
        if( !path.existsAsFile() )
        {
            // The factory click sounds (KickSettings MStyle0..7) are not part of the public
            // release. Fall back to a click set under assets/StarterKit/Metronome (the KickEmulator
            // installs its synthesised CC0 Starter Kit there), which has a separate accent for the
            // bar. With the factory click present nothing changes.
            path   = getAppDirs( MTAssets ).getChildFile( "StarterKit/Metronome/Click.wav" );
            accent = path.getSiblingFile( "Click Accent.wav" );
        }
        auto sound = getAudioSoundCache().getOrLoadMetronomeSound( path.getFullPathName() );
        slice.setSource( sound );
        accentSlice.setSource( accent.existsAsFile() ? getAudioSoundCache().getOrLoadMetronomeSound( accent.getFullPathName() ) : nullptr );
    }

    void Metronome::noteOn( int Note, int Velocity )
    {
        noteOff( Note );
        if( auto voice = getFreeVoice( Note, 1 ) )
        {
            // Note 0 is the bar (MTMetronome::triggerBar), note 1 the beat.
            voice->setSlice( ( 0 == Note && accentSlice.source ) ? &accentSlice : &slice );
            voice->noteOn( Note, Velocity );
            addVoiceForPlayback( voice );
        }
    }

};
