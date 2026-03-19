# ControlCast Test Plan

## Environment

- Device under test: Samsung phone via `adb`
- Target: `GoogleTV4740`
- Verification:
  - app UI state via `uiautomator dump`
  - TV output via phone camera aimed at the TV

## Test Cases

### T1. Preselected file then connect

Steps:
1. Launch the app.
2. Pick a local video file through the file picker.
3. Return to the app through OpenScreen Control Cast.
4. Tap the TV device row.

Expected:
- Connection reaches `Connected`.
- App status includes the loaded file and active playback state.
- TV shows the selected video, not the home screen.

### T2. Open a different file while already connected

Steps:
1. Start from T1 with TV already showing a video.
2. Tap `Open Video`.
3. Pick a different local video file.
4. Return to the app through OpenScreen Control Cast.

Expected:
- Existing Cast session stays usable.
- App file label changes to the new file.
- TV switches to the new file instead of keeping the old one or dropping to home.

### T3. Paused file then connect

Steps:
1. Launch the app.
2. Pick a local file.
3. Pause locally before connecting.
4. Tap the TV device row.

Expected:
- Cast opens the file at the paused position.
- TV shows the paused frame for the selected file.
- App does not end up in `Connected ... | no video selected`.

### T4. End of file then seek back

Steps:
1. Play a short file to end of stream.
2. After EOF, seek back to an earlier position.

Expected:
- Phone preview moves back.
- TV also updates to the earlier frame / resumed playback state.
- Cast does not remain stuck at EOF.

### T5. Unexpected disconnect state reporting

Steps:
1. Start from T1 with active playback.
2. Cause transport loss or disconnect.

Expected:
- `Connection.state` leaves `CONNECTED`.
- UI no longer claims an active connected session once Cast is gone.

## Latest Execution Notes

### 2026-03-18 Samsung run

- T1: PASS
  - Verified by adb-driven picker flow, device-row tap, app UI showing `Connected`, and camera screenshot confirming the TV was showing the selected video instead of the home screen.
- T2: NOT YET VERIFIED
  - The ad-hoc picker automation became flaky when re-entering the picker from a connected session, so this needs a cleaner rerun.
- T3: NOT YET VERIFIED
  - Same reason as T2; needs a dedicated paused-connect run.
- T4: NOT YET VERIFIED
  - EOF native fix is implemented locally, but the current UI automation path still needs a reliable way to drive seek-back after EOF.
- T5: NOT YET VERIFIED
  - Not exercised in this run.
