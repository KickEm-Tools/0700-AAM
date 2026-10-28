# Small console tests for the control-surface link. Not part of the Kick app; build them with
#   cmake --build Kick/build --target kick_protocol_roundtrip kick_serialport_pty_test
# and run them with ctest (cd Kick/build && ctest --output-on-failure -R kick_), or directly.
# See docs/DEVELOPMENT.md, section 8.

# Every control-surface packet type through CPktMgr: encode, decode, CRC, the app's reader.
juce_add_console_app(kick_protocol_roundtrip PRODUCT_NAME kick_protocol_roundtrip VERSION 1.0.0)
target_sources(kick_protocol_roundtrip PRIVATE protocol_roundtrip.cpp)
target_include_directories(kick_protocol_roundtrip PRIVATE ${CMAKE_SOURCE_DIR}/modules)
target_compile_definitions(kick_protocol_roundtrip PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(kick_protocol_roundtrip PRIVATE PktMgr juce::juce_core juce::juce_recommended_config_flags)
set_target_properties(kick_protocol_roundtrip PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME kick_protocol_roundtrip COMMAND kick_protocol_roundtrip)

# juce_serialport against a pseudo-terminal pair.
if(NOT WIN32)
    juce_add_console_app(kick_serialport_pty_test PRODUCT_NAME kick_serialport_pty_test VERSION 1.0.0)
    target_sources(kick_serialport_pty_test PRIVATE serialport_pty_test.cpp)
    target_compile_definitions(kick_serialport_pty_test PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(kick_serialport_pty_test PRIVATE juce_serialport juce::juce_core juce::juce_events juce::juce_recommended_config_flags)
    set_target_properties(kick_serialport_pty_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
    add_test(NAME kick_serialport_pty_test COMMAND kick_serialport_pty_test)
endif()
