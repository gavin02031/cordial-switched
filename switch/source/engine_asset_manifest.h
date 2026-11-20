#ifndef MORROW_ENGINE_ASSET_MANIFEST_H
#define MORROW_ENGINE_ASSET_MANIFEST_H

#include <stdbool.h>

// Verifies an extracted APK assets directory against the manifest written by
// switch/tools/extract_apk_assets.py. The check is deliberately cheap for
// console startup: manifest fields are parsed and the tree is counted and
// sized, but file contents are never hashed on device. A match means the
// right NUMBER of files and BYTES are present; it does not prove content
// identity (the manifest's combined_sha256 exists for host-side checks).
typedef struct {
    bool manifest_present;   // asset-manifest.txt exists and opened
    bool manifest_valid;     // all four required fields parsed, format == 1
    unsigned long long declared_files;
    unsigned long long declared_bytes;
    unsigned long long observed_files;
    unsigned long long observed_bytes;
    bool counts_match;       // manifest_valid and declared == observed
} EngineAssetManifestReport;

// Returns true only when the manifest is present, valid and both counts
// match the tree. A false return with manifest_present false means the
// extraction tool never ran (or the SD copy lost the manifest); a false
// return with counts_match false means a partial or stale copy.
bool engine_asset_manifest_verify(const char *asset_root,
                                  EngineAssetManifestReport *report);

#endif
