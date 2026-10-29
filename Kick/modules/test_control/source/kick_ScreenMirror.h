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

namespace testcontrol
{
    /// An opt-in way to show and touch the app's screen from another program, for an emulator or
    /// a remote check: KICK_SCREEN_MIRROR=<unix socket path>.
    ///
    /// Without the variable nothing is created and the window is the usual native one (on the unit:
    /// the whole display). With it, the main window gets no native window at all. Its peer is a
    /// headless one (createPeer) that
    ///
    /// - renders the window, everything on it included (popups, the spinner, bubbles: they are its
    ///   children, ui::overlayRoot()), with JUCE's software renderer, the renderer the unit uses,
    ///   when JUCE asks it to repaint, at most 30 times a second and only while a viewer is
    ///   connected;
    /// - serves the result on <unix socket path> as an RFB 3.8 ("VNC") server: no authentication,
    ///   raw encoding, only the areas that changed, one viewer at a time (a new one replaces the
    ///   old). The socket is created with mode 0600, never on a network;
    /// - turns the viewer's pointer into touches: button 1 down, moves while it is down, button 1
    ///   up go to the peer's handleMouseEvent, as a native window's events do (moves with no button
    ///   held are ignored: a touch panel has no hover), so JUCE dispatches
    ///   them to the component under the point, its mouse listeners and its parents' (list rows,
    ///   drags, sliders) exactly as a finger on the unit's screen.
    ///
    /// Because nothing depends on the operating system's windows, this works with the computer's
    /// screen locked, and nothing of the app appears on the desktop (on macOS its Dock icon is
    /// hidden too). Keyboard events from the viewer are ignored: keys are the control surface's
    /// job and arrive over the serial port. Log lines start with "ScreenMirror: ".
    struct ScreenMirror
    {
        /// True when KICK_SCREEN_MIRROR is set (read once).
        static bool requested();

        /// A headless peer serving Window, if requested() (call from Window's createNewPeer).
        /// Null otherwise, or if the socket cannot be created (logged; the caller then makes the
        /// usual native peer).
        static juce::ComponentPeer* createPeer( juce::Component& Window, int StyleFlags );

        /// True if C is on a ScreenMirror peer (it has no native window and no display to fill).
        static bool isMirrored( const juce::Component& C );
    };
} // namespace testcontrol
