/*
 * Copyright (c) 2025 MUSIC Tribe
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

#include "juce_serialport.h"

#if JUCE_SERIALPORT_POSIX
    #include <cerrno>
    #include <cstring>
    #include <dirent.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/ioctl.h>
    #include <sys/stat.h>
    #include <termios.h>
    #include <unistd.h>
    #if JUCE_MAC || defined( __APPLE__ )
        #include <IOKit/serial/ioss.h> // IOSSIOSPEED, for rates without a B constant
    #endif
#endif

#include <algorithm>

//==============================================================================
juce::String SerialPortConfig::toString() const
{
    static const char parityChar[] = { 'N', 'O', 'E', 'M', 'S' };
    static const char* stopText[]  = { "1", "1.5", "2" };
    static const char* flowText[]  = { "no flow control", "XON/XOFF", "RTS/CTS" };

    juce::String s;
    s << (int)bps << " " << (int)databits << parityChar[juce::jlimit( 0, 4, (int)parity )] << stopText[juce::jlimit( 0, 2, (int)stopbits )] << ", " << flowText[juce::jlimit( 0, 2, (int)flowcontrol )];
    return s;
}

//==============================================================================
void SerialPort::report( const juce::String& context, const juce::String& message ) const
{
    if( debugFn )
        debugFn( context, message );
    else
        juce::Logger::writeToLog( "SerialPort: " + context + ": " + message );
}

SerialPort::SerialPort( DebugFunction debug )
    : debugFn( std::move( debug ) )
{
}

SerialPort::SerialPort( const juce::String& newPortPath, const SerialPortConfig& portConfig, DebugFunction debug )
    : debugFn( std::move( debug ) )
{
    if( open( newPortPath ) )
        setConfig( portConfig );
}

SerialPort::~SerialPort()
{
    close();

    const juce::ScopedLock sl( streamsLock );
    for( auto* s : streams )
        s->port = nullptr;
    streams.clear();
}

void SerialPort::attach( SerialPortInputStream* s )
{
    const juce::ScopedLock sl( streamsLock );
    streams.addIfNotAlreadyThere( s );
}

void SerialPort::detach( SerialPortInputStream* s )
{
    const juce::ScopedLock sl( streamsLock );
    streams.removeFirstMatchingValue( s );
}

bool SerialPort::exists() const
{
    return portPath.isNotEmpty() && juce::File::isAbsolutePath( portPath ) && juce::File( portPath ).exists();
}

bool SerialPort::isOpen() const { return fd.load() >= 0; }

#if JUCE_SERIALPORT_POSIX

namespace
{
    struct SpeedEntry
    {
        juce::uint32 bps;
        speed_t      constant;
    };

    // Every rate termios names on this system.
    const SpeedEntry speedTable[] = {
        { 50, B50 }, { 75, B75 }, { 110, B110 }, { 134, B134 }, { 150, B150 }, { 200, B200 }, { 300, B300 }, { 600, B600 },
        { 1200, B1200 }, { 1800, B1800 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 }, { 19200, B19200 }, { 38400, B38400 },
    #ifdef B57600
        { 57600, B57600 },
    #endif
    #ifdef B115200
        { 115200, B115200 },
    #endif
    #ifdef B230400
        { 230400, B230400 },
    #endif
    #ifdef B460800
        { 460800, B460800 },
    #endif
    #ifdef B500000
        { 500000, B500000 },
    #endif
    #ifdef B576000
        { 576000, B576000 },
    #endif
    #ifdef B921600
        { 921600, B921600 },
    #endif
    #ifdef B1000000
        { 1000000, B1000000 },
    #endif
    #ifdef B1152000
        { 1152000, B1152000 },
    #endif
    #ifdef B1500000
        { 1500000, B1500000 },
    #endif
    #ifdef B2000000
        { 2000000, B2000000 },
    #endif
    #ifdef B2500000
        { 2500000, B2500000 },
    #endif
    #ifdef B3000000
        { 3000000, B3000000 },
    #endif
    #ifdef B3500000
        { 3500000, B3500000 },
    #endif
    #ifdef B4000000
        { 4000000, B4000000 },
    #endif
    };

    bool speedConstantFor( juce::uint32 bps, speed_t& out )
    {
        for( const auto& e : speedTable )
            if( e.bps == bps )
            {
                out = e.constant;
                return true;
            }
        return false;
    }

    juce::uint32 bpsForSpeedConstant( speed_t s )
    {
        for( const auto& e : speedTable )
            if( e.constant == s )
                return e.bps;
    #if JUCE_MAC || defined( __APPLE__ )
        return (juce::uint32)s; // Darwin's speed_t is the rate itself
    #else
        return 0;
    #endif
    }

    juce::String errnoText() { return juce::String( std::strerror( errno ) ) + " (errno " + juce::String( errno ) + ")"; }
}

juce::StringPairArray SerialPort::getSerialPortPaths()
{
    #if JUCE_MAC || defined( __APPLE__ )
    static const char* prefixes[] = { "cu.", "tty." };
    #else
    static const char* prefixes[] = { "ttyS", "ttyUSB", "ttyACM", "ttyAMA", "ttymxc", "ttyLP", "rfcomm" };
    #endif

    juce::StringArray names;
    if( DIR* dir = ::opendir( "/dev" ) )
    {
        while( auto* entry = ::readdir( dir ) )
        {
            const juce::String name( entry->d_name );
            for( const char* p : prefixes )
                if( name.startsWith( p ) && name.length() > (int)std::strlen( p ) )
                {
                    names.add( name );
                    break;
                }
        }
        ::closedir( dir );
    }
    names.sortNatural();

    juce::StringPairArray result;
    for( const auto& n : names )
        result.set( n, "/dev/" + n );
    return result;
}

bool SerialPort::open( const juce::String& newPortPath )
{
    close();
    portPath = newPortPath;

    // Non-blocking: open() must not wait for carrier detect, and the reader and writer wait
    // with poll() instead of blocking in read() / write().
    const int f = ::open( newPortPath.toRawUTF8(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC );
    if( f < 0 )
    {
        report( "SerialPort open", newPortPath + ": " + errnoText() );
        return false;
    }

    termios t{};
    if( ::tcgetattr( f, &t ) != 0 )
    {
        report( "SerialPort open", newPortPath + " is not a terminal device: " + errnoText() );
        ::close( f );
        return false;
    }

    // Raw: no line editing, no echo, no signals, no CR/LF translation, 8-bit clean. Ignore
    // the modem control lines and enable the receiver. read() returns what is there.
    ::cfmakeraw( &t );
    t.c_cflag |= ( CLOCAL | CREAD );
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    if( ::tcsetattr( f, TCSANOW, &t ) != 0 )
    {
        report( "SerialPort open", newPortPath + ": raw mode refused: " + errnoText() );
        ::close( f );
        return false;
    }

    // Drop whatever the driver buffered before we opened it.
    ::tcflush( f, TCIOFLUSH );

    fd.store( f );

    const juce::ScopedLock sl( streamsLock );
    for( auto* s : streams )
        s->startThread();
    return true;
}

void SerialPort::close()
{
    {
        const juce::ScopedLock sl( streamsLock );
        for( auto* s : streams )
            s->stopReading();
    }

    const juce::ScopedLock wl( writeLock );
    const int              f = fd.exchange( -1 );
    if( f >= 0 )
        ::close( f );
}

bool SerialPort::setConfig( const SerialPortConfig& c )
{
    const int f = fd.load();
    if( f < 0 )
        return false;

    termios t{};
    if( ::tcgetattr( f, &t ) != 0 )
    {
        report( "SerialPort config", errnoText() );
        return false;
    }
    const termios before = t;

    ::cfmakeraw( &t );
    t.c_cflag |= ( CLOCAL | CREAD );
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;

    // Data bits.
    t.c_cflag &= ~CSIZE;
    switch( c.databits )
    {
        case 5: t.c_cflag |= CS5; break;
        case 6: t.c_cflag |= CS6; break;
        case 7: t.c_cflag |= CS7; break;
        case 8: t.c_cflag |= CS8; break;
        default: report( "SerialPort config", juce::String( (int)c.databits ) + " data bits are not supported" ); return false;
    }

    // Parity.
    t.c_cflag &= ~( PARENB | PARODD );
    #ifdef CMSPAR
    t.c_cflag &= ~CMSPAR;
    #endif
    switch( c.parity )
    {
        case SerialPortConfig::SERIALPORT_PARITY_NONE: break;
        case SerialPortConfig::SERIALPORT_PARITY_ODD: t.c_cflag |= ( PARENB | PARODD ); break;
        case SerialPortConfig::SERIALPORT_PARITY_EVEN: t.c_cflag |= PARENB; break;
    #ifdef CMSPAR
        case SerialPortConfig::SERIALPORT_PARITY_MARK: t.c_cflag |= ( PARENB | CMSPAR | PARODD ); break;
        case SerialPortConfig::SERIALPORT_PARITY_SPACE: t.c_cflag |= ( PARENB | CMSPAR ); break;
    #endif
        default: report( "SerialPort config", "mark / space parity is not supported here" ); return false;
    }

    // Stop bits.
    switch( c.stopbits )
    {
        case SerialPortConfig::STOPBITS_1: t.c_cflag &= ~CSTOPB; break;
        case SerialPortConfig::STOPBITS_2: t.c_cflag |= CSTOPB; break;
        default: report( "SerialPort config", "1.5 stop bits are not available through termios" ); return false;
    }

    // Flow control.
    t.c_iflag &= ~( IXON | IXOFF | IXANY );
    #ifdef CRTSCTS
    t.c_cflag &= ~CRTSCTS;
    #endif
    switch( c.flowcontrol )
    {
        case SerialPortConfig::FLOWCONTROL_NONE: break;
        case SerialPortConfig::FLOWCONTROL_XONXOFF: t.c_iflag |= ( IXON | IXOFF ); break;
    #ifdef CRTSCTS
        case SerialPortConfig::FLOWCONTROL_HARDWARE: t.c_cflag |= CRTSCTS; break;
    #endif
        default: report( "SerialPort config", "hardware flow control is not supported here" ); return false;
    }

    // Baud rate. A rate with a B constant goes through cfsetspeed. macOS sets any other rate
    // with the IOSSIOSPEED ioctl after tcsetattr; Linux has no other rate here.
    speed_t speed{};
    bool    customSpeed = false;
    if( speedConstantFor( c.bps, speed ) )
    {
        if( ::cfsetispeed( &t, speed ) != 0 || ::cfsetospeed( &t, speed ) != 0 )
        {
            report( "SerialPort config", juce::String( (int)c.bps ) + " baud refused: " + errnoText() );
            return false;
        }
    }
    else
    {
    #if JUCE_MAC || defined( __APPLE__ )
        customSpeed = true;
    #else
        report( "SerialPort config", juce::String( (int)c.bps ) + " baud is not a standard rate" );
        return false;
    #endif
    }

    if( ::tcsetattr( f, TCSANOW, &t ) != 0 )
    {
        report( "SerialPort config", c.toString() + " refused: " + errnoText() );
        return false;
    }

    #if JUCE_MAC || defined( __APPLE__ )
    if( customSpeed )
    {
        speed_t s = (speed_t)c.bps;
        if( ::ioctl( f, IOSSIOSPEED, &s ) != 0 )
        {
            report( "SerialPort config", juce::String( (int)c.bps ) + " baud refused (IOSSIOSPEED): " + errnoText() );
            ::tcsetattr( f, TCSANOW, &before ); // all or nothing
            return false;
        }
    }
    #endif
    juce::ignoreUnused( customSpeed );
    return true;
}

bool SerialPort::getConfig( SerialPortConfig& out ) const
{
    const int f = fd.load();
    if( f < 0 )
        return false;

    termios t{};
    if( ::tcgetattr( f, &t ) != 0 )
        return false;

    out.bps = bpsForSpeedConstant( ::cfgetospeed( &t ) );

    switch( t.c_cflag & CSIZE )
    {
        case CS5: out.databits = 5; break;
        case CS6: out.databits = 6; break;
        case CS7: out.databits = 7; break;
        default: out.databits = 8; break;
    }

    out.parity = SerialPortConfig::SERIALPORT_PARITY_NONE;
    if( t.c_cflag & PARENB )
    {
    #ifdef CMSPAR
        if( t.c_cflag & CMSPAR )
            out.parity = ( t.c_cflag & PARODD ) ? SerialPortConfig::SERIALPORT_PARITY_MARK : SerialPortConfig::SERIALPORT_PARITY_SPACE;
        else
    #endif
            out.parity = ( t.c_cflag & PARODD ) ? SerialPortConfig::SERIALPORT_PARITY_ODD : SerialPortConfig::SERIALPORT_PARITY_EVEN;
    }

    out.stopbits    = ( t.c_cflag & CSTOPB ) ? SerialPortConfig::STOPBITS_2 : SerialPortConfig::STOPBITS_1;
    out.flowcontrol = SerialPortConfig::FLOWCONTROL_NONE;
    #ifdef CRTSCTS
    if( ( t.c_cflag & CRTSCTS ) == CRTSCTS )
        out.flowcontrol = SerialPortConfig::FLOWCONTROL_HARDWARE;
    else
    #endif
        if( t.c_iflag & IXON )
        out.flowcontrol = SerialPortConfig::FLOWCONTROL_XONXOFF;
    return true;
}

bool SerialPort::write( const void* data, size_t numBytes, int timeoutMs )
{
    const juce::ScopedLock sl( writeLock );
    const int              f = fd.load();
    if( f < 0 )
        return false;

    auto*  p    = static_cast<const char*>( data );
    size_t left = numBytes;
    while( left > 0 )
    {
        const ssize_t n = ::write( f, p, left );
        if( n > 0 )
        {
            p += n;
            left -= (size_t)n;
            continue;
        }
        if( n < 0 && errno == EINTR )
            continue;
        if( n < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) )
        {
            pollfd pfd{ f, POLLOUT, 0 };
            const int r = ::poll( &pfd, 1, timeoutMs );
            if( r > 0 && ( pfd.revents & POLLOUT ) )
                continue;
            if( r < 0 && errno == EINTR )
                continue;
            report( "SerialPort write", portPath + ": the device took no data for " + juce::String( timeoutMs ) + " ms, " + juce::String( (juce::int64)left ) + " bytes dropped" );
            return false;
        }
        report( "SerialPort write", portPath + ": " + errnoText() );
        return false;
    }
    return true;
}

//==============================================================================
SerialPortInputStream::SerialPortInputStream( SerialPort* p )
    : juce::Thread( "SerialPort reader" )
    , port( p )
{
    if( ::pipe( wakePipe ) == 0 )
    {
        for( int f : wakePipe )
        {
            ::fcntl( f, F_SETFL, ::fcntl( f, F_GETFL ) | O_NONBLOCK );
            ::fcntl( f, F_SETFD, FD_CLOEXEC );
        }
    }

    if( port != nullptr )
    {
        port->attach( this );
        if( port->isOpen() )
            startThread();
    }
}

SerialPortInputStream::~SerialPortInputStream()
{
    stopReading();
    cancelPendingUpdate();
    if( port != nullptr )
        port->detach( this );
    for( int& f : wakePipe )
        if( f >= 0 )
        {
            ::close( f );
            f = -1;
        }
}

void SerialPortInputStream::stopReading()
{
    signalThreadShouldExit();
    if( wakePipe[1] >= 0 )
    {
        const char b = 0;
        [[maybe_unused]] auto n = ::write( wakePipe[1], &b, 1 );
    }
    stopThread( 2000 );

    // Drain the wake-up bytes so a restarted reader does not wake at once.
    char tmp[16];
    while( wakePipe[0] >= 0 && ::read( wakePipe[0], tmp, sizeof tmp ) > 0 )
    {
    }
}

void SerialPortInputStream::setNotify( notifyflag flag, char c )
{
    notifyChar.store( c );
    notify.store( flag );

    // Bytes that arrived before notifications were switched on would otherwise wait for the next.
    const juce::ScopedLock sl( bufferLock );
    if( !buffer.empty() && ( flag == NOTIFY_ALWAYS || ( flag == NOTIFY_ON_CHAR && std::find( buffer.begin(), buffer.end(), c ) != buffer.end() ) ) )
        triggerAsyncUpdate();
}

void SerialPortInputStream::run()
{
    char chunk[4096];
    while( !threadShouldExit() )
    {
        auto* p = port;
        if( p == nullptr )
            break;
        const int f = p->getHandle();
        if( f < 0 )
            break;

        pollfd fds[2] = { { f, POLLIN, 0 }, { wakePipe[0], POLLIN, 0 } };
        const int r = ::poll( fds, wakePipe[0] >= 0 ? 2 : 1, 500 );
        if( r < 0 )
        {
            if( errno == EINTR )
                continue;
            p->report( "SerialPort read", p->getPortPath() + ": poll failed: " + errnoText() + "; reader stopped" );
            break;
        }
        if( r == 0 || threadShouldExit() )
            continue;
        if( fds[1].revents != 0 )
            continue; // woken by stopReading()

        if( fds[0].revents & POLLIN )
        {
            const ssize_t n = ::read( f, chunk, sizeof chunk );
            if( n > 0 )
            {
                bool wake = false;
                {
                    const juce::ScopedLock sl( bufferLock );
                    buffer.insert( buffer.end(), chunk, chunk + n );
                }
                const int flag = notify.load();
                if( flag == NOTIFY_ALWAYS )
                    wake = true;
                else if( flag == NOTIFY_ON_CHAR )
                    wake = std::memchr( chunk, notifyChar.load(), (size_t)n ) != nullptr;
                if( wake )
                    triggerAsyncUpdate();
                continue;
            }
            if( n < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) )
                continue;
            // n == 0 with POLLIN, or EIO: the other end has gone (a pty whose master closed).
            p->report( "SerialPort read", p->getPortPath() + ": " + ( n == 0 ? juce::String( "end of file" ) : errnoText() ) + "; reader stopped" );
            break;
        }

        if( fds[0].revents & ( POLLHUP | POLLERR | POLLNVAL ) )
        {
            p->report( "SerialPort read", p->getPortPath() + ": hung up; reader stopped" );
            break;
        }
    }
}

#else // Windows: no implementation

juce::StringPairArray SerialPort::getSerialPortPaths() { return {}; }

bool SerialPort::open( const juce::String& newPortPath )
{
    portPath = newPortPath;
    report( "SerialPort open", newPortPath + ": no serial port support in this build" );
    return false;
}

void SerialPort::close() {}
bool SerialPort::setConfig( const SerialPortConfig& ) { return false; }
bool SerialPort::getConfig( SerialPortConfig& ) const { return false; }
bool SerialPort::write( const void*, size_t, int ) { return false; }

SerialPortInputStream::SerialPortInputStream( SerialPort* p )
    : juce::Thread( "SerialPort reader" )
    , port( p )
{
}

SerialPortInputStream::~SerialPortInputStream() { cancelPendingUpdate(); }
void SerialPortInputStream::stopReading() {}
void SerialPortInputStream::run() {}

void SerialPortInputStream::setNotify( notifyflag flag, char c )
{
    notifyChar.store( c );
    notify.store( flag );
}

#endif

//==============================================================================
size_t SerialPortInputStream::readableBytesLocked() const
{
    if( inCallback && juce::MessageManager::existsAndIsCurrentThread() )
        return std::min( callbackLimit, buffer.size() );
    return buffer.size();
}

void SerialPortInputStream::handleAsyncUpdate()
{
    {
        const juce::ScopedLock sl( bufferLock );
        inCallback    = true;
        callbackLimit = buffer.size();
    }
    sendSynchronousChangeMessage();
    {
        const juce::ScopedLock sl( bufferLock );
        inCallback = false;
    }
}

juce::int64 SerialPortInputStream::getTotalLength()
{
    const juce::ScopedLock sl( bufferLock );
    return (juce::int64)readableBytesLocked();
}

bool SerialPortInputStream::isExhausted()
{
    return getTotalLength() == 0;
}

int SerialPortInputStream::read( void* dest, int maxBytes )
{
    if( dest == nullptr || maxBytes <= 0 )
        return 0;

    const juce::ScopedLock sl( bufferLock );
    const size_t           n = std::min( (size_t)maxBytes, readableBytesLocked() );
    if( n == 0 )
        return 0;

    std::memcpy( dest, buffer.data(), n );
    buffer.erase( buffer.begin(), buffer.begin() + (std::ptrdiff_t)n );
    if( inCallback && juce::MessageManager::existsAndIsCurrentThread() )
        callbackLimit -= n;
    position += (juce::int64)n;
    return (int)n;
}

bool SerialPortInputStream::canReadLine() const
{
    const juce::ScopedLock sl( bufferLock );
    return std::find( buffer.begin(), buffer.end(), '\n' ) != buffer.end();
}

bool SerialPortInputStream::canReadString() const
{
    const juce::ScopedLock sl( bufferLock );
    return std::find( buffer.begin(), buffer.end(), '\0' ) != buffer.end();
}

//==============================================================================
SerialPortOutputStream::SerialPortOutputStream( SerialPort* p )
    : port( p )
{
}

bool SerialPortOutputStream::write( const void* data, size_t numBytes )
{
    if( port == nullptr || !port->write( data, numBytes ) )
        return false;
    position += (juce::int64)numBytes;
    return true;
}
