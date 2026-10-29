/*
 * Copyright (c) 2025 MUSIC Tribe
 *
 * This file is part of Kick.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// kick_protocol_roundtrip: every control-surface packet type through CPktMgr and back.
//
//  - Surface -> app: encoded with CPktMgr::BuildAndTransmitPacket plus the one trailing byte
//    the microcontroller sends, decoded the way the app's serial reader does
//    (modules/input_events/input_events.cpp, HwSerialData::changeListenerCallback, copied
//    below as appRead(); keep the two in step), into the app's 20-byte receive FIFO.
//  - App -> surface: encoded the same way (no trailing byte), decoded byte by byte with
//    CPktMgr::PushByteToRxFifo, as a microcontroller would, into a FIFO large enough for the
//    biggest payload (33 bytes: EN_PAD_SET_STEP_ALL_PAD).
//
// Then the reader's behaviour on bad input, which docs/DEVELOPMENT.md section 8 describes:
// those cases print what happens and fail only if it differs from the description.
//
// Exit status 0 when everything matches.

#include <PktMgr/PktMgr.hpp>
#include <contracts/dcl/dcl_contracts.h>
#include <input_events/ComConfig.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using DCC = contracts::DeviceControlContracts;
    using Msg = CPktMgr::HWMsg_t;
    using Bytes = std::vector<uint8_t>;

    int failures = 0;
    int checks   = 0;

    const char* typeName( int t )
    {
        static const char* names[] = { "SYSTEM", "BUTTON", "LED", "PAD", "KNOBS", "MIDI", "CV", "SLIDER" };
        return t >= 0 && t < 8 ? names[t] : "?";
    }

    std::string hex( const Bytes& b )
    {
        std::string s;
        char        buf[4];
        for( auto v : b )
        {
            std::snprintf( buf, sizeof buf, "%02X ", v );
            s += buf;
        }
        if( !s.empty() ) s.pop_back();
        return s;
    }

    std::string describe( const Msg& m )
    {
        std::string s = std::string( typeName( m.pktType ) ) + ( m.pktDir == CPktMgr::EN_PKT_REQUEST ? " req" : " resp" ) + " cmd=" + std::to_string( m.pktCmd ) + " data[" + std::to_string( m.data.size() ) + "]=";
        for( size_t i = 0; i < m.data.size(); ++i )
            s += ( i ? "," : "" ) + std::to_string( m.data[i] );
        return s;
    }

    bool same( const Msg& a, const Msg& b ) { return a.pktType == b.pktType && a.pktDir == b.pktDir && a.pktCmd == b.pktCmd && a.data == b.data; }

    Msg msg( CPktMgr::enumPacketType type, int cmd, std::vector<int> data, CPktMgr::enumPacketDir dir = CPktMgr::EN_PKT_REQUEST )
    {
        Msg m;
        m.pktType = type;
        m.pktDir  = dir;
        m.pktCmd  = cmd;
        m.data    = std::move( data );
        return m;
    }

    /// CRC-8 as documented: polynomial 0x31, initial value 0xFF, MSB first, no final XOR,
    /// over every byte from 'M' to the last payload byte. Checked against CPktMgr::CrcGen.
    uint8_t referenceCrc( const Bytes& b, size_t n )
    {
        uint8_t c = 0xFF;
        for( size_t i = 0; i < n; ++i )
        {
            c ^= b[i];
            for( int j = 0; j < 8; ++j )
                c = ( c & 0x80 ) ? (uint8_t)( ( c << 1 ) ^ 0x31 ) : (uint8_t)( c << 1 );
        }
        return c;
    }

    Bytes encode( Msg m )
    {
        std::array<uint8_t, 64> rx{}, tx{};
        CPktMgr                 mgr( CPktMgr::EN_COM_CHANNEL_USB, rx.data(), (uint16_t)rx.size(), tx.data(), (uint16_t)tx.size(), 60 );
        mgr.Init();
        std::vector<char> frame;
        mgr.BuildAndTransmitPacket( m, frame );
        return { frame.begin(), frame.end() };
    }

    /// Reads a complete frame out of a CPktMgr, as input_events.cpp does.
    Msg take( CPktMgr& mgr )
    {
        Msg m;
        m.pktType = mgr.GetRxPktType();
        m.pktCmd  = mgr.GetRxCommand();
        m.pktDir  = mgr.GetPktDir();
        for( int size = (int)mgr.GetRxFrameSize(); size - 1; --size )
            m.data.push_back( (int)mgr.PopUint8FromRxFifo() );
        mgr.ClearReceiver();
        return m;
    }

    // The app's decoder state, as in input_events.cpp: one CPktMgr with BUFFER_RX_SIZE FIFOs.
    std::array<uint8_t, BUFFER_RX_SIZE> appRxFifo{}, appTxFifo{};
    CPktMgr                             appMgr( CPktMgr::EN_COM_CHANNEL_USB, appRxFifo.data(), BUFFER_RX_SIZE, appTxFifo.data(), BUFFER_RX_SIZE, 60 );

    /// One change notification's worth of bytes through the app's reader (a copy of the loop in
    /// HwSerialData::changeListenerCallback). Returns the frames it dispatches. *leftInStream is
    /// set when the reader returns without reading (fewer than 6 bytes available).
    std::vector<Msg> appRead( const Bytes& bytes, bool* leftInStream = nullptr )
    {
        std::vector<Msg> out;
        if( leftInStream ) *leftInStream = false;
        if( bytes.size() < 6 )
        {
            if( leftInStream ) *leftInStream = true;
            return out;
        }

        std::vector<char> serialData( bytes.begin(), bytes.end() );
        serialData.resize( bytes.size() + 8, 0 ); // the original reads byte [5] past a short tail
        const char* byteArray = serialData.data();
        int         rxLength  = (int)bytes.size();

        while( rxLength )
        {
            int packetLength = byteArray[5] + 7;
            if( ( rxLength >= packetLength ) && ( byteArray[0] == 'M' ) && ( byteArray[1] == 'T' ) )
            {
                while( packetLength )
                {
                    if( packetLength > 1 )
                        appMgr.PushByteToRxFifo( byteArray[0] );
                    byteArray++;
                    packetLength--;
                    rxLength--;
                }
            }
            else
                rxLength = 0;

            if( appMgr.IsFrameRcvComplete() == TRUE )
            {
                appMgr.SetFrameRcvComplete( FALSE );
                out.push_back( take( appMgr ) );
            }
            else
                appMgr.ClearReceiver();
        }
        return out;
    }

    /// A microcontroller-side decoder: every byte into PushByteToRxFifo. The FIFO is large
    /// enough for the largest payload the app sends. CPktMgr keeps its partial-frame buffer in a
    /// function-level static shared by every instance, so the app and surface decoders must not
    /// be fed interleaved.
    std::vector<Msg> surfaceRead( const Bytes& bytes )
    {
        std::array<uint8_t, 256> rx{}, tx{};
        CPktMgr                  mgr( CPktMgr::EN_COM_CHANNEL_USB, rx.data(), (uint16_t)rx.size(), tx.data(), (uint16_t)tx.size(), 60 );
        mgr.Init();
        std::vector<Msg> out;
        for( auto b : bytes )
        {
            mgr.PushByteToRxFifo( b );
            if( mgr.IsFrameRcvComplete() )
            {
                mgr.SetFrameRcvComplete( false );
                out.push_back( take( mgr ) );
                mgr.Init(); // a microcontroller restarts its state machine after each frame
            }
        }
        return out;
    }

    void expect( bool ok, const std::string& what )
    {
        ++checks;
        if( !ok ) ++failures;
        std::printf( "  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str() );
    }

    /// Frame layout and CRC, independent of CPktMgr's own decoder.
    void checkLayout( const Msg& m, const Bytes& w )
    {
        const size_t n = m.data.size();
        bool         ok = w.size() == n + 7 && w[0] == 'M' && w[1] == 'T' && w[2] == m.pktType && w[3] == m.pktDir && w[4] == (uint8_t)m.pktCmd && w[5] == n + 1;
        for( size_t i = 0; ok && i < n; ++i )
            ok = w[6 + i] == (uint8_t)m.data[i];
        ok = ok && w.back() == referenceCrc( w, w.size() - 1 );
        std::vector<char> c( w.begin(), w.end() - 1 );
        ok = ok && CPktMgr::CrcGen( c ) == w.back();
        expect( ok, "layout 'M' 'T' type dir cmd size=" + std::to_string( n + 1 ) + " data[" + std::to_string( n ) + "] crc=0x" + hex( { w.back() } ) );
    }

    void surfaceToApp( const char* label, const Msg& m )
    {
        std::printf( "%s\n  sent  %s\n", label, describe( m ).c_str() );
        Bytes w = encode( m );
        checkLayout( m, w );
        w.push_back( '\n' );
        std::printf( "  wire  %s\n", hex( w ).c_str() );
        const auto got = appRead( w );
        expect( got.size() == 1 && same( got[0], m ), "app reader: " + ( got.empty() ? std::string( "(nothing)" ) : describe( got[0] ) ) );
    }

    void appToSurface( const char* label, const Msg& m )
    {
        std::printf( "%s\n  sent  %s\n", label, describe( m ).c_str() );
        const Bytes w = encode( m );
        checkLayout( m, w );
        std::printf( "  wire  %s\n", hex( w ).c_str() );
        const auto got = surfaceRead( w );
        expect( got.size() == 1 && same( got[0], m ), "surface decoder: " + ( got.empty() ? std::string( "(nothing)" ) : describe( got[0] ) ) );
    }

    std::vector<int> range( int n, int start )
    {
        std::vector<int> v;
        for( int i = 0; i < n; ++i )
            v.push_back( ( start + i ) & 0xFF );
        return v;
    }

    Bytes withTrailer( const Msg& m )
    {
        Bytes w = encode( m );
        w.push_back( '\n' );
        return w;
    }

    Bytes cat( std::initializer_list<Bytes> parts )
    {
        Bytes out;
        for( const auto& p : parts )
            out.insert( out.end(), p.begin(), p.end() );
        return out;
    }
} // namespace

int main()
{
    appMgr.Init();

    std::printf( "== Surface -> app (trailing byte after the CRC, app reader, %d-byte FIFO)\n\n", BUFFER_RX_SIZE );
    surfaceToApp( "SYSTEM EN_SYS_CHECK_COMM_STATUS [1] (comm init)", msg( CPktMgr::EN_PKT_TYPE_SYSTEM, DCC::EN_SYS_CHECK_COMM_STATUS, { 1 } ) );
    surfaceToApp( "SYSTEM EN_SYS_CHECK_COMM_STATUS [2] (comm live)", msg( CPktMgr::EN_PKT_TYPE_SYSTEM, DCC::EN_SYS_CHECK_COMM_STATUS, { 2 } ) );
    surfaceToApp( "SYSTEM EN_SYS_GET_SW_VER [major, minor]", msg( CPktMgr::EN_PKT_TYPE_SYSTEM, DCC::EN_SYS_GET_SW_VER, { 1, 7 } ) );
    surfaceToApp( "SYSTEM EN_SYS_SW_UPDATE [code]", msg( CPktMgr::EN_PKT_TYPE_SYSTEM, DCC::EN_SYS_SW_UPDATE, { 3 } ) );
    surfaceToApp( "SYSTEM response (ignored by the app, decoded all the same)", msg( CPktMgr::EN_PKT_TYPE_SYSTEM, DCC::EN_SYS_COMM_INIT, { 1 }, CPktMgr::EN_PKT_RESPONSE ) );
    surfaceToApp( "BUTTON EN_BTN_STATUS PLAY pressed", msg( CPktMgr::EN_PKT_TYPE_BUTTON, DCC::EN_BTN_STATUS, { DCC::EN_PLAY, DCC::BTN_PRESSED } ) );
    surfaceToApp( "BUTTON EN_BTN_STATUS SHIFT pressed", msg( CPktMgr::EN_PKT_TYPE_BUTTON, DCC::EN_BTN_STATUS, { DCC::EN_SHIFT, DCC::BTN_PRESSED } ) );
    surfaceToApp( "BUTTON EN_BTN_STATUS ERASE long press", msg( CPktMgr::EN_PKT_TYPE_BUTTON, DCC::EN_BTN_STATUS, { DCC::EN_ERASE, DCC::BTN_LONGPRESSED } ) );
    surfaceToApp( "PAD EN_PAD_STATE pad 16 pressed, velocity 127, aftertouch 64", msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_STATE, { DCC::EN_PAD16, DCC::BTN_PRESSED, 127, 64 } ) );
    surfaceToApp( "KNOBS EN_KNOB_ROTARY centre knob, clockwise, 3 steps", msg( CPktMgr::EN_PKT_TYPE_KNOBS, DCC::EN_KNOB_ROTARY, { DCC::EN_KNOB_CENTER, DCC::EN_KNOB_CLOCKWISE, 3 } ) );
    surfaceToApp( "KNOBS EN_KNOB_QLINK knob 16, anticlockwise, 1 step", msg( CPktMgr::EN_PKT_TYPE_KNOBS, DCC::EN_KNOB_QLINK, { DCC::EN_KNOB_16, DCC::EN_KNOB_ANTICLOCKWISE, 1 } ) );
    surfaceToApp( "SLIDER EN_SLIDER_TOUCH_INPUT 100", msg( CPktMgr::EN_PKT_TYPE_SLIDER, DCC::EN_SLIDER_TOUCH_INPUT, { 100 } ) );
    surfaceToApp( "MIDI (type defined, unused by the app) note on", msg( CPktMgr::EN_PKT_TYPE_MIDI, 0, { 0x90, 60, 100 } ) );
    surfaceToApp( "CV (type defined, unused by the app)", msg( CPktMgr::EN_PKT_TYPE_CV, 0, { 1, 0x7F } ) );
    surfaceToApp( "largest payload that fits the app's FIFO (20 bytes)", msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_STATE, range( BUFFER_RX_SIZE, 1 ) ) );

    std::printf( "\n== App -> surface (no trailing byte, byte-by-byte decoder)\n\n" );
    appToSurface( "SYSTEM EN_SYS_COMM_INIT [EN_SYS_COMM_CONNECTED] (sent on connect)", msg( CPktMgr::EN_PKT_TYPE_SYSTEM, DCC::EN_SYS_COMM_INIT, { DCC::EN_SYS_COMM_CONNECTED } ) );
    appToSurface( "SYSTEM EN_SYS_COMM_INIT [2] (answer to comm init)", msg( CPktMgr::EN_PKT_TYPE_SYSTEM, DCC::EN_SYS_COMM_INIT, { DCC::EN_SYS_COMM_INIT } ) );
    appToSurface( "SYSTEM EN_SYS_CHECK_COMM_STATUS [2] (answer to comm live)", msg( CPktMgr::EN_PKT_TYPE_SYSTEM, DCC::EN_SYS_CHECK_COMM_STATUS, { 2 } ) );
    appToSurface( "SYSTEM EN_SYS_BPM [hi, lo] (120.00 BPM as 12000)", msg( CPktMgr::EN_PKT_TYPE_SYSTEM, DCC::EN_SYS_BPM, { 12000 >> 8, 12000 & 0xFF } ) );
    appToSurface( "SYSTEM EN_SYS_SW_UPDATE [code]", msg( CPktMgr::EN_PKT_TYPE_SYSTEM, DCC::EN_SYS_SW_UPDATE, { 1 } ) );
    appToSurface( "BUTTON EN_BTN_STATUS (setPlayStateMessage)", msg( CPktMgr::EN_PKT_TYPE_BUTTON, DCC::EN_BTN_STATUS, { DCC::EN_PLAY, DCC::BTN_PRESSED } ) );
    appToSurface( "LED EN_LED_SET_SINGLE_LED STOP on, colour 1", msg( CPktMgr::EN_PKT_TYPE_LED, DCC::EN_LED_SET_SINGLE_LED, { DCC::EN_STOP, DCC::StateOn, 1 } ) );
    appToSurface( "LED EN_LED_SET_SINGLE_LED bank B (setBankMessage)", msg( CPktMgr::EN_PKT_TYPE_LED, DCC::EN_LED_SET_SINGLE_LED, { DCC::EN_BANK_B, 1 } ) );
    appToSurface( "LED EN_LED_BLINK_SINGLE", msg( CPktMgr::EN_PKT_TYPE_LED, DCC::EN_LED_BLINK_SINGLE, { DCC::EN_RECORD, DCC::StateOnBlink, 1 } ) );
    appToSurface( "LED EN_LED_SET_ALL", msg( CPktMgr::EN_PKT_TYPE_LED, DCC::EN_LED_SET_ALL, { DCC::StateOff } ) );
    appToSurface( "PAD EN_PAD_SET_SINGLE_PAD", msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_SET_SINGLE_PAD, { DCC::EN_PAD3, DCC::StateSampleAssigned, DCC::PadColourAqua } ) );
    appToSurface( "PAD EN_PAD_SET_ALL_SINGLE_COLOUR (clearAllPadsMessage)", msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_SET_ALL_SINGLE_COLOUR, { 0, DCC::PadColourTrans } ) );
    appToSurface( "PAD EN_PAD_SET_ALL_MULTI_COLOUR state + 16 colours (17 bytes)", msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_SET_ALL_MULTI_COLOUR, [] { auto v = range( 16, 0 ); v.insert( v.begin(), DCC::StateOn ); return v; }() ) );
    appToSurface( "PAD EN_PAD_SET_COLOUR_STATE 16 colours + 16 states (32 bytes)", msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_SET_COLOUR_STATE, [] { auto v = range( 16, 0 ); auto s = range( 16, 0 ); for( auto& x : s ) x %= 6; v.insert( v.end(), s.begin(), s.end() ); return v; }() ) );
    appToSurface( "PAD EN_PAD_SET_STEP_ALL_PAD state + 16 colours + 16 velocities (33 bytes, the largest)", msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_SET_STEP_ALL_PAD, [] { std::vector<int> v{ DCC::StateOn }; auto c = range( 16, 0 ); auto vel = range( 16, 112 ); v.insert( v.end(), c.begin(), c.end() ); v.insert( v.end(), vel.begin(), vel.end() ); return v; }() ) );
    appToSurface( "PAD EN_PAD_SET_STEP_SINGLE_PAD", msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_SET_STEP_SINGLE_PAD, { DCC::EN_PAD1, DCC::StateOn, DCC::PadColourRed, 100 } ) );
    appToSurface( "PAD EN_PAD_SETTINGS", msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_SETTINGS, { 1, 2, 3, 4 } ) );
    appToSurface( "PAD EN_PAD_COLOR_MODE", msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_COLOR_MODE, { 1 } ) );
    appToSurface( "PAD EN_PAD_SET_AFTER_TOUCH", msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_SET_AFTER_TOUCH, { 1 } ) );
    appToSurface( "PAD EN_PAD_SET_BLINKING", msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_SET_BLINKING, { DCC::EN_PAD2, DCC::StateOnBlink, DCC::PadColourBlue1, DCC::PadColourTrans } ) );
    appToSurface( "SLIDER EN_SLIDER_BAR_LED 50 %", msg( CPktMgr::EN_PKT_TYPE_SLIDER, DCC::EN_SLIDER_BAR_LED, { 50 } ) );
    appToSurface( "SLIDER EN_SLIDER_LEVEL_LED 75 %", msg( CPktMgr::EN_PKT_TYPE_SLIDER, DCC::EN_SLIDER_LEVEL_LED, { 75 } ) );
    appToSurface( "SLIDER EN_SLIDER_INDIVIDUAL_LED", msg( CPktMgr::EN_PKT_TYPE_SLIDER, DCC::EN_SLIDER_INDIVIDUAL_LED, { 7 } ) );
    appToSurface( "MIDI (type defined, unused by the app)", msg( CPktMgr::EN_PKT_TYPE_MIDI, 0, { 0x80, 60, 0 } ) );
    appToSurface( "CV (type defined, unused by the app)", msg( CPktMgr::EN_PKT_TYPE_CV, 0, { 0, 0x40 } ) );

    std::printf( "\n== Values are bytes\n\n" );
    {
        const Msg  m   = msg( CPktMgr::EN_PKT_TYPE_LED, DCC::EN_LED_SET_SINGLE_LED, { -1, 256 + 5, 300 } );
        const auto got = surfaceRead( encode( m ) );
        expect( got.size() == 1 && got[0].data == std::vector<int>{ 255, 5, 44 }, "data -1, 261, 300 arrive as 255, 5, 44 (low 8 bits, unsigned)" );
    }

    std::printf( "\n== The app reader on imperfect input (docs/DEVELOPMENT.md 8.4)\n\n" );
    const Msg   play  = msg( CPktMgr::EN_PKT_TYPE_BUTTON, DCC::EN_BTN_STATUS, { DCC::EN_PLAY, DCC::BTN_PRESSED } );
    const Msg   pad1  = msg( CPktMgr::EN_PKT_TYPE_PAD, DCC::EN_PAD_STATE, { DCC::EN_PAD1, DCC::BTN_PRESSED, 100, 0 } );
    const Bytes wPlay = withTrailer( play ), wPad = withTrailer( pad1 );
    {
        const auto got = appRead( cat( { wPlay, wPad } ) );
        expect( got.size() == 2 && same( got[0], play ) && same( got[1], pad1 ), "two frames in one read: both dispatched" );
    }
    {
        Bytes bare = encode( play );
        expect( appRead( bare ).empty(), "frame without the trailing byte, alone: dropped (the reader wants size + 7 bytes)" );
        const auto got = appRead( cat( { encode( play ), encode( pad1 ) } ) );
        expect( got.size() == 1 && same( got[0], play ), "two frames without trailing bytes: the first is taken, its 'trailing' byte is the second's 'M', the second is lost" );
    }
    {
        Bytes bad = wPlay;
        bad[bad.size() - 2] ^= 0x55;
        expect( appRead( bad ).empty(), "bad CRC: dropped silently" );
        const auto got = appRead( wPad );
        expect( got.size() == 1 && same( got[0], pad1 ), "the next good frame after a bad CRC: dispatched" );
    }
    {
        const Bytes head( wPlay.begin(), wPlay.begin() + 7 ), tail( wPlay.begin() + 7, wPlay.end() );
        const auto  a = appRead( head );
        expect( a.empty(), "a frame split across two notifications, first part (7 of " + std::to_string( wPlay.size() ) + " bytes): read and dropped" );
        const auto b = appRead( cat( { tail, wPad } ) );
        expect( b.empty(), "the rest of it arrives with the next frame: the read does not start with 'M', both are dropped" );
        const auto b2 = appRead( wPad );
        expect( b2.size() == 1 && same( b2[0], pad1 ), "the frame after that: dispatched" );
        bool       left = false;
        const auto c    = appRead( Bytes( wPlay.begin(), wPlay.begin() + 5 ), &left );
        expect( c.empty() && left, "fewer than 6 bytes available: left in the stream until more arrive" );
    }
    {
        const auto got = appRead( cat( { Bytes{ 0x00, 0xFF }, wPlay } ) );
        expect( got.empty(), "garbage before a frame in the same read: the whole read is dropped" );
        const auto got2 = appRead( cat( { wPlay, Bytes{ 0x00 }, wPad } ) );
        expect( got2.size() == 1 && same( got2[0], play ), "garbage between two frames: the first is dispatched, the rest of the read dropped" );
    }
    {
        const Msg empty = msg( CPktMgr::EN_PKT_TYPE_SYSTEM, DCC::EN_SYS_GET_SW_VER, {} );
        expect( appRead( withTrailer( empty ) ).empty(), "frame with no payload: never completes" );
        const auto got = appRead( wPad );
        expect( got.empty(), "the good frame after it: also lost (CPktMgr's partial-frame buffer is not cleared by ClearReceiver)" );
        const auto got2 = appRead( wPad );
        expect( got2.size() == 1 && same( got2[0], pad1 ), "the frame after that: dispatched again" );
    }

    std::printf( "\n%d checks, %d failed: %s\n", checks, failures, failures ? "FAIL" : "PASS" );
    return failures ? 1 : 0;
}
