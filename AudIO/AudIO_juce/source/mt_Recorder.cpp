/*
 * Copyright (c) 2022 MUSIC Tribe
 *
 * This file is part of AudIO.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

namespace mt
{
    Recorder::Recorder( AudIO::Design& D, AudIO::BusType Type )
        : AudIO::Object( 2, 2, 0 )
        , AudIO::DawRouting( D, Type, AudIO::BusWidth::Stereo )
    {
        setName( "Recorder" );
        // The routing id is the object id, as in AudIO::DRecorder and Kick's ISynthObject.
        // Without it initAudioRouting() / connectInputTo() patch an uninitialised id: the
        // recorder then hears no input and its output reaches no bus (observed on the
        // desktop build: with a test tone on the input the sampler waited for a signal for ever).
        AudIO::DawRouting::setID( getId() );
        inMeterData.resize( 2 );
        outMeterData.resize( 2 );
    }

    Recorder::~Recorder() = default;

    void Recorder::notify( const std::function<void()>& Fn )
    {
        if( Fn )
            juce::MessageManager::callAsync( [Fn] { Fn(); } );
    }

    void Recorder::notifyLength()
    {
        // Same contract as AudIO::AudioRecorder: the configured length when it is set, the
        // recorded length when recording stops. Not per block.
        if( onRecorderLengthChanged )
        {
            auto       fn      = onRecorderLengthChanged;
            const auto rec     = recordedSamples.load();
            const auto seconds = ( isRecording() || rec == 0 ) ? lengthSeconds : ( sampleRate > 0 ? (double)rec / sampleRate : 0.0 );
            juce::MessageManager::callAsync( [fn, seconds] { fn( seconds ); } );
        }
    }

    void Recorder::prepareToPlay( uint32_t Samplerate, uint32_t Blocksize )
    {
        AudIO::Object::prepareToPlay( Samplerate, Blocksize );
        if( Samplerate > 0 && (double)Samplerate != sampleRate )
        {
            sampleRate = (double)Samplerate;
            if( lengthSeconds > 0.0 )
                setLength( lengthSeconds );
        }
    }

    void Recorder::teardown()
    {
        recState.store( RecIdle );
        playing.store( false );
        AudIO::Object::teardown();
    }

    void Recorder::setLength( double TimeInSeconds )
    {
        {
            const juce::SpinLock::ScopedLockType sl( lock );

            lengthSeconds       = juce::jmax( 0.0, TimeInSeconds );
            const int numSample = (int)std::llround( lengthSeconds * sampleRate );
            audioBuffer.setSize( 2, juce::jmax( 0, numSample ), true, true, true );
            if( recordedSamples.load() > numSample )
                recordedSamples.store( numSample );
            if( loopMode && recordedSamples.load() > 0 )
                recordedSamples.store( numSample ); // the loop is always the whole length
            if( writePos.load() >= numSample ) writePos.store( 0 );
            if( readPos.load() >= numSample ) readPos.store( 0 );
        }
        if( onRecorderLengthChanged )
        {
            auto fn      = onRecorderLengthChanged;
            auto seconds = lengthSeconds;
            juce::MessageManager::callAsync( [fn, seconds] { fn( seconds ); } );
        }
    }

    void Recorder::reset()
    {
        const juce::SpinLock::ScopedLockType sl( lock );
        recState.store( RecIdle );
        playing.store( false );
        audioBuffer.clear();
        recordedSamples.store( 0 );
        writePos.store( 0 );
        readPos.store( 0 );
        passSamples.store( 0 );
        progress = 0.0;
        hasUndo  = false;
    }

    void Recorder::takeUndoSnapshot()
    {
        // Caller holds the lock.
        undoBuffer.makeCopyOf( audioBuffer, true );
        undoSamples = recordedSamples.load();
        hasUndo     = true;
    }

    void Recorder::startRecording( bool FromBeginning, bool SkipThresholdDetection )
    {
        if( audioBuffer.getNumSamples() == 0 )
        {
            juce::Logger::writeToLog( "Recorder: startRecording called with no length set" );
            return;
        }

        {
            const juce::SpinLock::ScopedLockType sl( lock );
            if( recState.load() == RecIdle )
                takeUndoSnapshot(); // not again when the looper screen re-arms a running take

            if( loopMode )
            {
                // A stopped looper records from the top of the loop; a playing one punches in
                // at the play position and stays in time. The looper screen calls this twice
                // per key press (setRecord, then start): only the first one places the take.
                if( recState.load() == RecIdle )
                {
                    if( !playing.load() || FromBeginning )
                    {
                        readPos.store( 0 );
                        writePos.store( 0 );
                    }
                    else
                        writePos.store( readPos.load() );
                    passSamples.store( 0 );
                }
                // Overdub plays the loop while it adds to it. A replace take from a stopped
                // looper is silent until it has gone round; one punched in while the loop plays
                // keeps it playing (and so keeps the position running in time).
                if( overdub.load() )
                    playing.store( true );
            }
            else
            {
                if( FromBeginning )
                    writePos.store( 0 );
                if( !overdub.load() && FromBeginning )
                {
                    audioBuffer.clear();
                    recordedSamples.store( 0 );
                }
            }
        }

        const bool armOnly = !SkipThresholdDetection && threshold.load() > 0.f;
        const auto previous = recState.exchange( armOnly ? RecArmed : RecRecording );
        if( !armOnly && previous != RecRecording )
            notify( onRecorderStarted );
    }

    void Recorder::stopRecording()
    {
        if( recState.exchange( RecIdle ) == RecIdle )
            return;

        if( !loopMode )
        {
            // As AudIO::AudioRecorder::stop(): the take is what was recorded, so the
            // progress reads 1.0 and the waveform / saved file have no silent tail.
            const juce::SpinLock::ScopedLockType sl( lock );
            const int rec = recordedSamples.load();
            if( rec > 0 && rec < audioBuffer.getNumSamples() )
                audioBuffer.setSize( audioBuffer.getNumChannels(), rec, true, true, false );
            progress = rec > 0 ? 1.0 : 0.0;
        }

        notify( onRecorderStopped );
        notifyLength();
    }

    void Recorder::startPreview( double FromSeconds )
    {
        const int len = loopMode ? audioBuffer.getNumSamples() : recordedSamples.load();
        if( recordedSamples.load() == 0 || len == 0 )
            return;
        auto pos = (int)std::llround( juce::jmax( 0.0, FromSeconds ) * sampleRate );
        readPos.store( juce::jlimit( 0, juce::jmax( 0, len - 1 ), pos ) );
        if( !playing.exchange( true ) )
            notify( onRecorderPreviewStarted );
    }

    void Recorder::stopPreview()
    {
        if( playing.exchange( false ) )
            notify( onRecorderPreviewStopped );
    }

    AudIO::MeterData Recorder::readInputMeter()
    {
        return meter.getMeterIn( inMeterData );
    }

    AudIO::MeterData Recorder::readOutputMeter()
    {
        return meter.getMeterOut( outMeterData );
    }

    void Recorder::saveRecording( const std::string& FilePath )
    {
        saveToFile( juce::File( juce::String( FilePath ) ) );
    }

    bool Recorder::saveToFile( const juce::File& File )
    {
        const juce::SpinLock::ScopedLockType sl( lock );

        const int numSamples = loopMode ? ( recordedSamples.load() > 0 ? audioBuffer.getNumSamples() : 0 ) : recordedSamples.load();
        if( numSamples <= 0 )
        {
            juce::Logger::writeToLog( juce::String( getType() ) + ": nothing recorded, not saving " + File.getFullPathName() );
            return false;
        }

        File.deleteFile();
        std::unique_ptr<juce::FileOutputStream> stream( File.createOutputStream() );
        if( !stream || !stream->openedOk() )
        {
            juce::Logger::writeToLog( juce::String( getType() ) + ": cannot write " + File.getFullPathName() );
            return false;
        }

        // The three "OUT MONO" modes record the same signal on both channels; save them as
        // one channel, as AudIO::AudioRecorder did.
        const int                                numChannels = getRecordingMode() < L_STEREO ? 1 : audioBuffer.getNumChannels();
        juce::WavAudioFormat                     format;
        std::unique_ptr<juce::AudioFormatWriter> writer( format.createWriterFor( stream.get(), sampleRate, (unsigned int)numChannels, 24, {}, 0 ) );
        if( !writer )
            return false;
        stream.release(); // writer owns it now

        const bool ok = writer->writeFromAudioSampleBuffer( audioBuffer, 0, numSamples );
        juce::Logger::writeToLog( juce::String( getType() ) + ": saved " + File.getFullPathName() + " (" + juce::String( numSamples ) + " samples, " + juce::String( (double)numSamples / sampleRate, 3 ) + " s, " + juce::String( numChannels ) + " ch)" );
        return ok;
    }

    void Recorder::process( AudIO::Buffer<float>& Buffer )
    {
        const auto n = (int)Buffer.getBlockSize();
        Buffer.clearOutputs();

        if( !enabled.load() || n <= 0 )
            return;

        meter.processIn( Buffer );

        const float* inL  = Buffer.getInputBuffer( 0 );
        const float* inR  = Buffer.getInputBuffer( 1 );
        float*       outL = Buffer.getOutputBuffer( 0 );
        float*       outR = Buffer.getOutputBuffer( 1 );
        if( !inL ) inL = inR;
        if( !inR ) inR = inL;
        if( !outL || !outR )
            return;

        const juce::SpinLock::ScopedTryLockType stl( lock );
        if( stl.isLocked() )
        {
            const int   total   = audioBuffer.getNumSamples();
            const bool  forward = direction.load() == FORWARD;
            const bool  loop    = looping.load();
            const auto  recMode = (Mode)mode.load();
            const float thr     = threshold.load();
            const float g       = gain.load();
            // Constant-power pan normalised to unity in the centre (a take plays back at the
            // level it was recorded), limited to unity at the sides.
            const float p       = ( pan.load() + 1.f ) * 0.25f * juce::MathConstants<float>::pi;
            const float gL      = g * juce::jmin( 1.f, std::cos( p ) * juce::MathConstants<float>::sqrt2 );
            const float gR      = g * juce::jmin( 1.f, std::sin( p ) * juce::MathConstants<float>::sqrt2 );
            auto*       bufL    = audioBuffer.getWritePointer( 0 );
            auto*       bufR    = audioBuffer.getWritePointer( 1 );

            auto inputFrame = [&]( int i, float& l, float& r ) {
                switch( recMode )
                {
                    case L_MONO:
                    case L_STEREO: l = r = inL[i]; break;
                    case R_MONO:
                    case R_STEREO: l = r = inR[i]; break;
                    case LR_MONO: l = r = 0.5f * ( inL[i] + inR[i] ); break;
                    case LR_STEREO:
                    default:
                        l = inL[i];
                        r = inR[i];
                        break;
                }
            };

            // --- Threshold-armed start -------------------------------------------------
            int startAt = 0;
            if( recState.load() == RecArmed && inL )
            {
                startAt = n;
                for( int i = 0; i < n; ++i )
                {
                    if( std::abs( inL[i] ) >= thr || ( inR && std::abs( inR[i] ) >= thr ) )
                    {
                        recState.store( RecRecording );
                        notify( onRecorderStarted );
                        startAt = i;
                        break;
                    }
                }
            }

            if( loopMode )
            {
                // --- Looper: one position for recording and playback ----------------------
                const bool rec  = recState.load() == RecRecording && total > 0 && inL;
                const bool play = playing.load() && total > 0 && recordedSamples.load() > 0;
                if( ( rec || play ) && total > 0 )
                {
                    int pos  = readPos.load();
                    int pass = passSamples.load();
                    // A stopped looper armed by the threshold starts the loop where the signal
                    // crossed it.
                    for( int i = playing.load() ? 0 : startAt; i < n; ++i )
                    {
                        const int idx = forward ? pos : ( total - 1 - pos );
                        if( playing.load() )
                        {
                            outL[i] += bufL[idx] * gL;
                            outR[i] += bufR[idx] * gR;
                        }
                        if( rec && i >= startAt && recState.load() == RecRecording )
                        {
                            float l, r;
                            inputFrame( i, l, r );
                            if( overdub.load() )
                            {
                                bufL[pos] += l;
                                bufR[pos] += r;
                            }
                            else
                            {
                                bufL[pos] = l;
                                bufR[pos] = r;
                            }
                            if( recordedSamples.load() == 0 )
                                recordedSamples.store( total ); // the loop is the whole length from now on
                            if( ++pass >= total && !overdub.load() )
                            {
                                // A replace take has gone round once: play it, in time.
                                recState.store( RecIdle );
                                playing.store( true );
                                notify( onRecorderStopped );
                                notify( onRecorderPreviewStarted );
                            }
                        }
                        if( ++pos >= total )
                        {
                            pos = 0;
                            if( !loop && !( rec && recState.load() == RecRecording ) )
                            {
                                playing.store( false );
                                notify( onRecorderPreviewStopped );
                                break;
                            }
                        }
                    }
                    readPos.store( pos );
                    writePos.store( pos );
                    passSamples.store( pass );
                    progress = (double)pos / (double)total;
                }
            }
            else
            {
                // --- Sampler: record into the buffer ----------------------------------------
                if( recState.load() == RecRecording && total > 0 && inL )
                {
                    int wp  = writePos.load();
                    int rec = recordedSamples.load();
                    const bool add = overdub.load();

                    for( int i = startAt; i < n; ++i )
                    {
                        float l, r;
                        inputFrame( i, l, r );
                        if( add )
                        {
                            bufL[wp] += l;
                            bufR[wp] += r;
                        }
                        else
                        {
                            bufL[wp] = l;
                            bufR[wp] = r;
                        }

                        if( ++wp >= total )
                        {
                            rec = total;
                            if( loop )
                                wp = 0;
                            else
                            {
                                recState.store( RecIdle );
                                notify( onRecorderStopped );
                                break;
                            }
                        }
                    }
                    rec = juce::jmax( rec, wp );
                    writePos.store( wp );
                    recordedSamples.store( rec );
                    if( !playing.load() )
                        progress = (double)wp / (double)total;
                    if( recState.load() == RecIdle )
                    {
                        progress = 1.0;
                        notifyLength();
                    }
                }

                // --- Sampler: preview the take ----------------------------------------------
                const int recorded = juce::jmin( recordedSamples.load(), total );
                if( playing.load() && recorded > 0 )
                {
                    int rp = readPos.load();
                    for( int i = 0; i < n; ++i )
                    {
                        const int idx = forward ? rp : ( recorded - 1 - rp );
                        outL[i] += bufL[idx] * gL;
                        outR[i] += bufR[idx] * gR;

                        if( ++rp >= recorded )
                        {
                            rp = 0;
                            if( !loop )
                            {
                                playing.store( false );
                                notify( onRecorderPreviewStopped );
                                break;
                            }
                        }
                    }
                    readPos.store( rp );
                    progress = (double)rp / (double)recorded;
                }
            }
        }

        // --- Monitoring ---------------------------------------------------------------
        if( monitor.load() && inL )
        {
            const float mg = monitorGain.load();
            for( int i = 0; i < n; ++i )
            {
                outL[i] += inL[i] * mg;
                outR[i] += inR[i] * mg;
            }
        }

        processOutput( Buffer );
        meter.processOut( Buffer );
    }

    void Looper::undo()
    {
        const juce::SpinLock::ScopedLockType sl( lock );
        if( !hasUndo )
            return;
        std::swap( audioBuffer, undoBuffer );
        const int rec = recordedSamples.exchange( undoSamples );
        undoSamples   = rec;
        if( readPos.load() >= audioBuffer.getNumSamples() ) readPos.store( 0 );
        if( writePos.load() >= audioBuffer.getNumSamples() ) writePos.store( 0 );
        notifyLength();
    }
} // namespace mt
