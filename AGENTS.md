# Agent Rules

## build
For android build gradle from android-app

## Test Builds

- Before testing, commit the current state first.
- The `HEAD` git hash for the tested state must be included in the app in a way that is visible both:
  - to the user in the UI
  - through `adb`
- After that, continue by amending that commit until the result is satisfactory.
- Do not test uncommitted code when a result needs to be attributed to a specific build.

## Testing

- test with actual taps by preference
- Always take screenshot before and after you tap / scroll
- important: scroll until needed control is on screen!
- figure coords from screenshot
- Check tv state using phone camera - it is looking at the phone:
  - take a couple of shots to see if it is moving.
  - note: at end of video it will not move so you have to
    act quickly and if not moving check that!
  - even video if necessary
  - you can check video length for EOF
