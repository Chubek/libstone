#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOMIOS_PLUGIN_ABI_MAJOR 1u
#define DOMIOS_PLUGIN_ABI_MINOR 0u
#define DOMIOS_PLUGIN_ABI_PATCH 0u

typedef struct DomIOSVersion {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} DomIOSVersion;

typedef enum DomIOSEventKind {
    DOMIOS_EVENT_NONE = 0,
    DOMIOS_EVENT_LOG = 1,
    DOMIOS_EVENT_DOMAINLET_ERROR = 2,
    DOMIOS_EVENT_PLUGIN_DIAGNOSTIC = 3
} DomIOSEventKind;

typedef struct DomIOSEvent {
    uint32_t kind;
    uint32_t code;
    const char* subject;
    const char* message;
} DomIOSEvent;

typedef struct DomIOSCapabilitySet {
    const void* control_plane;
    const void* event_queue;
    const void* graphics_backend;
} DomIOSCapabilitySet;

typedef struct DomIOSHostAPI {
    DomIOSVersion abi;
    void (*log)(const char* message);
    int (*enqueue_event)(const DomIOSEvent* event);
    uint64_t (*monotonic_time_ns)(void);
} DomIOSHostAPI;

typedef struct DomIOSPluginDescriptor {
    DomIOSVersion abi;
    const char* name;
    const char* version;
    int (*initialize)(const DomIOSHostAPI* host, void** state);
    void (*shutdown)(void* state);
    int (*set_capabilities)(void* state, const DomIOSCapabilitySet* caps);
} DomIOSPluginDescriptor;

typedef const DomIOSPluginDescriptor* (*DomIOSPluginEntry)(void);

#ifdef __cplusplus
}
#endif

