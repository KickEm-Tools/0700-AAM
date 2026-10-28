/*
 * Copyright (c) 2024 MUSIC Tribe
 *
 * This file is part of Kick.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <functional>
#include <map>
#include <set>

namespace kick_ui
{
    /// The keys that change what a pad hit does, applied to pad packets before the current screen
    /// sees them, so every screen that plays pads (MAIN, GRID, mixers, PROG EDIT, ...) gets them:
    ///
    ///  - FULL LEVEL / HALF LEVEL (SHIFT + FULL LEVEL): toggles; pads play at velocity 127 / 64.
    ///    One excludes the other; both off = velocity from the pad.
    ///  - 16 LEVELS: toggle; the last pad hit is spread over the 16 pads, pad 1 softest
    ///    (velocity 7) to pad 16 loudest (127). The pads show the source pad's colour by level.
    ///  - NOTE REPEAT (held) / LATCH (SHIFT + NOTE REPEAT, toggles): a held pad retriggers at the
    ///    sequence's time-correct division (1/16 by default) at the sequence tempo; while the
    ///    sequencer plays the retriggers fall on that grid.
    ///  - ERASE held + pad: erases that pad's notes from the selected track instead of playing it.
    ///
    /// Not part of the original release: the app had no handler for NOTE REPEAT, LATCH and
    /// 16 LEVELS, and FULL / HALF LEVEL only lit their LEDs (see docs/DEVELOPMENT.md 4.9).
    class PadModifiers : private juce::Timer
    {
    public:
        using Dispatch = std::function<void( CPktMgr::HWMsg_t& )>;

        /// @param ToScreen hands a synthesised pad packet (note repeat) to the current screen.
        explicit PadModifiers( Dispatch ToScreen );
        ~PadModifiers() override;

        /// Call for every packet while a project is loaded. May rewrite a pad packet (pad number,
        /// velocity). Returns true when the packet is consumed and must not reach the screen.
        bool handle( CPktMgr::HWMsg_t& Msg, Screen Current );

    private:
        enum class Level
        {
            Off,
            Full,
            Half
        };

        struct Held
        {
            CPktMgr::HWMsg_t press; // the (rewritten) press packet to repeat
            double           nextMs{ 0.0 };
        };

        bool handleButton( int Code, bool Pressed );
        bool handlePad( CPktMgr::HWMsg_t& Msg, Screen Current );

        void setLevel( Level L );
        void setSixteenLevels( bool On );
        void updateRepeatLed();
        bool repeatActive() const { return repeatHeld || repeatLatched; }

        static bool   playsPads( Screen S );
        static int    sixteenLevelVelocity( int Pad );
        static double repeatIntervalMs();
        static double msToNextGridLine();

        void timerCallback() override;

        Dispatch           toScreen;
        Level              level{ Level::Off };
        bool               sixteenLevels{ false };
        int                sixteenSource{ 0 }; // pad 0-15 of the current bank
        int                lastPad{ 0 };
        bool               repeatHeld{ false }, repeatLatched{ false };
        bool               eraseHeld{ false };
        std::set<int>      erasedPads; // pads pressed while ERASE was held, until released
        std::map<int, Held> held;      // physical pad -> repeating press
        Screen             lastScreen{ Screen::Invalid };
    };
}
