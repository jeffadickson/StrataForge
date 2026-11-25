# Verification & Testing (`tests/`)

- `unit/` — Zero-dependency C++20 unit tests compiled with the strict Class B profile (`-fno-exceptions -fno-rtti -Werror -fstack-usage`).
- `rule_fuzzer/` — Deterministic seeded property/rule-based testing harness (F' `STest` parity) exercising command storms, SEU partition restarts, and lease reclamation.
- `verilator_hil/` — Flight C++20 instrument code running live against SystemVerilog `SCSR` & DMA RTL in Verilator.
