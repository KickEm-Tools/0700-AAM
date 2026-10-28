/*
 * Copyright (c) 2024 MUSIC Tribe
 *
 * This file is part of Kick.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "kick_PadModifiers.h"
#include "contracts/dcl/dcl_signals.h"
#include "contracts/dcl/dcl_contracts_helper.h"

namespace kick_ui
{
    namespace
    {
        using DCC = contracts::DeviceControlContracts;

        void log( const juce::String& S ) { juce::Logger::writeToLog( "PadModifiers: " + S ); }

        void setLed( int Id, bool On )
        {
            contracts::DclSignals::sendCommandToDcl( On ? contracts::DclContractsHelper::setLEDONMessage( Id ) : contracts::DclContractsHelper::setLEDOFFMessage( Id ) );
        }

        double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
    }

    PadModifiers::PadModifiers( Dispatch ToScreen )
        : toScreen( std::move( ToScreen ) )
    {
    }

    PadModifiers::~PadModifiers() { stopTimer(); }

    bool PadModifiers::handle( CPktMgr::HWMsg_t& Msg, Screen Current )
    {
        if( Current != lastScreen )
        {
            // A new screen: nothing keeps repeating behind it.
            held.clear();
            erasedPads.clear();
            lastScreen = Current;
        }

        if( contracts::DclContractsHelper::isButtonMessage( Msg ) )
            return handleButton( contracts::DclContractsHelper::getButtonCode( Msg ),
                                 contracts::DclContractsHelper::getButtonState( Msg ) == DCC::BTN_PRESSED );

        if( contracts::DclContractsHelper::isPadButtonMessage( Msg ) )
            return handlePad( Msg, Current );

        return false;
    }

    bool PadModifiers::handleButton( int Code, bool Pressed )
    {
        switch( Code )
        {
            case DCC::EN_FULL_LEVEL:
                if( Pressed ) setLevel( level == Level::Full ? Level::Off : Level::Full );
                return true;

            case DCC::EN_HALF_LEVEL:
                if( Pressed ) setLevel( level == Level::Half ? Level::Off : Level::Half );
                return true;

            case DCC::EN_16LEVELS:
                if( Pressed ) setSixteenLevels( !sixteenLevels );
                return true;

            case DCC::EN_NOTE_REPEAT:
                // Momentary: repeat while the key is held. Pressing it while latched unlatches.
                if( Pressed && repeatLatched )
                    repeatLatched = false;
                repeatHeld = Pressed;
                updateRepeatLed();
                return true;

            case DCC::EN_LATCH:
                if( Pressed )
                {
                    repeatLatched = !repeatLatched;
                    updateRepeatLed();
                }
                return true;

            case DCC::EN_ERASE:
                // ERASE also keeps its screen meaning (GRID: delete the selected notes,
                // ASSIGN SAMPLE: clear the pad), so it is not consumed.
                eraseHeld = Pressed;
                if( !Pressed ) erasedPads.clear();
                return false;

            default: break;
        }
        return false;
    }

    bool PadModifiers::handlePad( CPktMgr::HWMsg_t& Msg, Screen Current )
    {
        if( Msg.data.size() < 3 )
            return false;

        auto&      ae      = mt::getAudioInterface();
        const int  pad     = Msg.data[0];
        const bool pressed = Msg.data[1] == DCC::BTN_PRESSED;
        const bool plays   = playsPads( Current );

        // ERASE held + pad: erase the pad's notes from the selected track, no sound.
        if( plays && ( eraseHeld || erasedPads.count( pad ) ) )
        {
            if( pressed && eraseHeld )
            {
                const auto& sel    = ae.getSelected();
                const int   note   = ( sixteenLevels ? sixteenSource : pad ) + ae.getBankOffset();
                const int   before = ae.getNumNotes( sel );
                ae.deleteNotesInRange( sel, mt::NotesInRange( note, note ) );
                log( "ERASE pad " + juce::String( pad + 1 ) + " note " + juce::String( note ) + ": " + juce::String( before - ae.getNumNotes( sel ) )
                     + " notes erased from track " + juce::String( sel.track + 1 ) );
                erasedPads.insert( pad );
            }
            else if( !pressed )
                erasedPads.erase( pad );
            return true;
        }

        if( plays && pressed && !sixteenLevels )
            lastPad = pad;

        // 16 LEVELS: every pad plays the source pad, at the level of its position.
        if( plays && sixteenLevels )
        {
            Msg.data[0] = sixteenSource;
            if( pressed ) Msg.data[2] = sixteenLevelVelocity( pad );
        }
        else if( pressed && level != Level::Off )
            Msg.data[2] = level == Level::Full ? 127 : 64;

        // NOTE REPEAT: remember held pads; the timer retriggers them.
        if( plays )
        {
            if( pressed && repeatActive() )
            {
                const auto interval = repeatIntervalMs();
                const auto toGrid   = msToNextGridLine();
                held[pad]           = { Msg, nowMs() + ( toGrid > 0.0 ? toGrid : interval ) };
                log( "note repeat pad " + juce::String( pad + 1 ) + " every " + juce::String( interval, 1 ) + " ms" );
                if( !isTimerRunning() ) startTimer( 2 );
            }
            else if( !pressed && held.erase( pad ) && held.empty() )
                stopTimer();
        }
        return false;
    }

    void PadModifiers::setLevel( Level L )
    {
        level = L;
        contracts::DclContractsHelper::setFullLevel( L == Level::Full );
        contracts::DclContractsHelper::setHalfLevel( L == Level::Half );
        // Off first, so a surface that shows both on one key ends up showing the one that is on.
        if( L != Level::Full ) setLed( DCC::EN_FULL_LEVEL, false );
        if( L != Level::Half ) setLed( DCC::EN_HALF_LEVEL, false );
        if( L == Level::Full ) setLed( DCC::EN_FULL_LEVEL, true );
        if( L == Level::Half ) setLed( DCC::EN_HALF_LEVEL, true );
        log( juce::String( "level " ) + ( L == Level::Full ? "FULL (127)" : L == Level::Half ? "HALF (64)" : "off (pad velocity)" ) );
    }

    void PadModifiers::setSixteenLevels( bool On )
    {
        sixteenLevels = On;
        setLed( DCC::EN_16LEVELS, On );
        auto& ae = mt::getAudioInterface();
        if( On )
        {
            sixteenSource = lastPad;
            // Show the levels on the pads: the source pad's colour, dim to bright.
            const int        note   = sixteenSource + ae.getBankOffset();
            auto*            prog   = ae.getSelectedProgramData();
            const int        colour = prog ? prog->padColour[(size_t)juce::jlimit( 0, 127, note )].get() : (int)DCC::PadColourNeutralWhite;
            std::vector<int> colours( 16, colour ), velocities;
            for( int i = 0; i < 16; ++i ) velocities.push_back( sixteenLevelVelocity( i ) );
            contracts::DclSignals::sendCommandToDcl( contracts::DclContractsHelper::setPadColourByVelocityMessage( DCC::PadButtonState::StateOn, colours, velocities ) );
            log( "16 LEVELS on, source pad " + juce::String( sixteenSource + 1 ) + " (note " + juce::String( note ) + ")" );
        }
        else
        {
            contracts::DclSignals::refreshPadLeds();
            log( "16 LEVELS off" );
        }
    }

    void PadModifiers::updateRepeatLed()
    {
        setLed( DCC::EN_NOTE_REPEAT, repeatActive() );
        setLed( DCC::EN_LATCH, repeatLatched );
        if( !repeatActive() )
        {
            held.clear();
            stopTimer();
        }
        log( juce::String( "note repeat " ) + ( repeatActive() ? "on" : "off" ) + ( repeatLatched ? " (latched)" : "" ) );
    }

    bool PadModifiers::playsPads( Screen S )
    {
        // Screens whose pads play the selected track's program (not steps, mutes or sequences).
        switch( S )
        {
            case Screen::HomeListView:
            case Screen::HomeGridView:
            case Screen::MenuChannelMixer:
            case Screen::MenuProgramEdit:
            case Screen::MenuPadPerform: return true;
            default: return false;
        }
    }

    int PadModifiers::sixteenLevelVelocity( int Pad )
    {
        return juce::jlimit( 1, 127, ( juce::jlimit( 0, 15, Pad ) + 1 ) * 127 / 16 );
    }

    double PadModifiers::repeatIntervalMs()
    {
        auto&     ae    = mt::getAudioInterface();
        const int seq   = ae.getSelectedSequence();
        double    bpm   = ae.getMasterBPM();
        int       ticks = mt::defaultPPQN / 4; // 1/16
        if( ae.getNumSequences() > 0 )
        {
            if( auto* tm = ae.getTempoMgr( seq ) )
            {
                ticks          = juce::jmax( 1, tm->getTicksPerQuantization() );
                auto* seqData  = ae.getSequenceData( seq );
                if( seqData && !seqData->isMasterBPM.get() ) bpm = tm->getBPM();
            }
        }
        bpm = juce::jlimit( 20.0, 400.0, bpm > 0 ? bpm : 120.0 );
        return 60000.0 / bpm * ticks / mt::defaultPPQN;
    }

    double PadModifiers::msToNextGridLine()
    {
        // While the sequencer plays, put the repeats on the time-correct grid.
        auto& ae = mt::getAudioInterface();
        if( !ae.isSequencePlaying() )
            return 0.0;
        const int ppqn  = mt::defaultPPQN;
        int       ticks = ppqn / 4;
        if( auto* tm = ae.getTempoMgr( ae.getSelectedSequence() ) ) ticks = juce::jmax( 1, tm->getTicksPerQuantization() );
        const auto interval = repeatIntervalMs();
        const int  tick     = juce::jmax( 0, ae.getPlayhead().bnb.tick ) % ticks;
        auto       ms       = interval * ( ticks - tick ) / ticks;
        return ms < 5.0 ? ms + interval : ms;
    }

    void PadModifiers::timerCallback()
    {
        if( held.empty() || !repeatActive() )
        {
            stopTimer();
            return;
        }
        const auto now = nowMs();
        for( auto& [pad, h] : held )
        {
            if( now < h.nextMs )
                continue;
            auto release    = h.press;
            release.data[1] = DCC::BTN_RELEASED;
            auto press      = h.press;
            toScreen( release );
            toScreen( press );
            const auto interval = repeatIntervalMs();
            h.nextMs += interval;
            if( h.nextMs < now ) h.nextMs = now + interval; // fell behind (UI busy): do not burst
        }
    }
}
