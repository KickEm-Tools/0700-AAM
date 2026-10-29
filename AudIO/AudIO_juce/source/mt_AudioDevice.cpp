/*
 * Copyright (c) 2022 MUSIC Tribe
 *
 * This file is part of AudIO.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

namespace mt
{
    AudioDevice::AudioDevice( uint32_t NumInputs, uint32_t NumOutputs, const juce::String& PreferredDeviceName )
        : AudIO::IAudioDevice( NumInputs, NumOutputs )
        , preferredName( PreferredDeviceName )
        , numIns( NumInputs )
        , numOuts( NumOutputs )
    {
    }

    AudioDevice::~AudioDevice()
    {
        stop();
    }

    void AudioDevice::allocateFifos( int DeviceBufferSize )
    {
        // Enough room for several device callbacks plus several AudIO blocks.
        ringSize = juce::nextPowerOfTwo( 4 * juce::jmax( (int)chunk, DeviceBufferSize ) + 4096 );
        inRing.setSize( juce::jmax( 1, (int)numIns ), ringSize, false, true, true );
        outRing.setSize( juce::jmax( 1, (int)numOuts ), ringSize, false, true, true );
        inRing.clear();
        outRing.clear();
        inWrite = inAvail = 0;
        outWrite = outRead = outAvail = 0;
    }

    void AudioDevice::start( uint32_t samplerate, uint32_t blocksize )
    {
        if( running.load() )
            return;

        chunk = juce::jmin( blocksize, _buffer.getBlockSize() );
        if( chunk == 0 )
            chunk = _buffer.getBlockSize();

        auto error = deviceManager.initialiseWithDefaultDevices( (int)numIns, (int)numOuts );
        if( error.isNotEmpty() )
            juce::Logger::writeToLog( "AudioDevice: " + error );

        // Ask the host device for our sample rate and block size; it may not honour either.
        auto setup       = deviceManager.getAudioDeviceSetup();
        setup.sampleRate = (double)samplerate;
        setup.bufferSize = (int)chunk;
        if( preferredName.isNotEmpty() )
            setup.outputDeviceName = preferredName;
        error = deviceManager.setAudioDeviceSetup( setup, true );
        if( error.isNotEmpty() )
            juce::Logger::writeToLog( "AudioDevice: " + error );

        double actualRate   = (double)samplerate;
        int    actualBuffer = (int)chunk;
        if( auto* dev = deviceManager.getCurrentAudioDevice() )
        {
            actualRate   = dev->getCurrentSampleRate() > 0 ? dev->getCurrentSampleRate() : actualRate;
            actualBuffer = dev->getCurrentBufferSizeSamples() > 0 ? dev->getCurrentBufferSizeSamples() : actualBuffer;
            juce::Logger::writeToLog( "AudioDevice: using '" + dev->getName() + "' at " + juce::String( actualRate ) + " Hz, device buffer " + juce::String( actualBuffer ) + ", engine block " + juce::String( (int)chunk ) );
        }
        else
        {
            juce::Logger::writeToLog( "AudioDevice: no audio device available, engine will run silent" );
        }

        allocateFifos( actualBuffer );
        probe     = AudioProbe::createFromEnvironment( actualRate, (int)numOuts );
        testInput = TestInput::createFromEnvironment( actualRate );
        prepareToPlay( (uint32_t)actualRate, chunk );

        running.store( true );
        setState( AudIO::AudioDeviceState::playing );
        deviceManager.addAudioCallback( this );
        callbackAdded = true;
    }

    void AudioDevice::resume()
    {
        if( !callbackAdded && running.load() )
        {
            deviceManager.addAudioCallback( this );
            callbackAdded = true;
        }
        setState( AudIO::AudioDeviceState::playing );
    }

    void AudioDevice::pause()
    {
        if( callbackAdded )
        {
            deviceManager.removeAudioCallback( this );
            callbackAdded = false;
        }
        setState( AudIO::AudioDeviceState::paused );
    }

    void AudioDevice::stop()
    {
        if( !running.load() )
            return;

        pause();
        deviceManager.closeAudioDevice();
        _buffer.clearOutputs();
        running.store( false );
        setState( AudIO::AudioDeviceState::stopped );
        teardown();
        probe     = nullptr;
        testInput = nullptr;
    }

    void AudioDevice::audioDeviceAboutToStart( juce::AudioIODevice* ) {}
    void AudioDevice::audioDeviceStopped() {}
    void AudioDevice::audioDeviceError( const juce::String& errorMessage ) { juce::Logger::writeToLog( "AudioDevice error: " + errorMessage ); }

    void AudioDevice::audioDeviceIOCallbackWithContext( const float* const* inputChannelData, int numInputChannels, float* const* outputChannelData, int numOutputChannels, int numSamples, const juce::AudioIODeviceCallbackContext& )
    {
        const auto mask = ringSize - 1;

        if( !running.load() || ringSize == 0 || numSamples > ringSize )
        {
            for( int ch = 0; ch < numOutputChannels; ++ch )
                if( auto* out = outputChannelData[ch] )
                    juce::FloatVectorOperations::clear( out, numSamples );
            return;
        }

        // 1. Push device input into the input ring.
        for( int ch = 0; ch < (int)numIns; ++ch )
        {
            auto*       ring = inRing.getWritePointer( ch );
            const auto* src  = ch < numInputChannels ? inputChannelData[ch] : nullptr;
            for( int i = 0; i < numSamples; ++i )
                ring[( inWrite + i ) & mask] = src ? src[i] : 0.f;
        }
        inWrite = ( inWrite + numSamples ) & mask;
        inAvail += numSamples;

        // 2. Run AudIO for every complete block we have input for.
        while( inAvail >= (int)chunk && ( ringSize - outAvail ) >= (int)chunk )
        {
            const int readStart = ( inWrite - inAvail ) & mask;

            for( uint32_t ch = 0; ch < _buffer.getNumInputChannels(); ++ch )
            {
                if( auto* dst = _buffer.getInputBuffer( ch ) )
                {
                    if( ch < numIns )
                    {
                        const auto* ring = inRing.getReadPointer( (int)ch );
                        for( uint32_t i = 0; i < chunk; ++i )
                            dst[i] = ring[( readStart + (int)i ) & mask];
                    }
                    else
                    {
                        juce::FloatVectorOperations::clear( dst, (int)chunk );
                    }
                }
            }
            inAvail -= (int)chunk;

            if( testInput )
            {
                float* ins[8] = {};
                const auto n  = (int)juce::jmin<uint32_t>( 8, _buffer.getNumInputChannels() );
                for( int ch = 0; ch < n; ++ch )
                    ins[ch] = _buffer.getInputBuffer( (uint32_t)ch );
                testInput->render( ins, n, (int)chunk );
            }

            _buffer.clearOutputs();
            process( _buffer ); // IAudioDevice -> engine callback

            if( probe )
            {
                float* outs[8] = {};
                const auto n   = (int)juce::jmin<uint32_t>( 8, _buffer.getNumOutputChannels() );
                for( int ch = 0; ch < n; ++ch )
                    outs[ch] = _buffer.getOutputBuffer( (uint32_t)ch );
                probe->push( outs, n, (int)chunk );
            }

            for( uint32_t ch = 0; ch < numOuts; ++ch )
            {
                auto*       ring = outRing.getWritePointer( (int)ch );
                const auto* src  = ch < _buffer.getNumOutputChannels() ? _buffer.getOutputBuffer( ch ) : nullptr;
                for( uint32_t i = 0; i < chunk; ++i )
                    ring[( outWrite + (int)i ) & mask] = src ? src[i] : 0.f;
            }
            outWrite = ( outWrite + (int)chunk ) & mask;
            outAvail += (int)chunk;
        }

        // 3. Pop processed audio to the device (silence if we are short, which only
        //    happens on the first callback when sizes differ).
        const int available = juce::jmin( outAvail, numSamples );
        for( int ch = 0; ch < numOutputChannels; ++ch )
        {
            auto* out = outputChannelData[ch];
            if( !out ) continue;

            if( ch < (int)numOuts )
            {
                const auto* ring = outRing.getReadPointer( ch );
                for( int i = 0; i < available; ++i )
                    out[i] = ring[( outRead + i ) & mask];
                for( int i = available; i < numSamples; ++i )
                    out[i] = 0.f;
            }
            else
            {
                juce::FloatVectorOperations::clear( out, numSamples );
            }
        }
        outRead = ( outRead + available ) & mask;
        outAvail -= available;
    }
} // namespace mt
