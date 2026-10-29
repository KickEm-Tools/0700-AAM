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

namespace mt
{
    static int gTrackCtr = 0;

    MTTrack::MTTrack( MTAudioManager& M, MTSequence& Seq, juce::UndoManager* UM, juce::ValueTree& VT )
        : MTITrackData( VT, UM )
        , MTNoteList( VT, UM, Seq )
        , manager( M )
        , seq( Seq )
        , undoManager( UM )
        , vTree( VT )
    {
        ++gTrackCtr;

        if( 0 >= vTree.getProperty( IDs::id ).toString().length() )
            vTree.setProperty( IDs::id, CommonUtils::createID(), nullptr );

        if( 0 >= vTree.getProperty( IDs::name ).toString().length() )
            vTree.setProperty( IDs::name, ( "Track " + juce::String( gTrackCtr ) ), nullptr );

        vTree.addListener( this );

        // Locate midi file for this track...
        /*
        {
            auto       projDir  = manager.getAssetManager()->getProjDir();
            auto       seqData  = Seq.getData();
            auto       n        = seqData.Name + juce::File().getSeparatorString() + data.Name + ".mid";
            juce::File midiFile = projDir.getChildFile( n );
            if( !midiFile.exists() )
            {
                midiFile.create();
            }
            {
                // Read Midifile...
                juce::MidiFile mFile;
                auto           stream = juce::FileInputStream( midiFile );
                mFile.readFrom( stream );
                if( auto trk = mFile.getTrack( 0 ) )
                    midiSeq = *trk;

                MTNoteList::setMidiList( &midiSeq );
            }
        }
        */

        // Setup handler for program being deleted
        programBeingDeleted.connect( [this]( MTIProgram* MTIProg ) {
            if( program.load() == MTIProg )
            {
                setProgram( nullptr );
            }
        } );

        // Restore program.
        // Connect handler to length change handler.
        lengthChangeHandler();
        progChangeHandler();
    }

    void MTTrack::setName( const juce::String& Name )
    {
        if( Name.isNotEmpty() )
            MTITrackData::name = Name;
    }

    void MTTrack::setLevel( int Level ) { MTITrackData::level = Level; }
    void MTTrack::setPan( const float Pan ) { MTITrackData::pan = Pan; }
    void MTTrack::setLength( const int Length )
    {
        auto len             = juce::jmin<int>( seq.length.get(), Length );
        MTITrackData::length = len;
    }
    void MTTrack::setUsingSeqLength( bool UseSeqLength ) { MTITrackData::isSeqLength = UseSeqLength; }
    void MTTrack::setMute( bool Muted )
    {
        MTITrackData::mute = Muted;
        if( 0 < muteGroup.get() )
        {
            seq.forAllTracks( [&, Muted]( MTTrack* t ) {
                if( muteGroup == t->muteGroup )
                {
                    if( t->isMute() != Muted )
                        t->setMute( Muted );
                }
            } );
        }
    }

    void MTTrack::setTranspose( int Transpose ) { MTITrackData::transpose = Transpose; }
    void MTTrack::setMidiPort( const juce::String& MidiPort ) { MTITrackData::midiPort.setValue( midiPort, undoManager ); }
    void MTTrack::setMidiChannel( int MidiChannel ) { MTITrackData::midiChannel = MidiChannel; }
    void MTTrack::setMidiOutChannel( int MidiChannel ) { MTITrackData::midiOutChannel = MidiChannel; }
    void MTTrack::setMuteGroup( int Group ) { MTITrackData::muteGroup = Group; }

    bool MTTrack::isMidiInAllowed( const juce::String& Source, int Channel ) const noexcept
    {
        auto midiInCh   = MTITrackData::midiChannel.get();
        auto midiInPort = MTITrackData::midiPort.get();

        auto isSrcAllowed     = ( 0 == midiInPort.compareIgnoreCase( "all" ) ) || ( 0 == midiInPort.compareIgnoreCase( Source ) );
        auto isChannelAllowed = ( 0 >= midiInCh ) || ( Channel == midiInCh );
        return ( isSrcAllowed && isChannelAllowed );
    }

    MTIProgram* MTTrack::getProgram() const { return program.load(); }

    int MTTrack::setProgram( MTIProgram* p )
    {
        MTITrackData::programId = p ? p->id.get() : "";
        return 0;
    };

    void MTTrack::progChangeHandler()
    {
        auto newProgram = manager.getProgramById( programId.get() );
        if( newProgram != program.load() )
        {
            stopAllMidiNotes();
            program.store( newProgram );
            manager.triggerTrackProgramChanged( getIndex(), program.load() );
        }
    }

