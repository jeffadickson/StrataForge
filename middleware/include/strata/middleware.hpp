#pragma once

// STRATA-20 Flagship C++20 Bare-Metal Flight Instrument Middleware Umbrella Header
// Zero-heap, zero-vtable, zero-exception, NASA NPR 7150.2D Class B compatible.

#include <strata/core/concepts.hpp>
#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/fsm.hpp>
#include <strata/core/qos.hpp>
#include <strata/core/types.hpp>

#include <strata/ipc/blackboard.hpp>
#include <strata/ipc/critical_store.hpp>
#include <strata/ipc/event_ring.hpp>
#include <strata/ipc/nv_fault_log.hpp>
#include <strata/ipc/param_bank.hpp>
#include <strata/ipc/spsc_channel.hpp>

#include <strata/data/data_topic.hpp>
#include <strata/data/frame_lease.hpp>

#include <strata/hw/reg_map.hpp>
#include <strata/hw/slot_handle.hpp>

#include <strata/runtime/partition_runtime.hpp>
#include <strata/runtime/subtopology.hpp>
