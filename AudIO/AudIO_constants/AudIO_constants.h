/*******************************************************************************
 BEGIN_JUCE_MODULE_DECLARATION

  ID:                 AudIO_constants
  vendor:             MUSIC Tribe
  version:            0.1.0
  name:               AudIO constants (compatibility shim)
  description:        Parameter range constants Kick expects from the internal AudIO_constants module, plus a forward to the public AudIO module.
  website:
  license:            AGPLv3
  dependencies:       AudIO

 END_JUCE_MODULE_DECLARATION
*******************************************************************************/

/*
 * Copyright (c) 2022 MUSIC Tribe
 *
 * This file is part of AudIO.
 *
 * AudIO is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * WHY THIS FILE EXISTS
 *
 * Kick/modules/common/common.h includes <AudIO_constants/AudIO_constants.h>, a module
 * from the internal AudIO repository that was not part of the public release. Kick's
 * own `modules/common/source/kick_Constants.h` defines most parameter ranges (BPM, BAR,
 * SEMI, FINE, TRANSPOSE, MUTEGROUP, ...) but five groups, and one sentinel, were only
 * ever defined in AudIO_constants. They are reconstructed here.
 *
 * The VALUES below are inferred from how Kick and AudIO use them, not copied from the
 * original file. The reasoning for each one is the comment beside it (also tabled in
 * docs/DEVELOPMENT.md). If one turns out wrong, this header is the only place to fix it.
 */

#pragma once
#ifndef AUDIO_CONSTANTS_H_INCLUDE
    #define AUDIO_CONSTANTS_H_INCLUDE

    #include <AudIO/AudIO.h>

///
/// Engine geometry. These feed AudIO::DawEngine and the audio device.
///
/// AUDIO_BLOCK_SIZE must not exceed the AudIO memory-pool block (BLOCK_SIZE in Daw/DawEngine.cpp,
/// 480 samples = 10 ms at 48 kHz); DawEngine::startAudioDevice clamps to it anyway.
/// Submix count matches the fixed "Submix 1..4" list in the MIDI control UI.
///
    #define AUDIO_SAMPLE_RATE      ( 48000 ) // engine rate; AUDIO_BLOCK_SIZE is then 10 ms
    #define AUDIO_BLOCK_SIZE       ( 480 )   // must equal AudIO's memory-pool block (BLOCK_SIZE in Daw/DawEngine.cpp)
    #define AUDIO_NUM_INPUTS       ( 2 )     // stereo line input
    #define AUDIO_NUM_OUTPUTS      ( 2 )     // stereo main output
    #define AUDIO_NUM_SEND_RETURNS ( 4 )     // sends 1-4 are referenced by the mixer code
    #define AUDIO_NUM_SUBMIX       ( 4 )     // the MIDI control UI lists "Submix 1..4"
    #define AUDIO_NUM_AUX          ( 2 )     // a guess: nothing in the code pins it

/// Decibel floor used by meters. Matches AudIO::utils::linearTolog(), which returns -100 for silence.
namespace DB
{
    static constexpr float MIN{ -100.f }; // AudIO::utils::linearTolog() returns -100 for silence
    static constexpr float MAX{ 0.f };    // full scale
}

/// 7-bit MIDI value range. Used as the generic 0..127 range for levels, velocities and
/// note numbers all over the UI, e.g. `juce::jlimit( MIDI::MIN, MIDI::MAX, note )`.
namespace MIDI
{
    static constexpr int   MIN{ 0 };               // 7-bit MIDI
    static constexpr int   MAX{ 127 };             // 7-bit MIDI
    static constexpr int   NUMERIC_MAX{ 128 };     // exclusive upper bound, e.g. `for( i = MIN; i < NUMERIC_MAX; ... )`
    static constexpr int   MIDDLE_C{ 60 };         // MIDI note 60
    static constexpr int   NOTE_DEFAULT{ 60 };    // default note number of a new MIDI note event
    static constexpr int   STEP{ 1 };              // integer steps
    static constexpr int   DEFAULT{ 0 };           // default LFO/modulation amount; also used as a "continuous" slider interval
    static constexpr int   LEVEL_DEFAULT{ 100 };   // default level for pads/tracks and the upper bound of level Q-Link knobs
    static constexpr float SCALER{ 1.0f / 127.0f }; // MIDI value -> 0..1; source comment "(MidiLevel / 127)" in kick_common_utils.cpp
}

/// MIDI channel as shown to the user (1-based).
namespace MIDI_CH
{
    static constexpr int MIN{ 1 };     // shown 1-based in the UI
    static constexpr int MAX{ 16 };    // 16 MIDI channels
    static constexpr int STEP{ 1 };
    static constexpr int DEFAULT{ 1 }; // first channel
}

/// Voices per program. ISynthObject allocates POLYPHONY::MAX voices up front.
namespace POLYPHONY
{
    static constexpr int MIN{ 1 };      // at least one voice
    static constexpr int MAX{ 64 };     // allocated up front by ISynthObject, so kept modest
    static constexpr int STEP{ 1 };
    static constexpr int DEFAULT{ 16 }; // inferred, nothing pins it exactly
}

/// Linear gain fed to AudIO::Panner::setParameters( pan, gain ). Sends default to GAIN::MIN.
namespace GAIN
{
    static constexpr double MIN{ 0.0 };     // linear gain: silence
    static constexpr double MAX{ 1.0 };     // linear gain: unity
    static constexpr double STEP{ 0.01 };   // gain knobs move in 0.01 steps
    static constexpr double DEFAULT{ 1.0 }; // unity
    static constexpr double ROOT_2{ 0.7071067811865476 }; // -3 dB; default metronome gain (kick_settings.cpp)
}

/// Pan position. AudIO::Panner documents its range as -1.0 (left) .. 1.0 (right).
namespace PAN
{
    static constexpr double MIN{ -1.0 };    // AudIO::Panner: full left
    static constexpr double MAX{ 1.0 };     // AudIO::Panner: full right
    static constexpr double STEP{ 0.01 };   // same knob step as GAIN
    static constexpr double DEFAULT{ 0.0 }; // centre
}

/// Sentinel stored in a juce::Value that is not attached to anything yet
/// (see kick_ui_controls/source/kick_attachments.h).
static constexpr int UNKNOWN_VALUE{ -1 };

#endif // AUDIO_CONSTANTS_H_INCLUDE
