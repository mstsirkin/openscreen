# Agent Rules

## Test Builds

- Before testing, commit the current state first.
- The `HEAD` git hash for the tested state must be included in the app in a way that is visible both:
  - to the user in the UI
  - through `adb`
- After that, continue by amending that commit until the result is satisfactory.
- Do not test uncommitted code when a result needs to be attributed to a specific build.
