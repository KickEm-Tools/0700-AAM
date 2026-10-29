/*
 * Copyright (c) 2025 MUSIC Tribe
 *
 * This file is part of Kick.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <PktMgr/PktMgr.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Controls
{
    /// Cuts the bytes the control surface sends into frames, checks each with CPktMgr, and hands
    /// back the messages (docs/DEVELOPMENT.md 8.2 and 8.4).
    ///
    /// A frame is 'M' 'T' type dir cmd size data[size - 1] crc. The surface sends one more byte
    /// after the CRC (a '\n'); it is skipped, and not required. Bytes are kept across reads, so a
    /// frame split over two serial notifications is put back together. Anything that is not the
    /// start of a frame is skipped up to the next 'M' 'T'. A frame whose CRC fails is dropped (up to
    /// an 'M' 'T' inside it, where a good frame may start), so a good frame after it is still found. Frames
    /// with no payload (which CPktMgr::PushByteToRxFifo never completes, and which no handler
    /// expects) and frames whose payload would overrun the receive FIFO are dropped.
    ///
    /// Every frame is fed to CPktMgr from its 'M', whole, so CPktMgr's partial-frame buffer (a
    /// function-level static) is always emptied by its CRC state.
    class SurfaceFrameReader
    {
    public:
        struct Stats
        {
            int frames{ 0 }, badCrc{ 0 }, noPayload{ 0 }, oversized{ 0 }, skippedBytes{ 0 };
        };

        /// Decoder: the CPktMgr whose receive FIFO holds MaxPayload bytes.
        SurfaceFrameReader( CPktMgr& Decoder, size_t MaxPayload )
            : decoder( Decoder ), maxPayload( MaxPayload ) {}

        /// Problems worth a log line ("skipped 3 bytes before a frame: 00 FF 13"). Optional.
        std::function<void( const std::string& )> onProblem;

        /// Appends Size bytes and calls OnFrame for every complete, CRC-checked frame.
        void push( const uint8_t* Data, size_t Size, const std::function<void( CPktMgr::HWMsg_t& )>& OnFrame )
        {
            pending.insert( pending.end(), Data, Data + Size );
            for( ;; )
            {
                // Up to the next 'M' 'T'. One byte right after a frame is its trailer: no report.
                size_t start = 0;
                while( start + 1 < pending.size() && !( pending[start] == 'M' && pending[start + 1] == 'T' ) )
                    ++start;
                if( start + 1 >= pending.size() && !pending.empty() && pending.back() != 'M' )
                    start = pending.size(); // nothing that could start a frame
                if( start > 0 )
                {
                    const size_t skipped = start - ( trailerDue ? 1 : 0 );
                    if( skipped > 0 )
                    {
                        stats.skippedBytes += (int)skipped;
                        problem( "skipped " + std::to_string( skipped ) + " byte(s) before a frame: " + hex( pending.data() + ( trailerDue ? 1 : 0 ), skipped ) );
                    }
                    pending.erase( pending.begin(), pending.begin() + (long)start );
                }
                trailerDue = false;
                if( pending.size() < 6 )
                    return; // wait for the header

                const size_t size = pending[5]; // data bytes + 1 (the CRC)
                if( size == 0 || size - 1 > maxPayload )
                {
                    // Not a frame we can take: a size of 0 cannot be, and a payload over the
                    // receive FIFO would overrun it. Resume the search after this 'M'.
                    if( size != 0 )
                        ++stats.oversized;
                    problem( "dropped a frame header with size " + std::to_string( size ) + " (the payload limit is " + std::to_string( maxPayload ) + ")" );
                    pending.erase( pending.begin() );
                    continue;
                }
                const size_t total = 6 + size;
                if( pending.size() < total )
                    return; // the rest comes with the next read

                if( size == 1 )
                {
                    ++stats.noPayload;
                    problem( "dropped a frame with no payload: " + hex( pending.data(), total ) );
                    pending.erase( pending.begin(), pending.begin() + (long)total );
                    trailerDue = true;
                    continue;
                }

                decoder.ClearReceiver();
                for( size_t i = 0; i < total; ++i )
                    decoder.PushByteToRxFifo( pending[i] );
                if( !decoder.IsFrameRcvComplete() )
                {
                    ++stats.badCrc;
                    problem( "dropped a frame with a bad CRC: " + hex( pending.data(), total ) );
                    decoder.ClearReceiver();
                    // Drop it, up to an 'M' 'T' inside it, where a good frame may start (the size
                    // byte may have been the corrupt one).
                    size_t next = 1;
                    while( next + 1 < total && !( pending[next] == 'M' && pending[next + 1] == 'T' ) )
                        ++next;
                    pending.erase( pending.begin(), pending.begin() + (long)( next + 1 < total ? next : total ) );
                    trailerDue = next + 1 >= total;
                    continue;
                }
                decoder.SetFrameRcvComplete( false );
                CPktMgr::HWMsg_t msg;
                msg.pktType = decoder.GetRxPktType();
                msg.pktCmd  = decoder.GetRxCommand();
                msg.pktDir  = decoder.GetPktDir();
                for( size_t i = 1; i < size; ++i )
                    msg.data.push_back( (int)decoder.PopUint8FromRxFifo() );
                decoder.ClearReceiver();
                pending.erase( pending.begin(), pending.begin() + (long)total );
                trailerDue = true;
                ++stats.frames;
                OnFrame( msg );
            }
        }

        /// Forgets a partial frame (after the port was reopened).
        void reset()
        {
            pending.clear();
            trailerDue = false;
            decoder.ClearReceiver();
        }

        size_t pendingBytes() const { return pending.size(); }

        Stats stats;

    private:
        void problem( const std::string& What )
        {
            if( onProblem )
                onProblem( What );
        }

        static std::string hex( const uint8_t* Data, size_t Size )
        {
            static const char* digits = "0123456789ABCDEF";
            std::string        s;
            for( size_t i = 0; i < Size && i < 48; ++i )
            {
                if( i ) s += ' ';
                s += digits[Data[i] >> 4];
                s += digits[Data[i] & 15];
            }
            if( Size > 48 ) s += " ...";
            return s;
        }

        CPktMgr&             decoder;
        const size_t         maxPayload;
        std::vector<uint8_t> pending;
        bool                 trailerDue{ false };
    };
} // namespace Controls
