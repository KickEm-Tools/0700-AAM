/*******************************************************************************
 BEGIN_JUCE_MODULE_DECLARATION

  ID:                 juce_serialport
  vendor:             MUSIC Tribe
  version:            1.0.0
  name:               Serial port
  description:        POSIX (termios) serial port with JUCE input / output streams. Linux and macOS; a stub on Windows.
  website:
  license:            AGPLv3
  dependencies:       juce_core, juce_events

 END_JUCE_MODULE_DECLARATION
*******************************************************************************/

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

/*
 * A serial port for the link between the Kick app and the control-surface microcontroller
 * (/dev/ttymxc0 on the unit, modules/input_events). Written for this repository: the module the
 * original tree vendored had no licence and was removed from the public release, so this keeps
 * the API that input_events uses and implements it from scratch on termios.
 *
 *  - SerialPort opens a device with O_NOCTTY, puts it in raw mode and applies a
 *    SerialPortConfig (baud rate, data bits, parity, stop bits, flow control).
 *  - SerialPortInputStream runs a reader thread that appends everything the port delivers to a
 *    buffer and tells its ChangeListeners (on the message thread) that data arrived.
 *  - SerialPortOutputStream writes straight to the port; it has no buffer of its own.
 *  - SerialPort::getSerialPortPaths() lists the serial devices in /dev.
 *
 * Windows has no termios: there the classes compile, the port never opens, reads are empty and
 * writes are discarded (the app drives the control surface from the keyboard on desktops).
 */

#pragma once
#ifndef JUCE_SERIALPORT_H_INCLUDED
    #define JUCE_SERIALPORT_H_INCLUDED

    #include <juce_core/juce_core.h>
    #include <juce_events/juce_events.h>

    #include <atomic>
    #include <functional>
    #include <vector>

    #if JUCE_WINDOWS
        #pragma message( "juce_serialport has no Windows implementation: serial ports never open in this build" )
        #define JUCE_SERIALPORT_POSIX 0
    #else
        #define JUCE_SERIALPORT_POSIX 1
    #endif

class SerialPortInputStream;

class SerialPortConfig
{
public:
    enum SerialPortParity
    {
        SERIALPORT_PARITY_NONE = 0,
        SERIALPORT_PARITY_ODD,
        SERIALPORT_PARITY_EVEN,
        SERIALPORT_PARITY_MARK,
        SERIALPORT_PARITY_SPACE
    };

    enum SerialPortStopBits
    {
        STOPBITS_1 = 0,
        STOPBITS_1ANDHALF,
        STOPBITS_2
    };

    enum SerialPortFlowControl
    {
        FLOWCONTROL_NONE = 0,
        FLOWCONTROL_XONXOFF,
        FLOWCONTROL_HARDWARE
    };

    SerialPortConfig() = default;
    SerialPortConfig( juce::uint32 bitsPerSecond, juce::uint32 dataBits, SerialPortParity parityBits, SerialPortStopBits stopBitsValue, SerialPortFlowControl flow )
        : bps( bitsPerSecond ), databits( dataBits ), parity( parityBits ), stopbits( stopBitsValue ), flowcontrol( flow )
    {
    }

    /// "230400 8N1, no flow control", for logs.
    juce::String toString() const;

    juce::uint32          bps         = 9600;
    juce::uint32          databits    = 8;
    SerialPortParity      parity      = SERIALPORT_PARITY_NONE;
    SerialPortStopBits    stopbits    = STOPBITS_1;
    SerialPortFlowControl flowcontrol = FLOWCONTROL_NONE;
};

class SerialPort
{
public:
    /// Called with a short context ("SerialPort open") and a message, for every error and state
    /// change. May be called from the reader thread.
    using DebugFunction = std::function<void( juce::String, juce::String )>;

    SerialPort( DebugFunction debug = nullptr );
    /// Opens portPath and applies portConfig. Check isOpen() for the result.
    SerialPort( const juce::String& portPath, const SerialPortConfig& portConfig, DebugFunction debug = nullptr );
    virtual ~SerialPort();

    /// The serial devices in /dev, as { name, path } pairs, for example { "ttymxc0", "/dev/ttymxc0" }.
    /// Linux: ttyS*, ttyUSB*, ttyACM*, ttyAMA*, ttymxc*, ttyLP*, rfcomm*. macOS: cu.* and tty.*.
    /// Empty on Windows.
    static juce::StringPairArray getSerialPortPaths();

