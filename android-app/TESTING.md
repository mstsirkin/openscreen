# ControlCast Testing Guide

## Prerequisites

- Android device (arm64, API 29+) connected via USB with ADB debugging enabled
- Cast receiver (Chromecast/Google TV) on the same WiFi network
- A test video file pushed to the device:
  ```
  adb push /path/to/video.mp4 /data/local/tmp/test.mp4
  ```

## Keep screen on during testing

```bash
adb shell svc power stayon true
```

## Launch with intent extras

Basic cast test:
```bash
adb shell am start -n org.openscreen.controlcast/.MainActivity \
  --es test_target "<receiver_ip>:8009" \
  --es test_file "/data/local/tmp/test.mp4"
```

Fullscreen mode:
```bash
adb shell am start -n org.openscreen.controlcast/.MainActivity \
  --es test_target "<receiver_ip>:8009" \
  --es test_file "/data/local/tmp/test.mp4" \
  --ez test_fullscreen true
```

A/V sync calibration:
```bash
adb shell am start -n org.openscreen.controlcast/.MainActivity \
  --es test_target "<receiver_ip>:8009" \
  --ez test_calibrate true
```

## Test cases

### 1. Device discovery
- Launch app without intent extras
- Verify Cast devices appear in the list
- Tap Refresh — list should update

### 2. Basic casting
- Tap a Cast device
- Tap Open Video, select a video
- Verify video plays on TV
- Verify local preview shows in the app
- Verify slider advances with playback

### 3. Play/pause
- Tap Pause — both TV and local should pause
- Tap Play — both should resume
- Verify TV responds within ~400ms (playout delay)

### 4. Seek
- Drag slider while playing — TV should update within ~200ms
- Drag slider while paused — TV should show a preview frame
- Release slider — both local and TV should be at the same position

### 5. Fullscreen
- Tap the [ ] button on the video preview
- Verify video fills the screen
- Verify slider, Pause, Exit buttons are overlaid at bottom
- Verify pinch-to-zoom and drag-to-pan work
- Tap Exit — returns to normal view

### 6. Rotation
- While playing, rotate the device
- Verify video continues playing (both local and TV)
- Verify slider position is preserved (not reset to 0)
- Verify fullscreen state is preserved
- Rotate back — same checks

### 7. Share from Gallery
- Open Gallery/Photos app
- Share a video to "OpenScreen Control Cast"
- Verify the app opens with the video loaded

### 8. Hardware encoding
- Toggle "HW enc" switch OFF
- Start casting — should use software VP8 (lower quality, 480p)
- Toggle "HW enc" switch ON (takes effect on next cast session)
- Start casting — should use hardware H.264 (1080p)

### 9. A/V sync calibration
- Connect to a Cast device
- Tap "Cal" button (grant camera+mic permissions if prompted)
- Point phone camera at TV, ensure TV audio is audible
- Wait 16 seconds for calibration to complete
- Verify toast shows measured offset
- Verify A/V sync field is updated

### 10. Settings persistence
- Change buffer size (e.g., 200ms)
- Change A/V sync offset
- Toggle fullscreen
- Kill and restart the app
- Verify fullscreen state persists
- Verify buffer/sync values are available

## Monitoring

Check frame drops:
```bash
adb logcat -s OSP:* | grep -c "Dropping"
```

Check native logs:
```bash
adb logcat -s ControlCast:* OSP:* MediaCodecEnc:*
```

Check for crashes:
```bash
adb logcat -b crash | grep signal
```

## Building

Native library:
```bash
ninja -C out/android cast/standalone_sender:controlcast
cp out/android/libcontrolcast.so android-app/app/src/main/jniLibs/arm64-v8a/
```

APK:
```bash
cd android-app && gradle assembleDebug
```

Install:
```bash
adb install -r android-app/app/build/outputs/apk/debug/app-debug.apk
```
