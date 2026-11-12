# cooler

A small Windows window for Corsair Commander Core coolers (the hub on
Elite Capellix / Elite LCD AIOs), so iCUE doesn't have to run. It sets
the pump and fan curves, with presets, and **saves them on the controller
itself**: it runs them on the liquid temperature with the app closed and
across restarts. Nothing needs to stay running.

It talks to the controller with Windows' HID API, using the protocol
liquidctl and FanControl.CorsairLink document.

- Presets Silent, Quiet, Balanced, Performance, or drag the points (up to
  7; double-click adds, right-click removes).
- Every curve keeps to safety rules: full speed by 50°C liquid at the
  latest, the pump never under 50%, never slower as it warms.
- Only connected fan ports are changed.
- Reads happen on opening, F5, and after saving. Each read puts the
  controller in software mode briefly, so **its lights blink once**
  (a known Commander Core quirk).
- It refuses to save while iCUE is running.

## Building

`build.bat` (Visual Studio 2022 C++ tools) builds `build\cooler.exe` and
runs the protocol tests. The tests also build anywhere:

    g++ -std=c++17 -Isrc tests/tests.cpp src/protocol.cpp -o tests && ./tests

`cooler.exe -dump <file>` reads everything once (raw endpoints included).
`cooler.exe -apply <preset> <file>` saves a preset with no window.