    void MTTrack::lengthChangeHandler()
    {
        auto trackLength = getLength();
        lengthInTicks    = ( (unsigned int)( seq.getTicksUpto( trackLength ) ) );
        lengthFactor     = ( 1.0 / lengthInTicks );
    }

    void MTTrack::valueTreePropertyChanged( juce::ValueTree& TreeWhosePropertyHasChanged, const juce::Identifier& Property )
    {
        if( vTree == TreeWhosePropertyHasChanged )
        {
            if( Property == IDs::name )
                trackNameChanged( this );
            else if( Property == IDs::level )
                levelScaler = ( level.get() * MIDI::SCALER );
            else if( Property == IDs::solo )
                seq.updateTrackSolo( this );
            else if( Property == IDs::length || Property == IDs::usingSequenceLength )
                lengthChangeHandler();
            else if( Property == IDs::program )
                progChangeHandler();
        }
    }

    void MTTrack::handleAsyncUpdate()
    {
        manager.triggerTrackNotesChanged( manager.getSequenceIndex( &seq ), getIndex() );

        /*
        auto projDir = manager.getAssetManager()->getProjDir();
        auto n = seq.getName() + juce::File().getSeparatorString() + getName() + ".mid";
        juce::File midiFile = projDir.getChildFile( n );
        if( !midiFile.exists() )
        {
            midiFile.create();
        }
        {
            midiFile.replaceWithData( nullptr, 0 );
            juce::MidiFile mFile;
            mFile.setTicksPerQuarterNote( defaultPPQN );
            mFile.addTrack( midiSeq );
            auto stream = juce::FileOutputStream( midiFile );
            mFile.writeTo( stream );
        }
        */
    }

    void MTTrack::play()
    {
        // play / pause / stop come from the message thread (keys) or the tick thread (NEXT SEQ);
        // onTick runs on the tick thread under the same lock, and stopAllMidiNotes clears the
        // queuedNotes it iterates.
        const juce::ScopedLock sl( lock );
        isPlaying = true;
        // The variables are cleared here and in stop as the workflow may be pause->playstart. if Ui calls stop and resume then it may lead to issues.
        trackPlayhead.clear();
        trackPlayhead.bnb.bar = seq.isLooping.get() ? juce::jmax<int>( 0, seq.getLoopStart() - 1 ) : 0;
        prevTick              = 0;
        tickCtr               = 0;
        tickPos               = 0;
        playFromStart         = true;
    }

    void MTTrack::resume()
    {
        isPlaying = true;
    }

    void MTTrack::pause()
    {
        const juce::ScopedLock sl( lock );
        isPlaying = false;
        stopAllMidiNotes();
    }

    void MTTrack::stop()
    {
        const juce::ScopedLock sl( lock );
        stopAllMidiNotes();
        isPlaying = false;
        trackPlayhead.clear();
        trackPlayhead.bnb.bar = seq.isLooping.get() ? juce::jmax<int>( 0, seq.getLoopStart() - 1 ) : 0;
        prevTick              = 0;
        tickCtr               = 0;
        tickPos               = 0;
    }

    void MTTrack::seek( int posInTicks )
    {
        tickPos = posInTicks;
        if( seq.isLooping.get() )
            while( tickPos > lengthInTicks.load() ) // This is cheap modulo
                tickPos -= lengthInTicks.load();

        //// restrict the position between loop start and sequence length.
        //tickPos  = juce::jlimit<int>( 0, seq.getTicksUpto( getLength() ), posInTicks );
        //auto bnb = seq.toBnB( tickPos );

        //const juce::ScopedLock sl( lock );

        //trackPlayhead.bnb.tick = bnb.tick;
        //trackPlayhead.bnb.bar  = bnb.bar;
        //trackPlayhead.bnb.beat = bnb.beat;

        recalculateIndex( tickPos );
        updateTrackPlayhead( tickPos );
    }

    void MTTrack::recalculateIndex( unsigned int BeginTick )
    {
        noteIdx = 0;

        // If zero
        if( 0 == BeginTick )
            return;

        // Loop to find correct index...
        const auto& vt = getNoteListVT();
        while( noteIdx < vt.getNumChildren() )
        {
            auto     tmp = vt.getChild( noteIdx );
            MTNoteRO note( tmp );
            if( note.start() < (int)BeginTick )
                noteIdx++;
            else
                break;
        }
    }

    void MTTrack::onBeat()
    {
        trackPlayhead.bnb.tick = 0;
        trackPlayhead.bnb.beat++;
    };

