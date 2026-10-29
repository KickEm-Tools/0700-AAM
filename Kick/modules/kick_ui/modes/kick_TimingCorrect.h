/*
 * Copyright (c) 2024 MUSIC Tribe
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

#pragma once

namespace kick_ui
{
    // Timing Correct, the TC key (SHIFT + TC = ON/OFF).
    //
    // The release had no handler for either key. TC opens this window: NOTE VALUE is the selected
    // sequence's quantization (the same value the step sequencer's time division and the grid
    // use), STRENGTH how far notes move towards the grid. Apply stores the note value and, with
    // Timing Correct on, moves the selected track's notes. ON/OFF switches Timing Correct on or
    // off and lights the TC key while it is on. correct() is what live recording applies to a
    // captured note start.
    class TimingCorrect : public juce::Component
    {
    public:
        TimingCorrect();

        static bool isEnabled() noexcept { return enabled.load(); }
        static void setEnabled( bool On );   // ON/OFF; sends the TC LED and shows a notification
        static void showWindow();            // TC
        static int  correct( int StartTick ); // a recorded start snapped to the note value, when on

    private:
        ui::KickCheckBoxButton onOff{ "Timing Correct", "Timing Correct" };
        ui::KickListBox        noteValue{ ui::KickListBox::withTitleBar, "NOTE VALUE", noteValueNames(), 4 };
        ui::KickSlider         strength{ "STRENGTH(%)", juce::Slider::LinearBar, juce::Slider::TextBoxAbove };
        ui::KickLabel          info{ "" };

        static std::atomic<bool> enabled;
        static bool              windowOpen;

        static const std::vector<juce::String>& noteValueNames();
        static const std::vector<mt::MTTimeBase>& noteValueBases();

        int apply(); // returns the number of notes moved, or -1 without a sequence / track

        void paint( juce::Graphics& g ) override;
        void resized() override;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR( TimingCorrect )
    };
}
