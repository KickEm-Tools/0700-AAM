/*
 * Copyright (c) 2025 MUSIC Tribe
 *
 * This file is part of Kick.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// kick_serialport_pty_test: juce_serialport against a pseudo-terminal pair. The test holds the
// master side and plays the device; SerialPort opens the slave path the way input_events opens
// /dev/ttymxc0 (230400 8N1, no flow control). Checks open and configuration, change
// notifications on the message thread, reads, writes (including one far larger than the pty
// buffer), close and reopen, and what happens when the other end hangs up.
//
// Exit status 0 when every check passes. POSIX only.

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_serialport/juce_serialport.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    int failures = 0;
    int checks   = 0;

    void expect( bool ok, const juce::String& what )
    {
        ++checks;
        if( !ok ) ++failures;
        std::printf( "  %-4s %s\n", ok ? "ok" : "FAIL", what.toRawUTF8() );
        std::fflush( stdout );
    }

    std::vector<uint8_t> readMaster( int master, size_t want, int timeoutMs )
    {
        std::vector<uint8_t> out;
        const auto           end = juce::Time::getMillisecondCounter() + (juce::uint32)timeoutMs;
        while( out.size() < want && juce::Time::getMillisecondCounter() < end )
        {
            pollfd p{ master, POLLIN, 0 };
            if( ::poll( &p, 1, 20 ) > 0 && ( p.revents & POLLIN ) )
            {
                uint8_t buf[4096];
                auto    n = ::read( master, buf, sizeof buf );
                if( n > 0 ) out.insert( out.end(), buf, buf + n );
            }
        }
        return out;
    }

    void writeMaster( int master, const std::vector<uint8_t>& b ) { [[maybe_unused]] auto n = ::write( master, b.data(), b.size() ); }

    void pump( int ms ) { juce::MessageManager::getInstance()->runDispatchLoopUntil( ms ); }

    bool pumpUntil( std::function<bool()> done, int timeoutMs )
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32)timeoutMs;
        while( !done() && juce::Time::getMillisecondCounter() < end )
            pump( 10 );
        return done();
    }

    struct Listener : juce::ChangeListener
    {
        SerialPortInputStream*            in = nullptr;
        std::vector<uint8_t>              got;
        int                               calls = 0;
        std::function<void( Listener& )> onCall;

        void changeListenerCallback( juce::ChangeBroadcaster* ) override
        {
            ++calls;
            expect( juce::MessageManager::existsAndIsCurrentThread(), "listener called on the message thread" );
            if( onCall )
                onCall( *this );
            else
                drain();
        }

        void drain()
        {
            const int n = (int)in->getTotalLength();
            if( n <= 0 ) return;
            std::vector<uint8_t> buf( (size_t)n );
            const int            r = in->read( buf.data(), n );
            got.insert( got.end(), buf.begin(), buf.begin() + r );
        }
    };

    int openMaster( juce::String& slavePath )
    {
        const int m = ::posix_openpt( O_RDWR | O_NOCTTY );
        if( m < 0 || ::grantpt( m ) != 0 || ::unlockpt( m ) != 0 )
            return -1;
        slavePath = ::ptsname( m );
        return m;
    }
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf( "== getSerialPortPaths()\n" );
    {
        const auto paths = SerialPort::getSerialPortPaths();
        for( const auto& k : paths.getAllKeys() )
            std::printf( "  %s -> %s\n", k.toRawUTF8(), paths[k].toRawUTF8() );
        std::printf( "  (%d ports)\n", paths.size() );
    }

    juce::String slavePath;
    int          master = openMaster( slavePath );
    expect( master >= 0, "posix_openpt: slave " + slavePath );
    if( master < 0 ) return 1;

    std::printf( "\n== open and configure (as input_events: 230400 8N1, no flow control)\n" );
    juce::StringArray debugLines;
    auto              debug = [&]( juce::String a, juce::String b ) {
        debugLines.add( a + ": " + b );
        std::printf( "  [debug] %s: %s\n", a.toRawUTF8(), b.toRawUTF8() );
    };
    const SerialPortConfig cfg( 230400, 8, SerialPortConfig::SERIALPORT_PARITY_NONE, SerialPortConfig::STOPBITS_1, SerialPortConfig::FLOWCONTROL_NONE );
    auto                   port = std::make_unique<SerialPort>( slavePath, cfg, debug );
    expect( port->isOpen(), "open " + slavePath + " (O_RDWR | O_NOCTTY)" );
    expect( port->exists(), "exists()" );
    {
        SerialPortConfig back;
        expect( port->getConfig( back ) && back.bps == 230400 && back.databits == 8 && back.parity == SerialPortConfig::SERIALPORT_PARITY_NONE && back.stopbits == SerialPortConfig::STOPBITS_1 && back.flowcontrol == SerialPortConfig::FLOWCONTROL_NONE,
            "getConfig() reads back " + back.toString() );
    }
    {
        // Other settings round-trip too; then back to the device's.
        const SerialPortConfig other( 115200, 7, SerialPortConfig::SERIALPORT_PARITY_EVEN, SerialPortConfig::STOPBITS_2, SerialPortConfig::FLOWCONTROL_NONE );
        SerialPortConfig       back;
        const bool             set = port->setConfig( other ) && port->getConfig( back );
       #if JUCE_LINUX
        // The Linux pty driver forces 8 data bits and no parity (pty_set_termios); a UART keeps them.
        expect( set && back.bps == 115200 && back.stopbits == SerialPortConfig::STOPBITS_2, "setConfig( 115200 7E2 ) reads back " + back.toString() + " (a Linux pty forces 8N)" );
       #else
        expect( set && back.bps == 115200 && back.databits == 7 && back.parity == SerialPortConfig::SERIALPORT_PARITY_EVEN && back.stopbits == SerialPortConfig::STOPBITS_2,
            "setConfig( 115200 7E2 ) reads back " + back.toString() );
       #endif
        expect( !port->setConfig( SerialPortConfig( 230400, 8, SerialPortConfig::SERIALPORT_PARITY_NONE, SerialPortConfig::STOPBITS_1ANDHALF, SerialPortConfig::FLOWCONTROL_NONE ) ), "1.5 stop bits refused" );
       #if JUCE_MAC
        {
            // A rate without a B constant goes through IOSSIOSPEED; a pty may or may not take it.
            const bool ok = port->setConfig( SerialPortConfig( 250000, 8, SerialPortConfig::SERIALPORT_PARITY_NONE, SerialPortConfig::STOPBITS_1, SerialPortConfig::FLOWCONTROL_NONE ) );
            SerialPortConfig b;
            port->getConfig( b );
            std::printf( "  info 250000 baud (no B constant, IOSSIOSPEED): %s, reads back %d\n", ok ? "accepted" : "refused", (int)b.bps );
        }
       #endif
        expect( port->setConfig( cfg ), "back to " + cfg.toString() );
    }

    std::printf( "\n== device -> app: change notifications and reads\n" );
    SerialPortInputStream in( port.get() );
    Listener              l;
    l.in = &in;
    in.addChangeListener( &l );
    in.setNotify( SerialPortInputStream::NOTIFY_ALWAYS );
    expect( in.isReading(), "reader thread running" );

    // BUTTON PLAY pressed, with its CRC and the trailing byte, as the surface sends it.
    const std::vector<uint8_t> frame = [] {
        std::vector<uint8_t> f   = { 'M', 'T', 1, 0, 0, 3, 45, 1 };
        uint8_t              crc = 0xFF;
        for( auto b : f )
        {
            crc ^= b;
            for( int j = 0; j < 8; ++j )
                crc = ( crc & 0x80 ) ? (uint8_t)( ( crc << 1 ) ^ 0x31 ) : (uint8_t)( crc << 1 );
        }
        f.push_back( crc );
        f.push_back( '\n' );
        return f;
    }();
    writeMaster( master, frame );
    expect( pumpUntil( [&] { return l.got.size() >= frame.size(); }, 2000 ) && l.got == frame, "a 10-byte frame arrives intact through the listener (" + juce::String( l.calls ) + " callback(s))" );

    {
        // While a listener runs, the length it sees does not grow; the newer bytes come in the
        // next callback.
        l.got.clear();
        l.calls = 0;
        juce::int64 first = -1, afterSleep = -1;
        l.onCall          = [&]( Listener& self ) {
            if( first < 0 )
            {
                first = self.in->getTotalLength();
                writeMaster( master, { 'b', 'b', 'b' } );
                juce::Thread::sleep( 100 );
                afterSleep = self.in->getTotalLength();
            }
            self.drain();
        };
        writeMaster( master, { 'a', 'a', 'a', 'a' } );
        pumpUntil( [&] { return l.got.size() >= 7; }, 2000 );
        expect( first == 4 && afterSleep == 4, "length seen inside a callback is fixed when it starts (4, still " + juce::String( afterSleep ) + " after 3 more bytes arrived)" );
        expect( l.got == std::vector<uint8_t>{ 'a', 'a', 'a', 'a', 'b', 'b', 'b' } && l.calls >= 2, "the 3 later bytes arrive in a later callback (" + juce::String( l.calls ) + " callbacks)" );
        l.onCall = nullptr;
    }

    {
        // Polling without a listener sees bytes as soon as they arrive.
        in.setNotify( SerialPortInputStream::NOTIFY_OFF );
        writeMaster( master, { 1, 2, 3 } );
        juce::Thread::sleep( 100 );
        uint8_t buf[8];
        expect( in.getTotalLength() == 3 && in.read( buf, 8 ) == 3 && buf[2] == 3 && in.isExhausted(), "NOTIFY_OFF: polled read of 3 bytes" );
    }

    {
        l.got.clear();
        l.calls = 0;
        in.setNotify( SerialPortInputStream::NOTIFY_ON_CHAR, '\n' );
        writeMaster( master, { 'x', 'y' } );
        pump( 200 );
        const bool quiet = l.calls == 0;
        writeMaster( master, { '\n' } );
        pumpUntil( [&] { return l.calls > 0; }, 2000 );
        expect( quiet && l.got == std::vector<uint8_t>{ 'x', 'y', '\n' }, "NOTIFY_ON_CHAR '\\n': no callback for \"xy\", one for \"\\n\" with all 3 bytes" );
        in.setNotify( SerialPortInputStream::NOTIFY_ALWAYS );
    }

    std::printf( "\n== app -> device: writes\n" );
    SerialPortOutputStream out( port.get() );
    {
        std::vector<uint8_t> pads( 40 );
        for( size_t i = 0; i < pads.size(); ++i ) pads[i] = (uint8_t)i;
        expect( out.write( pads.data(), pads.size() ), "write 40 bytes" );
        out.flush();
        expect( readMaster( master, pads.size(), 2000 ) == pads, "the device reads the same 40 bytes" );
    }
    {
        // Larger than the pty buffer: write() has to wait for the reader.
        std::vector<uint8_t> big( 256 * 1024 );
        for( size_t i = 0; i < big.size(); ++i ) big[i] = (uint8_t)( i * 7 );
        std::vector<uint8_t> got;
        std::thread          reader( [&] { got = readMaster( master, big.size(), 10000 ); } );
        const bool           ok = out.write( big.data(), big.size() );
        reader.join();
        expect( ok && got == big, "write 256 KiB while the device reads: all of it, in order" );
        expect( out.getPosition() == (juce::int64)( 40 + big.size() ), "getPosition() counts written bytes" );
    }

    std::printf( "\n== close and reopen\n" );
    port->close();
    expect( !port->isOpen() && !in.isReading(), "close(): port closed, reader thread stopped" );
    expect( !out.write( "x", 1 ), "write after close fails" );
    expect( port->open( slavePath ) && port->setConfig( cfg ) && in.isReading(), "reopen: the attached input stream reads again" );
    l.got.clear();
    writeMaster( master, frame );
    expect( pumpUntil( [&] { return l.got.size() >= frame.size(); }, 2000 ) && l.got == frame, "a frame after reopening" );

    std::printf( "\n== the device end hangs up\n" );
    debugLines.clear();
    ::close( master );
    const bool stopped = pumpUntil( [&] { return !in.isReading(); }, 3000 );
    expect( stopped, "reader stops when the other end closes: " + debugLines.joinIntoString( " | " ) );
    expect( port->isOpen(), "the port stays open until the app closes it (input_events decides about reconnecting)" );

    in.removeChangeListener( &l );
    port.reset();
    expect( true, "port destroyed with an input stream still attached" );

    std::printf( "\n%d checks, %d failed: %s\n", checks, failures, failures ? "FAIL" : "PASS" );
    return failures ? 1 : 0;
}