    /// Opens the device (O_RDWR | O_NOCTTY), in raw mode, and keeps the current settings until
    /// setConfig() is called. Closes a port that is already open first.
    bool open( const juce::String& newPortPath );
    /// Stops the reader threads of the attached input streams, then closes the device.
    void close();
    /// True when the path of this port names an existing file.
    bool exists() const;
    bool isOpen() const;

    /// Applies a configuration to the open port: baud rate, 5-8 data bits, parity, 1 or 2 stop
    /// bits (1.5 is not available through termios and fails), flow control. Raw mode, receiver
    /// on, modem lines ignored. Returns false if the port is closed or the device refused it.
    bool setConfig( const SerialPortConfig& newConfig );
    /// Reads the configuration back from the device (what it actually accepted).
    bool         getConfig( SerialPortConfig& outConfig ) const;
    juce::String getPortPath() const { return portPath; }

    /// Writes every byte, waiting for the device to accept them (up to timeoutMs per stall).
    /// Returns false if the port is closed, the write failed or the device stalled.
    bool write( const void* data, size_t numBytes, int timeoutMs = 1000 );

    /// The file descriptor, or -1.
    int getHandle() const { return fd.load(); }

    void report( const juce::String& context, const juce::String& message ) const;

private:
    friend class SerialPortInputStream;
    void attach( SerialPortInputStream* );
    void detach( SerialPortInputStream* );

    juce::String                          portPath;
    std::atomic<int>                      fd{ -1 };
    DebugFunction                         debugFn;
    juce::CriticalSection                 streamsLock, writeLock;
    juce::Array<SerialPortInputStream*>   streams;

    JUCE_DECLARE_NON_COPYABLE( SerialPort )
};

/// Bytes received from a SerialPort. A reader thread appends everything the port delivers to an
/// internal buffer; read() consumes from the front of it. ChangeListeners are called on the
/// message thread when data arrives (see setNotify). While a listener runs, getTotalLength()
/// and read() on the message thread see only the bytes that had arrived when the callback
/// started, so a listener that asks for the length and then reads that many bytes gets exactly
/// that many even if more arrive meanwhile; the newer bytes cause another callback.
class SerialPortInputStream : public juce::InputStream,
                              public juce::ChangeBroadcaster,
                              private juce::Thread,
                              private juce::AsyncUpdater
{
public:
    enum notifyflag
    {
        NOTIFY_OFF = 0, ///< no change messages
        NOTIFY_ON_CHAR, ///< a change message when the notify character arrives
        NOTIFY_ALWAYS ///< a change message whenever data arrives
    };

    explicit SerialPortInputStream( SerialPort* port );
    ~SerialPortInputStream() override;

    void setNotify( notifyflag flag = NOTIFY_ON_CHAR, char c = 0 );

    /// True when a '\n' is buffered.
    bool canReadLine() const;
    /// True when a '\0' is buffered.
    bool canReadString() const;

    /// The reader thread is running (the port is open and has not hung up or failed).
    bool isReading() const { return isThreadRunning(); }

    // juce::InputStream
    juce::int64 getTotalLength() override;
    bool        isExhausted() override;
    int         read( void* destBuffer, int maxBytesToRead ) override;
    juce::int64 getPosition() override { return position; }
    bool        setPosition( juce::int64 ) override { return false; }

private:
    friend class SerialPort;
    void   run() override;
    void   handleAsyncUpdate() override;
    void   stopReading();
    size_t readableBytesLocked() const;

    SerialPort*               port;
    int                       wakePipe[2]{ -1, -1 };
    juce::CriticalSection     bufferLock;
    std::vector<char>         buffer;
    bool                      inCallback{ false };
    size_t                    callbackLimit{ 0 };
    juce::int64               position{ 0 };
    std::atomic<int>          notify{ NOTIFY_ON_CHAR };
    std::atomic<char>         notifyChar{ 0 };

    JUCE_DECLARE_NON_COPYABLE( SerialPortInputStream )
};

/// Bytes to a SerialPort. Unbuffered: write() hands the bytes to the device before it returns,
/// so flush() has nothing to do.
class SerialPortOutputStream : public juce::OutputStream
{
public:
    explicit SerialPortOutputStream( SerialPort* port );
    ~SerialPortOutputStream() override = default;

    // juce::OutputStream
    void        flush() override {}
    bool        setPosition( juce::int64 ) override { return false; }
    juce::int64 getPosition() override { return position; }
    bool        write( const void* data, size_t numBytes ) override;

private:
    SerialPort* port;
    juce::int64 position{ 0 };

    JUCE_DECLARE_NON_COPYABLE( SerialPortOutputStream )
};

#endif // JUCE_SERIALPORT_H_INCLUDED
