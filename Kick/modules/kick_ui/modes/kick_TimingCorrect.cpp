/*
 * Copyright (c) 2024 MUSIC Tribe
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

namespace kick_ui
{
    std::atomic<bool> TimingCorrect::enabled{ false };
    bool              TimingCorrect::windowOpen{ false };

    const std::vector<juce::String>& TimingCorrect::noteValueNames()
    {
        static const std::vector<juce::String> names{ "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T" };
        return names;
    }

    const std::vector<mt::MTTimeBase>& TimingCorrect::noteValueBases()
    {
        static const std::vector<mt::MTTimeBase> bases{ mt::MTTimeBase::_4, mt::MTTimeBase::_4T, mt::MTTimeBase::_8, mt::MTTimeBase::_8T,
                                                         mt::MTTimeBase::_16, mt::MTTimeBase::_16T, mt::MTTimeBase::_32, mt::MTTimeBase::_32T };
        return bases;
    }

    TimingCorrect::TimingCorrect()
    {
        setName( "TIMING CORRECT" );
        mt::utils::addAndMakeVisible( this, onOff, noteValue, strength, info );

        strength.showTitleAndValue( true, juce::Slider::TextBoxBelow, juce::Justification::centred );
        strength.setNumDecimalPlacesToDisplay( 0 );
        strength.setRange( 0, 100, 1 );
        strength.setValue( 100, juce::dontSendNotification );
        noteValue.setJustificationType( juce::Justification::centred );
        info.setJustificationType( juce::Justification::centred );
        info.setTextColour( ui::Theme::textColour( 2 ) );

        auto& ae = mt::getAudioInterface();
        if( auto tempoMgr = ae.getTempoMgr( ae.getSelected().sequence ) )
        {
            const auto& bases = noteValueBases();
            const auto  q     = tempoMgr->getQuantization();
            for( size_t i = 0; i < bases.size(); ++i )
                if( magic_enum::enum_integer<mt::MTTimeBase>( bases[i] ) == q )
                    noteValue.setSelected( (int)i, juce::dontSendNotification );
        }
        onOff.setToggleState( isEnabled(), juce::dontSendNotification );

        auto track = ae.getTrackData( ae.getSelected().sequence, ae.getSelected().track );
        info.setText( track ? "Apply moves the notes of track \"" + track->name.get() + "\"" : juce::String( "No track selected" ), juce::dontSendNotification );
    }

    void TimingCorrect::paint( juce::Graphics& g )
    {
        g.fillAll( ui::Theme::backgroundColour( 1 ) );
    }

    void TimingCorrect::resized()
    {
        onOff.setBoundsRelative( 0.05f, 0.04f, 0.9f, 0.12f );
        noteValue.setBoundsRelative( 0.05f, 0.20f, 0.9f, 0.30f );
        strength.setBoundsRelative( 0.10f, 0.54f, 0.8f, 0.24f );
        info.setBoundsRelative( 0.0f, 0.82f, 1.0f, 0.12f );
    }

    void TimingCorrect::setEnabled( bool On )
    {
        enabled = On;
        contracts::DclSignals::sendCommandToDcl( On ? contracts::DclContractsHelper::setLEDONMessage( contracts::DeviceControlContracts::EN_TC )
                                                    : contracts::DclContractsHelper::setLEDOFFMessage( contracts::DeviceControlContracts::EN_TC ) );
        auto&        ae  = mt::getAudioInterface();
        juce::String msg = juce::String( "Timing Correct " ) + ( On ? "ON" : "OFF" );
        if( auto tempoMgr = ae.getTempoMgr( ae.getSelected().sequence ) )
            if( On )
                msg += "  1/" + juce::String( tempoMgr->getQuantization() );
        juce::Logger::writeToLog( "TimingCorrect: " + msg );
        mt::utils::showNotification( msg );
    }

    void TimingCorrect::showWindow()
    {
        if( windowOpen )
            return;
        windowOpen = true;
        auto p     = new TimingCorrect();
        juce::Logger::writeToLog( "TimingCorrect: window shown" );
        mt::utils::showOptions( p->getName(), p, [p]( int R ) {
            if( R == UI::APPLY )
            {
                const bool on = p->onOff.getToggleState();
                if( on != isEnabled() )
                    setEnabled( on );
                p->apply();
            }
            windowOpen = false;
            delete p;
        } );
    }

    int TimingCorrect::apply()
    {
        auto&      ae       = mt::getAudioInterface();
        const auto sel      = ae.getSelected();
        auto       tempoMgr = ae.getTempoMgr( sel.sequence );
        if( tempoMgr == nullptr )
            return -1;

        const auto idx = juce::jlimit( 0, (int)noteValueBases().size() - 1, noteValue.getSelectedIndex() );
        tempoMgr->setQuantization( noteValueBases()[(size_t)idx] );
        const int grid = tempoMgr->getTicksPerQuantization();
        if( !isEnabled() || grid <= 0 )
        {
            juce::Logger::writeToLog( "TimingCorrect: note value " + noteValueNames()[(size_t)idx] + " (" + juce::String( grid ) + " ticks), notes not moved (Timing Correct off)" );
            return 0;
        }

        // Collect first: moving a note re-sorts the note list.
        std::vector<std::pair<int, int>> notes; // id, start
        for( auto n : ae.getNoteList( sel.sequence, sel.track ) )
            notes.emplace_back( (int)n.getProperty( mt::IDs::id ), (int)n.getProperty( mt::IDs::start ) );

        const double amount = strength.getValue() / 100.0;
        int          moved  = 0;
        for( const auto& [id, start] : notes )
        {
            const int target = (int)std::lround( (double)start / grid ) * grid;
            const int delta  = (int)std::lround( ( target - start ) * amount );
            if( delta != 0 && 0 == ae.editNote( id, (double)delta, 0, 0.0 ) )
                ++moved;
        }
        juce::Logger::writeToLog( "TimingCorrect: note value " + noteValueNames()[(size_t)idx] + " (" + juce::String( grid ) + " ticks), strength "
                                  + juce::String( (int)strength.getValue() ) + "%: moved " + juce::String( moved ) + " of " + juce::String( (int)notes.size() ) + " notes" );
        return moved;
    }

    int TimingCorrect::correct( int StartTick )
    {
        if( !isEnabled() )
            return StartTick;
        auto& ae = mt::getAudioInterface();
        if( auto tempoMgr = ae.getTempoMgr( ae.getSelected().sequence ) )
            if( const int grid = tempoMgr->getTicksPerQuantization(); grid > 0 )
                return (int)std::lround( (double)StartTick / grid ) * grid;
        return StartTick;
    }
}
