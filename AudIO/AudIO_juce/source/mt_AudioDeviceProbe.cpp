/*
 * Copyright (c) 2022 MUSIC Tribe
 *
 * This file is part of AudIO.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

namespace mt
{
    namespace
    {
        juce::String toDb( double Linear )
        {
            return juce::String( juce::Decibels::gainToDecibels( Linear, -120.0 ), 1 ) + "dBFS";
        }
    } // namespace

    //==============================================================================
    std::unique_ptr<AudioProbe> AudioProbe::createFromEnvironment( double SampleRate, int NumChannels )
    {
        auto path = juce::SystemStats::getEnvironmentVariable( "KICK_AUDIO_PROBE", {} );
        if( path.isEmpty() )
            return nullptr;

        auto file = juce::File::getCurrentWorkingDirectory().getChildFile( path );
        return std::make_unique<AudioProbe>( file, SampleRate, NumChannels );
    }

    AudioProbe::AudioProbe( const juce::File& File, double SampleRate, int NumChannels )
        : juce::Thread( "AudioProbe" )
        , file( File )
        , sampleRate( SampleRate )
        , numChannels( juce::jmax( 1, NumChannels ) )
        , fifo( juce::nextPowerOfTwo( (int)SampleRate * 4 ) )
    {
        ring.setSize( numChannels, fifo.getTotalSize() );
        scratch.setSize( numChannels, fifo.getTotalSize() );

        file.deleteFile();
        file.getParentDirectory().createDirectory();
        if( auto stream = file.createOutputStream() )
        {
            juce::WavAudioFormat wav;
            writer.reset( wav.createWriterFor( stream.release(), sampleRate, (unsigned)numChannels, 32, {}, 0 ) );
        }

        if( writer )
        {
            juce::Logger::writeToLog( "AudioProbe: writing master output to " + file.getFullPathName() );
            startThread();
        }
        else
        {
            juce::Logger::writeToLog( "AudioProbe: cannot write " + file.getFullPathName() );
        }
    }

    AudioProbe::~AudioProbe()
    {
        stopThread( 2000 );
        drain();
        if( framesInSecond > 0 )
            logSecond();
        writer = nullptr; // finalises the WAV header
    }

    void AudioProbe::push( const float* const* Channels, int NumChannels, int NumSamples ) noexcept
    {
        if( !writer )
            return;

        const auto scope = fifo.write( NumSamples );
        if( scope.blockSize1 + scope.blockSize2 < NumSamples )
            dropped.fetch_add( NumSamples - ( scope.blockSize1 + scope.blockSize2 ) );

        for( int ch = 0; ch < numChannels; ++ch )
        {
            const auto* src = ch < NumChannels ? Channels[ch] : nullptr;
            auto*       dst = ring.getWritePointer( ch );
            if( scope.blockSize1 > 0 )
            {
                if( src ) juce::FloatVectorOperations::copy( dst + scope.startIndex1, src, scope.blockSize1 );
                else      juce::FloatVectorOperations::clear( dst + scope.startIndex1, scope.blockSize1 );
            }
            if( scope.blockSize2 > 0 )
            {
                if( src ) juce::FloatVectorOperations::copy( dst + scope.startIndex2, src + scope.blockSize1, scope.blockSize2 );
                else      juce::FloatVectorOperations::clear( dst + scope.startIndex2, scope.blockSize2 );
            }
        }
    }

    void AudioProbe::run()
    {
        while( !threadShouldExit() )
        {
            wait( 50 );
            drain();
        }
    }

    void AudioProbe::drain()
    {
        const auto ready = fifo.getNumReady();
        if( ready == 0 || !writer )
            return;

        const auto scope = fifo.read( ready );
        int        done  = 0;
        for( auto [start, size] : { std::pair{ scope.startIndex1, scope.blockSize1 }, std::pair{ scope.startIndex2, scope.blockSize2 } } )
        {
            for( int ch = 0; ch < numChannels; ++ch )
                scratch.copyFrom( ch, done, ring, ch, start, size );
            done += size;
        }

        // Statistics, split at whole seconds of audio.
        const auto framesPerSecond = (int64_t)sampleRate;
        for( int i = 0; i < done; ++i )
        {
            for( int ch = 0; ch < numChannels; ++ch )
            {
                const auto s = scratch.getSample( ch, i );
                peak         = juce::jmax( peak, std::abs( s ) );
                sumSquares += (double)s * s;
            }
            if( ++framesInSecond == framesPerSecond )
                logSecond();
        }

        writer->writeFromAudioSampleBuffer( scratch, 0, done );
        writer->flush(); // keeps the header valid even if the app is killed

        if( auto d = dropped.exchange( 0 ) )
            juce::Logger::writeToLog( "AudioProbe: dropped " + juce::String( d ) + " frames" );
    }

    void AudioProbe::logSecond()
    {
        const auto rms = std::sqrt( sumSquares / (double)juce::jmax<int64_t>( 1, framesInSecond * numChannels ) );
        juce::Logger::writeToLog( "AudioProbe: t=" + juce::String( ++secondsLogged ) + "s peak=" + toDb( peak ) + " rms=" + toDb( rms ) );
        framesInSecond = 0;
        peak           = 0.f;
        sumSquares     = 0.0;
    }

    //==============================================================================
    std::unique_ptr<TestInput> TestInput::createFromEnvironment( double SampleRate )
    {
        auto spec = juce::SystemStats::getEnvironmentVariable( "KICK_AUDIO_INPUT", {} ).trim();
        if( spec.isEmpty() )
            return nullptr;
        if( spec == "device" ) // explicit: the real device input, as when unset
        {
            juce::Logger::writeToLog( "TestInput: using the device input" );
            return nullptr;
        }

        std::unique_ptr<TestInput> in( new TestInput() );
        in->sampleRate = SampleRate;

        if( spec == "tone" || spec.startsWith( "tone:" ) )
        {
            in->kind = Kind::tone;
            if( spec.startsWith( "tone:" ) )
                in->frequency = juce::jlimit( 1.0, SampleRate * 0.45, spec.fromFirstOccurrenceOf( ":", false, false ).getDoubleValue() );
            juce::Logger::writeToLog( "TestInput: " + juce::String( in->frequency ) + " Hz sine at -12 dBFS replaces the device input" );
            return in;
        }

        if( spec == "silence" )
        {
            in->kind = Kind::silence;
            juce::Logger::writeToLog( "TestInput: silence replaces the device input" );
            return in;
        }

        if( spec == "pulse" || spec.startsWith( "pulse:" ) )
        {
            in->kind = Kind::pulse;
            if( spec.startsWith( "pulse:" ) )
                in->pulsePeriod = juce::jlimit( 0.06, 60.0, spec.fromFirstOccurrenceOf( ":", false, false ).getDoubleValue() / 1000.0 );
            juce::Logger::writeToLog( "TestInput: 1 kHz 50 ms pulse every " + juce::String( juce::roundToInt( in->pulsePeriod * 1000.0 ) ) + " ms replaces the device input" );
            return in;
        }

        auto file = juce::File::getCurrentWorkingDirectory().getChildFile( spec );
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader( formats.createReaderFor( file ) );
        if( !reader || reader->lengthInSamples <= 0 )
        {
            juce::Logger::writeToLog( "TestInput: cannot read " + file.getFullPathName() + ", input left alone" );
            return nullptr;
        }

        const int                srcLen = (int)reader->lengthInSamples;
        const int                numCh  = juce::jmax( 1, (int)reader->numChannels );
        juce::AudioBuffer<float> src( numCh, srcLen );
        reader->read( &src, 0, srcLen, 0, true, true );

        const double ratio  = reader->sampleRate / SampleRate;
        const int    dstLen = juce::jmax( 1, (int)std::floor( srcLen / ratio ) );
        in->data.setSize( numCh, dstLen );
        for( int ch = 0; ch < numCh; ++ch )
        {
            juce::LagrangeInterpolator interp;
            interp.process( ratio, src.getReadPointer( ch ), in->data.getWritePointer( ch ), dstLen );
        }

        in->kind = Kind::file;
        juce::Logger::writeToLog( "TestInput: looping " + file.getFullPathName() + " (" + juce::String( dstLen / SampleRate, 2 ) + " s) into the device input" );
        return in;
    }

    void TestInput::render( float* const* Channels, int NumChannels, int NumSamples ) noexcept
    {
        constexpr float toneGain  = 0.2511886f; // -12 dBFS
        constexpr float pulseGain = 0.5011872f; // -6 dBFS

        for( int i = 0; i < NumSamples; ++i )
        {
            float l = 0.f, r = 0.f;
            switch( kind )
            {
                case Kind::silence:
                    break;

                case Kind::tone:
                    l = r = toneGain * (float)std::sin( phase );
                    phase += juce::MathConstants<double>::twoPi * frequency / sampleRate;
                    if( phase > juce::MathConstants<double>::twoPi )
                        phase -= juce::MathConstants<double>::twoPi;
                    break;

                case Kind::pulse:
                {
                    const auto period = (int64_t)std::llround( sampleRate * pulsePeriod );
                    const auto inBeat = position % period;
                    if( inBeat < (int64_t)( sampleRate * 0.05 ) )
                        l = r = pulseGain * (float)std::sin( juce::MathConstants<double>::twoPi * 1000.0 * (double)inBeat / sampleRate );
                    break;
                }

                case Kind::file:
                {
                    const auto idx = (int)( position % data.getNumSamples() );
                    l              = data.getSample( 0, idx );
                    r              = data.getSample( juce::jmin( 1, data.getNumChannels() - 1 ), idx );
                    break;
                }
            }
            ++position;

            for( int ch = 0; ch < NumChannels; ++ch )
                if( auto* dst = Channels[ch] )
                    dst[i] = ( ch & 1 ) ? r : l;
        }
    }
} // namespace mt