    void MTTrack::onBar()
    {
        trackPlayhead.bnb.beat = 0;
        int last               = seq.isLooping.get() ? juce::jmin<int>( getLength(), seq.getLoopEnd() ) : juce::jmin<int>( getLength(), seq.getLength() );
        if( ++trackPlayhead.bnb.bar % last == 0 )
        {
            trackPlayhead.bnb.bar = trackPlayhead.bnb.bar = seq.isLooping.get() ? juce::jmax<int>( 0, seq.getLoopStart() - 1 ) : 0;
            trackPlayhead.loopCount++;
        }
    }

    void MTTrack::fillMidiMessages( unsigned int tick, unsigned int PrevTick, int MidiChannel )
    {
        //juce::Logger::writeToLog( "prevTick, " + juce::String( PrevTick ) + "PresentTick, " + juce::String(tick));
        juce::HashMap<int, MTNote*>* map = nullptr;
        // The clock's first tick after play() is 1 or more, never 0: include the starting position,
        // or the notes on the first downbeat are skipped until the sequence loops.
        auto first = PrevTick + 1;
        if( playFromStart )
        {
            playFromStart = false;
            first         = PrevTick;
        }
        for( auto pos = first; pos <= tick; pos++ )
        {
            int posOnTrack = pos % lengthInTicks.load();
            map            = getNotesAtPos( posOnTrack );
            if( !map ) continue;
            for( const auto& it : *map )
            {
                // RECORD take: the notes from before the punch-in are being replaced.
                if( replacing.load() && it->id.get() <= takeLastOldId.load() )
                    continue;

                int noteNo = getEffectiveNoteNo( it->number.get() );
                if( !isTrackEffectivelyMuted() && CommonUtils::isValidMidiNote( noteNo ) )
                {
                    // Check probability of playback.
                    auto rand = AudIO::utils::random<int>( PROBABILITY::MIN, PROBABILITY::MAX );
                    if( rand <= it->probability.get() )
                    {
                        int noteVelocity = getEffectiveNoteVelocity( it->velocity.get() );

                        // Trigger note now.
                        midiMsgs.insert( -1, juce::MidiMessage::noteOn( MidiChannel, noteNo, (juce::uint8)noteVelocity ) );

                        // KICK_LOG_NOTES=1 logs every note the sequencer plays (read once; unset, nothing).
                        static const bool logNotes = juce::SystemStats::getEnvironmentVariable( "KICK_LOG_NOTES", {} ) == "1";
                        if( logNotes )
                            juce::Logger::writeToLog( "Note: track '" + getName() + "' tick " + juce::String( pos ) + " pos " + juce::String( posOnTrack ) + " note " + juce::String( noteNo ) + " velocity " + juce::String( noteVelocity ) + " id " + juce::String( it->id.get() ) );

                        // Check Ratchet
                        auto parts = (int)( it->ratchet.get() );
                        if( 1 < parts )
                        {
                            auto time      = tickCtr;
                            auto partTicks = ( it->length.get() / parts );
                            // Queue Retriggers...
                            for( auto i = 2; i <= parts; ++i )
                            {
                                queuedNotes.insert( { time - 1, juce::MidiMessage::noteOff( MidiChannel, noteNo, (juce::uint8)noteVelocity ) } );
                                time += partTicks;
                                queuedNotes.insert( { time, juce::MidiMessage::noteOn( MidiChannel, noteNo, (juce::uint8)noteVelocity ) } );
                            }
                        }
                        // Queue last note off.
                        queuedNotes.insert( { ( pos + it->length.get() - 1 ), juce::MidiMessage::noteOff( MidiChannel, noteNo, (juce::uint8)noteVelocity ) } ); // Queue note-off
                    }
                }
            }
        }
    }

