cmake_minimum_required(VERSION 3.22.1)

project(controlcast)

# The JNI source lives in the openscreen tree alongside the rest of the
# standalone sender code.  When building WITHOUT the full Open Screen
# library, a status-only stub is compiled (HAVE_OPENSCREEN is not defined).
# To build with the real Cast sender, use the GN target instead:
#   gn gen out/android --args='target_os="android" ...'
#   ninja -C out/android controlcast

set(OPENSCREEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../openscreen")

add_library(
    controlcast
    SHARED
    "${OPENSCREEN_ROOT}/cast/standalone_sender/controlcast_jni.cpp"
)

find_library(
    log-lib
    log
)

target_link_libraries(
    controlcast
    ${log-lib}
)
