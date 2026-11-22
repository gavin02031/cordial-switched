#ifndef CORDIAL_SWITCH_MORROW_INPUT_BIND_H
#define CORDIAL_SWITCH_MORROW_INPUT_BIND_H

#include <stdbool.h>
#include <stdint.h>

#include "engine_entry_probe.h"
#include "engine_memory_plan.h"
#include "morrow_input_pump.h"

// Translate the entry probe's discovered gamepad natives into pump entries.
// Pure address translation through the validated RX memory plan: this binds
// function pointers but invokes nothing. Axis and supported-motion natives
// are deliberately NOT bound; their emit paths stay inactive (see the
// import census, input section: s0/s1 roles unverified).
//
// A native whose export the probe did not find, or whose address does not
// translate into an RX run, stays null; the pump's per-emit null checks
// keep that path refused. Binding therefore says nothing about guest
// readiness, lifetime, thread or device gates - activation still requires
// every morrow_input_pump gate independently.
bool morrow_input_bind_from_probe(morrow_input_pump_entries_t *entries,
                                  const EngineEntryProbe *probe,
                                  const EngineMemoryPlan *plan,
                                  uint64_t runtime_base);

#endif