    void MTTrack::onTick( unsigned int Tick )
    {
        auto pProgram = program.load();
        if( !pProgram || ( 0 == lengthInTicks.load() ) )
            return;

        if( pProgram->isAudio() )
            return;

        const juce::ScopedLock sl( lock );
        if( Tick < prevTick ) // clock restarted (play() while a tick was in flight)
            prevTick = 0;
        if( playFromStart.load() && Tick > 96 ) // a tick of the previous clock run, after play()
            return;
        unsigned int diffTick = Tick - prevTick;

        if( recording.load() )
            takeTicks += diffTick;

        tickCtr += diffTick;
        tickPos += diffTick;
        trackPlayhead.bnb.tick += diffTick;
        auto isEffectivelyMute   = isTrackEffectivelyMuted();
        auto trackMidiOutChannel = getMidiOutChannel();

        // Check if any pending queued note off's are to be sent...
        for( auto p = queuedNotes.begin(); p != queuedNotes.end(); )
        {
            if( p->first <= tickCtr )
            {
                midiMsgs.insert( -1, p->second );
                p = queuedNotes.erase( p );
            }
            else
                ++p;
        }

        fillMidiMessages( Tick, prevTick, trackMidiOutChannel );

        prevTick = Tick;

        if( tickPos >= lengthInTicks.load() )
        {
            tickPos -= lengthInTicks.load();
            noteIdx = 0;
        }

        updateTrackPlayhead( tickPos );
        // add logic to compute normalized playhead position using lengthInTicks
        if( !midiMsgs.isEmpty() )
        {
            reloadMidiMeter( levelScaler );
            pProgram->noteProcess( midiMsgs );
            midiMsgs.clearQuick();
        }
    }

    int MTTrack::processIncomingMidiMessage( const juce::MidiMessage& Msg )
    {
        auto pProgram = program.load();
        if( pProgram && !isTrackEffectivelyMuted() )
        {
            juce::Array<juce::MidiMessage> list;

            if( Msg.isNoteOnOrOff() )
            {
                auto effCh = getMidiOutChannel();
                auto effNo = getEffectiveNoteNo( Msg.getNoteNumber() );

                if( Msg.isNoteOn() )
                {
                    auto effVel = getEffectiveNoteVelocity( Msg.getVelocity() );
                    list.insert( -1, juce::MidiMessage::noteOn( effCh, effNo, (juce::uint8)effVel ) );
                }
                else
                    list.insert( -1, juce::MidiMessage::noteOff( effCh, effNo ) );
            }
            else
                list.insert( -1, Msg );

            pProgram->noteProcess( list );
        }
        if( !isTrackEffectivelyMuted() )
            reloadMidiMeter( levelScaler );

        return 0;
    }

    int MTTrack::processIncomingMidiMessage( const MTMidiMessage& Msg )
    {
        if( isMidiInAllowed( Msg.source->getName(), Msg.message.getChannel() ) )
            return processIncomingMidiMessage( Msg.message );
        return -1;
    }

    /*inline void MTTrack::addMidiNote( int NoteNumber, int Velocity )
    {
        auto msg = juce::MidiMessage::noteOn( getMidiOutChannel(), NoteNumber, (juce::uint8)Velocity );
        processIncomingMidiMessage( msg );
    }

    inline void MTTrack::stopMidiNote( int NoteNumber )
    {
        auto msg = juce::MidiMessage::noteOff( getMidiOutChannel(), NoteNumber );
        processIncomingMidiMessage( msg );
    }*/

    inline void MTTrack::stopAllMidiNotes()
    {
        auto msg = juce::MidiMessage::allNotesOff( getMidiOutChannel() );
        processIncomingMidiMessage( msg );
        queuedNotes.clear();
    }

    ///
    /// Live note recording.
    ///
    void MTTrack::startRecording( bool Replace )
    {
        if( recording.load() )
            stopRecording();

        const juce::ScopedLock sl( lock );
        const auto             len = lengthInTicks.load();

        // Notes that existed before the take: ids are unique and only grow (MTNoteUtil::createVT).
        int lastId = 0;
        for( auto n : getNoteListVT() )
            lastId = juce::jmax( lastId, (int)n.getProperty( IDs::id ) );

        // The first position the take plays: the next tick, or position 0 before the first tick.
        takeStart = ( 0 == len ) ? 0 : ( playFromStart.load() ? tickPos : ( tickPos + 1 ) ) % len;
        takeTicks.store( 0 );
        takeLastOldId.store( lastId );
        heldNotes.clear();
        replacing.store( Replace );
        recording.store( true );
        juce::Logger::writeToLog( "Record: take on track '" + getName() + "' (" + ( Replace ? "RECORD, replace" : "OVERDUB" ) + ") from tick " + juce::String( takeStart ) );
    }

