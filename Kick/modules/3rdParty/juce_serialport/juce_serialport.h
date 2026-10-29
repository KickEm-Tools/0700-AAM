/*******************************************************************************
 BEGIN_JUCE_MODULE_DECLARATION

  ID:                 juce_serialport
  vendor:             MUSIC Tribe
  version:            0.1.0
  name:               Serial port stub
  description:        API-compatible stub of the juce_serialport module. Compiles everywhere, talks to no hardware.
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
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * WHY THIS FILE EXISTS
 *
 * The original Kick tree vendored the community `juce_serialport` module, which has
 * no license and was therefore removed from the public release. `modules/input_events`
 * still includes it and uses a handful of its classes to talk to the control-surface
 * microcontroller over /dev/ttymxc0 on the device.
 *
 * This stub reproduces the public API surface that Kick uses, with no behaviour:
 *  - the port never opens,
 *  - the input stream is always empty,
 *  - the output stream discards writes.
 *
 * That is enough to build and run Kick on a desktop: there is no /dev/ttymxc0, so the
 * serial path in input_events.cpp never opens a port (macOS and Windows builds drive
 * the control surface from the keyboard instead, modules/contracts/utils/keyboard). On
 * the device it means the control surface (pads, buttons, knobs, LEDs) is unreachable:
 * a real device build must replace this directory with a working serial-port
 * implementation. Every Linux build prints a compiler message below so this cannot be
 * forgotten silently. It is a `#pragma message` rather than a `#warning` because
 * Kick/CMakeLists.txt compiles with `-w`, which hides `#warning`.
 */

#pragma once
#ifndef JUCE_SERIALPORT_STUB_H_INCLUDED
    #define JUCE_SERIALPORT_STUB_H_INCLUDED

    #include <juce_core/juce_core.h>
    #include <juce_events/juce_events.h>

    #include <functional>

    #if JUCE_LINUX || defined( __linux__ )
        #pragma message( "juce_serialport is a stub: the control surface will not be reachable in this build. A device build needs a real implementation, see modules/3rdParty/juce_serialport/juce_serialport.h" )
    #endif

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

    juce::uint32          bps         = 9600;
    juce::uint32          databits    = 8;
    SerialPortParity      parity      = SERIALPORT_PARITY_NONE;
    SerialPortStopBits    stopbits    = STOPBITS_1;
    SerialPortFlowControl flowcontrol = FLOWCONTROL_NONE;
};

class SerialPort
{
public:
    using DebugFunction = std::function<void( juce::String, juce::String )>;

    SerialPort( DebugFunction debug = nullptr )
        : debugFn( std::move( debug ) )
    {
    }

    SerialPort( const juce::String& portPath, const SerialPortConfig& portConfig, DebugFunction debug = nullptr )
        : debugFn( std::move( debug ) )
    {
        open( portPath );
        setConfig( portConfig );
    }

    virtual ~SerialPort() { close(); }

    /// On the device this enumerates /dev/tty*. The stub reports no ports.
    static juce::StringPairArray getSerialPortPaths() { return {}; }

    bool open( const juce::String& newPortPath )
    {
        portPath = newPortPath;
        if( debugFn ) debugFn( "juce_serialport stub", "open(" + newPortPath + ") ignored: no serial support in this build" );
        return false;
    }

    void close() { portPath.clear(); }
    bool exists() const { return false; }
    bool isOpen() const { return false; }

    bool             setConfig( const SerialPortConfig& newConfig ) { config = newConfig; return false; }
    bool             getConfig( SerialPortConfig& outConfig ) const { outConfig = config; return false; }
    juce::String     getPortPath() const { return portPath; }

private:
    juce::String     portPath;
    SerialPortConfig config;
    DebugFunction    debugFn;

    JUCE_DECLARE_NON_COPYABLE( SerialPort )
};

class SerialPortInputStream : public juce::InputStream,
                              public juce::ChangeBroadcaster
{
public:
    enum notifyflag
    {
        NOTIFY_OFF = 0,
        NOTIFY_ON_CHAR,
        NOTIFY_ALWAYS
    };

    explicit SerialPortInputStream( SerialPort* ) {}
    ~SerialPortInputStream() override = default;

    void setNotify( notifyflag flag = NOTIFY_ON_CHAR, char c = 0 )
    {
        notify     = flag;
        notifyChar = c;
    }

    bool canReadLine() const { return false; }
    bool canReadString() const { return false; }

    // juce::InputStream
    juce::int64 getTotalLength() override { return 0; }
    bool        isExhausted() override { return true; }
    int         read( void*, int ) override { return 0; }
    juce::int64 getPosition() override { return 0; }
    bool        setPosition( juce::int64 ) override { return false; }

private:
    notifyflag notify     = NOTIFY_ON_CHAR;
    char       notifyChar = 0;
};

class SerialPortOutputStream : public juce::OutputStream
{
public:
    explicit SerialPortOutputStream( SerialPort* ) {}
    ~SerialPortOutputStream() override = default;

    // juce::OutputStream
    void        flush() override {}
    bool        setPosition( juce::int64 ) override { return false; }
    juce::int64 getPosition() override { return 0; }
    bool        write( const void*, size_t ) override { return false; }
};

#endif // JUCE_SERIALPORT_STUB_H_INCLUDED
