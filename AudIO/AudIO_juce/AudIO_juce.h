/*******************************************************************************
 BEGIN_JUCE_MODULE_DECLARATION

  ID:                 AudIO_juce
  vendor:             MUSIC Tribe
  version:            0.1.0
  name:               AudIO JUCE glue (reconstructed)
  description:        JUCE-backed audio device, envelope and recorder objects for AudIO, in namespace mt, as Kick expects.
  website:
  license:            AGPLv3
  dependencies:       AudIO, AudIO_constants, juce_core, juce_events, juce_data_structures, juce_audio_basics, juce_audio_devices, juce_audio_formats, juce_audio_utils

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
 * WHY THIS MODULE EXISTS
 *
 * Kick/modules/audio_interface includes <AudIO_juce/AudIO_juce.h>, a module from the
 * internal AudIO repository that was not part of the public release. The Kick engine
 * uses these things from it, all in namespace mt:
 *
 *   mt::AudioDevice     an AudIO::IAudioDevice backed by juce::AudioDeviceManager
 *   mt::ADSR            a per-sample envelope generator used by the drum/keygroup voices
 *   mt::Filter          a stereo state-variable filter (and the FilterType enum) used by the voices
 *   mt::Recorder        an AudIO object that records its input bus into a juce::AudioBuffer
 *   mt::SampleRecorder  Recorder used by the sampler screen
 *   mt::Looper          Recorder with an insert chain, used by the looper screen
 *
 * and it also relied on this header to bring the JUCE audio modules into scope.
 *
 * Everything in source/ is a reconstruction written against how Kick calls it, not the
 * original code. See docs/DEVELOPMENT.md.
 */

#pragma once
#ifndef AUDIO_JUCE_H_INCLUDE
    #define AUDIO_JUCE_H_INCLUDE

    #include <juce_core/juce_core.h>
    #include <juce_events/juce_events.h>
    #include <juce_data_structures/juce_data_structures.h>
    #include <juce_audio_basics/juce_audio_basics.h>
    #include <juce_audio_devices/juce_audio_devices.h>
    #include <juce_audio_formats/juce_audio_formats.h>
    #include <juce_audio_utils/juce_audio_utils.h>

    #include <AudIO/AudIO.h>
    #include <AudIO_constants/AudIO_constants.h>

    #include "source/mt_ADSR.h"
    #include "source/mt_Filter.h"
    #include "source/mt_AudioDevice.h"
    #include "source/mt_Recorder.h"

#endif // AUDIO_JUCE_H_INCLUDE