    void MTTrack::stopRecording()
    {
        if( !recording.load() )
            return;

        const juce::ScopedLock sl( lock );
        const auto             len = lengthInTicks.load();
        recording.store( false );

        // Close notes still held (pad not released before STOP).
        if( 0 < len )
        {
            const auto pos = tickPos % len;
            for( auto& it : heldNotes )
            {
                auto length = (int)( ( pos + len - it.second.first ) % len );
                addNote( it.first, it.second.second, PROBABILITY::MAX, 1, (int)it.second.first, juce::jmax( 1, length ), true );
            }
        }
        heldNotes.clear();

        // RECORD: erase the old notes the playhead passed during the take.
        int erased = 0;
        if( replacing.load() && 0 < len )
        {
            const auto      passed = takeTicks.load();
            juce::Array<int> ids;
            for( auto n : getNoteListVT() )
            {
                const int id    = n.getProperty( IDs::id );
                const int start = n.getProperty( IDs::start );
                const auto dist = (unsigned int)( ( (unsigned int)juce::jmax( 0, start ) + len - takeStart ) % len );
                if( id <= takeLastOldId.load() && ( passed >= len || dist < passed ) )
                    ids.add( id );
            }
            for( auto id : ids )
                eraseNote( id );
            erased = ids.size();
        }
        replacing.store( false );
        triggerAsyncUpdate();
        juce::Logger::writeToLog( "Record: take on track '" + getName() + "' ended after " + juce::String( takeTicks.load() ) + " ticks, " + juce::String( erased ) + " old notes erased, " + juce::String( getNumNotes() ) + " notes" );
    }

    void MTTrack::recordMidiMessage( const juce::MidiMessage& Msg )
    {
        if( !recording.load() || !Msg.isNoteOnOrOff() )
            return;

        const juce::ScopedLock sl( lock );
        const auto             len = lengthInTicks.load();
        if( 0 == len )
            return;

        // The position of the last tick played: the hit sounds now, between this tick and the next.
        const auto pos = tickPos % len;
        const int  no  = Msg.getNoteNumber();
        if( Msg.isNoteOn() )
        {
            heldNotes[no] = { pos, (int)Msg.getVelocity() };
        }
        else if( auto it = heldNotes.find( no ); it != heldNotes.end() )
        {
            auto       length = (int)( ( pos + len - it->second.first ) % len );
            const auto played = (int)it->second.first;
            const auto start   = recordTimingCorrect ? ( recordTimingCorrect( played ) % (int)len ) : played; // TC
            addNote( no, it->second.second, PROBABILITY::MAX, 1, start, juce::jmax( 1, length ), true );
            juce::Logger::writeToLog( "Record: note " + juce::String( no ) + " velocity " + juce::String( it->second.second ) + " at tick " + juce::String( start )
                                      + ( start != played ? " (played at " + juce::String( played ) + ", timing corrected)" : juce::String() ) + " length " + juce::String( juce::jmax( 1, length ) ) );
            heldNotes.erase( it );
            triggerAsyncUpdate();
        }
    }

#if 0
    struct lessNote
    {
        template<typename T>
        bool operator()( const T& l, const T& r ) const
        {
            return l.second < r.second;
        }
    };

    int MTTrack::explode( const MTSelected& /*selection*/, std::vector<MTNoteData>* /*Notes*/ )
    {
        std::multiset < std::pair<std::shared_ptr<MTNoteData>, int>, lessNote > noteList;
        for( auto& it : notes_.getAllNotes() )
        {
            noteList.insert( std::make_pair( it, it->NoteNumber ) );
        }

        int prevNoteNum = -1;
        int trackIndex = -1;
        for( auto& it : noteList )
        {
            if( it.second != prevNoteNum )
            {
                trackIndex = sequence.createTrack( getName() + "_" + std::to_string( it.second ) );
                sequence.getTracks()[(size_t)trackIndex]->setProgram( getProgram() );
                prevNoteNum = it.second;
            }
            sequence.getTracks()[(size_t)trackIndex]->addNote( *it.first );
        }
        return 0;
    }

    int MTTrack::setLengthInBarAndBeats( int Bar, double Beats )
    {
        return setLength( sequence.tempoSequence.barsBeatsToTime( { Bar, Beats } ) );
    }

    double MTTrack::modifyLength( double Length, double Pos, bool Insert )
    {
        if( 0 <= Length )
        {
            // Update new Length...
            auto currLength = getLength();
            length = (Insert ? (currLength + Length) : (currLength - Length));

            for( const auto& itr : track->getClips() )
            {
                if( itr->getName() == KickMidiClipName )
                {
                    auto startBeat = (Insert ? (Pos) : (Pos - Length));
                    auto delta = (Insert ? (Length) : (-Length));

                    for( auto& it : notes_.getAllNotes() )
                    {
                        if( it->StartBeat >= startBeat )
                            notes_.editStartAndLength( &it, (it->StartBeat + delta), it->LengthInBeats );
                    }
                }
                // Set maximum lengths...
                itr->setLength( length, true );
            }
        }
        return getLength();
    }

#endif
}
