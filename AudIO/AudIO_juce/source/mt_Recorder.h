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
    /// An AudIO DAW object that records its input bus into a juce::AudioBuffer and can play
    /// it back. Base of SampleRecorder (sampler screen) and Looper (looper screen).
    ///
    /// Reconstructed against Kick's MTAudioRecorderManager and MTLooperInterface.
    ///
    /// Threading: the audio thread runs process(); the UI thread calls everything else.
    /// Buffer (re)allocation and saving take a spin lock that process() only try-locks,
    /// so a resize can at worst drop one block of audio.
    ///
    /// Semantics follow the public AudIO::AudioRecorder (AudIO/Objects/Recorder.cpp) where it
    /// has the same API: onRecorderLengthChanged fires when the length is set and, with the
    /// recorded length, when recording stops; a sampler recording stopped early is truncated
    /// to what was recorded, so the progress reads 1.0 afterwards. Recording always runs
    /// forward; Direction is the playback direction (the looper's reverse button).
    class Recorder : public AudIO::Object,
                     public AudIO::DawRouting
    {
    public:
        enum Direction
        {
            FORWARD = 0,
            REVERSE
        };

        enum Mode
        {
            L_MONO = 0, // left input, mono recording
            R_MONO, // right input, mono recording
            LR_MONO, // (L+R)/2, mono recording
            L_STEREO, // left input to both channels
            R_STEREO, // right input to both channels
            LR_STEREO // true stereo
        };

        explicit Recorder( AudIO::Design& D, AudIO::BusType Type = AudIO::BusType::Object );
        ~Recorder() override;

        const char* getType() const override { return "Recorder"; }

        // Notifications, delivered on the message thread.
        std::function<void()>                       onRecorderStarted;
        std::function<void()>                       onRecorderStopped;
        std::function<void()>                       onRecorderPaused;
        std::function<void()>                       onRecorderResumed;
        std::function<void()>                       onRecorderPreviewStarted;
        std::function<void()>                       onRecorderPreviewStopped;
        std::function<void( double TimeInSeconds )> onRecorderLengthChanged;

        // Configuration...
        void setEnable( bool Enable ) noexcept { enabled.store( Enable ); }
        bool isEnabled() const noexcept { return enabled.load(); }

        void   setLength( double TimeInSeconds );
        float  getLength() const noexcept { return (float)lengthSeconds; }
        double getLengthInSeconds() const noexcept { return lengthSeconds; }

        void setMonitorOn( bool Monitor ) noexcept { monitor.store( Monitor ); }
        bool isMonitorOn() const noexcept { return monitor.load(); }
        void  setMonitorGain( float Gain ) noexcept { monitorGain.store( juce::jlimit( 0.f, 2.f, Gain ) ); }
        float getMonitorGain() const noexcept { return monitorGain.load(); }

        void setOverdub( bool Overdub ) noexcept { overdub.store( Overdub ); }
        bool isOverdubbing() const noexcept { return overdub.load(); }

        void setLooping( bool Loop ) noexcept { looping.store( Loop ); }
        bool isLooping() const noexcept { return looping.load(); }

        void  setGain( float Gain ) noexcept { gain.store( juce::jlimit( 0.f, 2.f, Gain ) ); }
        void  setGainDB( float GainDB ) noexcept { setGain( AudIO::utils::logToLinear( GainDB ) ); }
        float getGain() const noexcept { return gain.load(); }
        void  setPan( float Pan ) noexcept { pan.store( juce::jlimit( -1.f, 1.f, Pan ) ); }
        float getPan() const noexcept { return pan.load(); }
        void  setGainAndPan( float Gain, float Pan, bool /*Smooth*/ = true ) noexcept
        {
            setGain( Gain );
            setPan( Pan );
        }

        void  setThreshold( float Threshold ) noexcept { threshold.store( juce::jlimit( 0.f, 1.f, Threshold ) ); }
        void  setThresholdDB( float DB ) noexcept { setThreshold( AudIO::utils::logToLinear( DB ) ); }
        float getThreshold() const noexcept { return threshold.load(); }
        float getThresholdDB() const noexcept { return AudIO::utils::linearTolog( threshold.load() ); }

        void      setDirection( Direction Dir ) noexcept { direction.store( (int)Dir ); }
        void      setDirection( bool Forward ) noexcept { setDirection( Forward ? FORWARD : REVERSE ); }
        Direction getDirection() const noexcept { return (Direction)direction.load(); }
        bool      isForward() const noexcept { return getDirection() == FORWARD; }
        bool      isReverse() const noexcept { return getDirection() == REVERSE; }

        void setRecordingMode( Mode M ) noexcept { mode.store( (int)M ); }
        Mode getRecordingMode() const noexcept { return (Mode)mode.load(); }

        // Transport...
        virtual void reset(); // Clears the recording. Keeps length and settings.

        void startRecording( bool FromBeginning = true, bool SkipThresholdDetection = false );
        void stopRecording();
        bool isRecording() const noexcept { return recState.load() != RecIdle; }
        bool isArmed() const noexcept { return recState.load() == RecArmed; }

        void startPreview( double FromSeconds = 0.0 );
        void stopPreview();
        bool isPreviewing() const noexcept { return playing.load(); }

        bool isEmpty() const noexcept { return recordedSamples.load() == 0; }
        bool isRunning() const noexcept { return isRecording() || isPreviewing(); }

        // Data...
        juce::AudioBuffer<float>& getAudioBuffer() noexcept { return audioBuffer; }
        int                       getRecordedSamples() const noexcept { return recordedSamples.load(); }
        float                     readProgress() const noexcept { return (float)progress; }
        double&                   getProgress() noexcept { return progress; }
        float                     getSampleRate() const noexcept { return (float)sampleRate; }

        AudIO::MeterData readInputMeter();
        AudIO::MeterData readOutputMeter();

        void saveRecording( const std::string& FilePath );
        bool saveToFile( const juce::File& File );

    protected:
        // AudIO::Object
        void prepareToPlay( uint32_t Samplerate, uint32_t Blocksize ) override;
        void process( AudIO::Buffer<float>& Buffer ) override;
        void teardown() override;

        /// Hook for subclasses to run inserts on the mixed output (Looper).
        virtual void processOutput( AudIO::Buffer<float>& ) {}

        /// Looper behaviour: the buffer length is the loop (N bars), playback and recording
        /// share one position and wrap at the loop length, a replace recording plays the loop
        /// once it has gone round, and overdub adds to the loop while it plays.
        bool loopMode{ false };

        void notify( const std::function<void()>& Fn );
        void notifyLength();
        void takeUndoSnapshot();

        enum RecordState
        {
            RecIdle = 0,
            RecArmed,
            RecRecording
        };

        juce::SpinLock           lock;
        juce::AudioBuffer<float> audioBuffer; // 2 channels, lengthSeconds * sampleRate samples
        juce::AudioBuffer<float> undoBuffer;
        int                      undoSamples{ 0 };
        bool                     hasUndo{ false };

        double sampleRate{ (double)AUDIO_SAMPLE_RATE };
        double lengthSeconds{ 0.0 };
        double progress{ 0.0 };

        std::atomic<bool>  enabled{ false }, monitor{ false }, overdub{ false }, looping{ false }, playing{ false };
        std::atomic<float> gain{ 1.f }, pan{ 0.f }, monitorGain{ 1.f }, threshold{ 0.f };
        std::atomic<int>   direction{ FORWARD }, mode{ LR_STEREO }, recState{ RecIdle };
        std::atomic<int>   writePos{ 0 }, readPos{ 0 }, recordedSamples{ 0 };
        std::atomic<int>   passSamples{ 0 }; // looper: samples recorded since the last startRecording()

        AudIO::LevelMeter meter;
        AudIO::MeterData  inMeterData, outMeterData;

        JUCE_DECLARE_NON_COPYABLE( Recorder )
    };

    /// Recorder used by the sampler screen.
    class SampleRecorder : public Recorder
    {
    public:
        explicit SampleRecorder( AudIO::Design& D )
            : Recorder( D ) {}
        const char* getType() const override { return "SampleRecorder"; }
    };

    /// Recorder with an insert-effects chain, used by the looper screen.
    class Looper : public Recorder,
                   public AudIO::DawInserts
    {
    public:
        explicit Looper( AudIO::Design& D )
            : Recorder( D )
        {
            loopMode = true;
            looping.store( true ); // a loop plays round until stopped
        }
        const char* getType() const override { return "Looper"; }

        /// RECORD (replace). The screen sets the overdub flag only for OVERDUB, so a plain
        /// record clears it here.
        void setRecord( bool Record )
        {
            if( Record )
            {
                setOverdub( false );
                startRecording( false, true );
            }
            else
                stopRecording();
        }
        /// PLAY on the looper screen: punch out of any recording and keep the loop playing in
        /// time (from the top if it was stopped).
        void setPlay()
        {
            stopRecording();
            setOverdub( false );
            if( !isPreviewing() )
                startPreview( 0.0 );
        }

        void start( bool FromBeginning, bool SkipThresholdDetection ) { startRecording( FromBeginning, SkipThresholdDetection ); }
        /// PLAY / PLAY START keys on the looper screen: same as setPlay(); a stopped looper
        /// starts at FromSeconds. A running loop is not restarted, so it stays in time with
        /// the sequencer the screen may have started alongside it.
        void play( double FromSeconds )
        {
            stopRecording();
            setOverdub( false );
            if( !isPreviewing() )
                startPreview( FromSeconds );
        }
        void stop() override
        {
            stopRecording();
            stopPreview();
            setOverdub( false );
        }
        void clear() { reset(); }
        void undo();
        void redo() { undo(); } // Single-level: redo swaps the same two buffers back.
        void save( const juce::String& FilePath ) { saveRecording( FilePath.toStdString() ); }

    protected:
        void prepareToPlay( uint32_t Samplerate, uint32_t Blocksize ) override
        {
            Recorder::prepareToPlay( Samplerate, Blocksize );
            AudIO::DawInserts::prepareToPlay( Samplerate, Blocksize );
        }
        void processOutput( AudIO::Buffer<float>& Buffer ) override { AudIO::DawInserts::process( Buffer ); }
    };
} // namespace mt
