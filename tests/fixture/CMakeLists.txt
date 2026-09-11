# Test fixture: builds a known, free VST3 plug-in for the §8 headless test.
#
# The §8 test needs a plugin that is guaranteed present, free, deterministic, and
# has a gain-like parameter. Installed commercial plugins satisfy none of those —
# they may require licensing or pop dialogs at instantiation. The SDK's own
# "again-sample-accurate" gain example satisfies all of them, and unlike the plain
# "again" example it does not require VSTGUI.
#
# Configure/build separately from native/ — this exists only for testing.
#
#   cmake -S tests/fixture -B tests/fixture/build -A x64
#   cmake --build tests/fixture/build --config Release --target again-sample-accurate

cmake_minimum_required(VERSION 3.25)
project(NetPlugHostTestFixture)

set(SMTG_ENABLE_VSTGUI_SUPPORT        OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES  ON  CACHE BOOL "" FORCE)
# Symlinking into the system VST3 folder needs admin rights and would leak a test
# fixture into the user's real plugin list.
set(SMTG_CREATE_PLUGIN_LINK           OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../external/vst3sdk vst3sdk-build)
