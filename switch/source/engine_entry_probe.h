#ifndef CORDIAL_SWITCH_ENGINE_ENTRY_PROBE_H
#define CORDIAL_SWITCH_ENGINE_ENTRY_PROBE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { ENGINE_ACTIVITY_LIFECYCLE_STAGES = 9 };

// The Android Application.ActivityLifecycleCallbacks order used by Roblox's
// ActivityNativeMain shell; these are export names, not Java event emulation.
const char *engine_activity_lifecycle_stage_name(unsigned index);

typedef enum {
    ENGINE_ENTRY_INVALID = 0,
    ENGINE_ENTRY_IO_ERROR,
    ENGINE_ENTRY_NO_DYNAMIC_SYMBOLS,
    ENGINE_ENTRY_INSPECTED
} EngineEntryProbeStatus;

typedef struct {
    EngineEntryProbeStatus status;
    bool has_jni_onload;
    bool has_game_activity_initialize_native;
    bool has_native_activity_oncreate;
    bool has_app_bridge_global_init;
    bool has_app_bridge_update_adapter_init;
    bool has_app_bridge_v2_init;
    bool has_app_bridge_lua_shell_start;
    bool has_app_bridge_surface_update;
    bool has_app_bridge_start_app;
    bool has_app_bridge_main_thread_messages;
    // Gamepad input natives (class NativeInputInterface), discovered like
    // every other export: bytes only, never executed by this probe. The
    // first four feed the gated input wiring (morrow_input_pump); the last
    // two are catalogued but their emit paths stay inactive (see the
    // import census, input section).
    bool has_gamepad_connect;
    bool has_gamepad_disconnect;
    bool has_gamepad_set_supported_key;
    bool has_gamepad_button_event;
    bool has_gamepad_axis_event;
    bool has_gamepad_set_supported_motion;
    bool has_activity_lifecycle[ENGINE_ACTIVITY_LIFECYCLE_STAGES];
    bool has_init;
    bool has_init_array;
    uint64_t jni_onload_virtual_address;
    uint64_t game_activity_initialize_native_virtual_address;
    uint64_t native_activity_virtual_address;
    uint64_t app_bridge_global_init_virtual_address;
    uint64_t app_bridge_update_adapter_init_virtual_address;
    uint64_t app_bridge_v2_init_virtual_address;
    uint64_t app_bridge_lua_shell_start_virtual_address;
    uint64_t app_bridge_surface_update_virtual_address;
    uint64_t app_bridge_start_app_virtual_address;
    uint64_t app_bridge_main_thread_messages_virtual_address;
    uint64_t gamepad_connect_virtual_address;
    uint64_t gamepad_disconnect_virtual_address;
    uint64_t gamepad_set_supported_key_virtual_address;
    uint64_t gamepad_button_event_virtual_address;
    uint64_t gamepad_axis_event_virtual_address;
    uint64_t gamepad_set_supported_motion_virtual_address;
    uint64_t activity_lifecycle_virtual_address[ENGINE_ACTIVITY_LIFECYCLE_STAGES];
    uint64_t init_virtual_address;
    uint64_t init_array_virtual_address;
    unsigned init_array_entries;
    char detail[112];
} EngineEntryProbe;

// Read-only ARM64 ELF entry discovery. Calls no guest code and never guesses
// an exported function from an import or from a truncated section table.
void engine_entry_probe_stream(FILE *file, EngineEntryProbe *result);

#endif
