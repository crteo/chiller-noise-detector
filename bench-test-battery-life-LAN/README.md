# Battery-life bench tests

This folder contains two self-contained bench tests for the existing
ESP32-S3 + INMP441 acoustic sensor:

- `4xAA/`: four AA cells feeding an LM2596 adjusted to 3.30 V.
- `PowerBank/`: a 20,000 mAh USB power bank operating in its low-current mode.

Both tests use the same workload so their results can be compared:

- a DS3231 Alarm 1 wake every 10 minutes;
- a two-second, 32 kHz acoustic capture;
- Stage 5b Hann-windowed FFT and A-weighted level calculation, plus peak dBFS;
- one compact HTTP/JSON measurement sent to an ESP32-S3 master;
- a local master dashboard at `http://192.168.4.1/`;
- Wi-Fi channel 11 for both master and sensor;
- no WAV creation or transmission; and
- deep sleep for the remainder of the interval.

Each subfolder contains its own sensor firmware, master receiver firmware,
serial CSV logger, runtime calculator, wiring instructions, and acceptance
criteria. Start with the README in the power-source folder being tested.

The dashboard is stored on the master and works entirely within its
`BatteryBench` access point. It does not require an internet connection or
cloud hosting. Upload both the master firmware and its LittleFS dashboard as
described in each test's README.

The transmitted SPL-like field is `estimated_dba`: the FFT-derived A-weighted
dBFS value plus the configured calibration offset. The calculation now matches
the Stage 5b weighting method, adapted to the bench test's existing 32 kHz,
two-second capture. It remains prototype instrumentation until validated
against a trusted sound-level reference.
