# Registers AudIO as JUCE modules so that Kick can consume it.
#
# Kick's 3rdParty.cmake does `include(${AudIO_SOURCE_DIR}/juce_modules.cmake)` after
# fetching (or being pointed at) this directory. Kick's own modules depend on two
# module names that the internal AudIO repository used to provide:
#
#   AudIO_juce       -> included by Kick/modules/audio_interface
#   AudIO_constants  -> included by Kick/modules/common
#
# The public AudIO release ships a single JUCE module, `AudIO/AudIO.h`. The two
# directories next to it (`AudIO_juce/`, `AudIO_constants/`) are thin header-only
# modules that forward to it, so Kick builds without source changes.

juce_add_modules(
    ${CMAKE_CURRENT_LIST_DIR}/AudIO
    ${CMAKE_CURRENT_LIST_DIR}/AudIO_constants
    ${CMAKE_CURRENT_LIST_DIR}/AudIO_juce
)
