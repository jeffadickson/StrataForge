# StrataForge Mock Instrument Framework (`instruments/`)

Zero-heap, zero-vtable, freestanding C++20 framework for rapidly constructing realistic mock spacecraft instruments for Host SIL, Verilator HIL, FlatSat, and live `StrataGround` I&T campaigns.

## Core Capabilities (`strata/instruments/mock_instrument.hpp`)
- **CRTP `MockInstrumentBase<Derived, FrameBytes, MaxFaults>`**: Zero-overhead instrument harness with mode FSM (`Off -> Boot -> Standby -> Safe -> Science -> Calibration -> Fault`), deterministic cadence stepping, standard command handler, and packed `InstrumentHousekeeping` telemetry.
- **Physical `ThermalPowerModel`**: Deterministic 1st-order thermal/power ODE (thermal capacitance, radiative sink, heater PWM, mode-dependent electronics dissipation, bus voltage & current draw, fault-injected thermal runaway and power anomalies).
- **SCSR-Compatible `InstrumentRegisterBlock`**: Directly satisfies `strata::RegisterTransport` with standard `MANIFEST_HASH`, RW control registers, RO telemetry mirrors, RW1C fault latches, and SEU bit-flip fault injection.
- **Deterministic `FaultInjector` & `DeterministicRng`**: Time-scheduled or immediate injection of `ThermalRunaway`, `RegisterSeuBitFlip`, `FrameCrcCorrupt`, `DmaStall`, `WatchdogFreeze`, `PowerBrownout`, `OverCurrent`, and `SensorSaturation`.
- **Zero-Copy `FramePool` Science Frame Emission**: Synthesizes radiometric/spectral/waveform frames with deterministic scene patterns and CRC-32C integrity footers directly into caller-owned zero-copy `FramePool` slots.

## Reference Instruments (`strata/instruments/reference_instruments.hpp`)
- **`HyperspectralImagerMock`**: Pushbroom VNIR/SWIR spectrometer with cryogenic detector thermal dynamics, integration-time control, calibration lamp, Gaussian point targets, and thermal/SEU fault hooks.
- **`FluxgateMagnetometerMock`**: Tri-axial vector magnetometer with orbital dipole wave synthesis, calibration coil mode, and magnetic anomaly injection.
