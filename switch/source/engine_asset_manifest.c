#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "engine_asset_manifest.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define MANIFEST_TEXT_NAME "asset-manifest.txt"
#define MANIFEST_JSON_NAME "asset-manifest.json"

// The two manifest files are metadata about the tree, not guest assets, so
// the observed counts exclude them.
static bool is_manifest_name(const char *name)
{
    return strcmp(name, MANIFEST_TEXT_NAME) == 0 ||
           strcmp(name, MANIFEST_JSON_NAME) == 0;
}

static void walk_tree(const char *path, unsigned long long *files,
                      unsigned long long *bytes, unsigned depth)
{
    if (depth > 32) return; // asset trees are shallow; never chase cycles
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[1024];
        const int length = snprintf(child, sizeof(child), "%s/%s", path,
                                    entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(child)) continue;
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_tree(child, files, bytes, depth + 1);
        } else if (S_ISREG(st.st_mode) && !is_manifest_name(entry->d_name)) {
            ++*files;
            *bytes += (unsigned long long)st.st_size;
        }
    }
    closedir(dir);
}

static bool parse_manifest(FILE *file, EngineAssetManifestReport *report)
{
    char line[128];
    bool have_format = false, have_files = false, have_bytes = false,
         have_hash = false;
    unsigned long long format = 0;
    while (fgets(line, sizeof(line), file)) {
        unsigned long long value;
        char hash[80];
        if (sscanf(line, "format %llu", &value) == 1) {
            format = value;
            have_format = true;
        } else if (sscanf(line, "file_count %llu", &value) == 1) {
            report->declared_files = value;
            have_files = true;
        } else if (sscanf(line, "total_bytes %llu", &value) == 1) {
            report->declared_bytes = value;
            have_bytes = true;
        } else if (sscanf(line, "combined_sha256 %79s", hash) == 1) {
            have_hash = strlen(hash) == 64;
        }
    }
    return have_format && format == 1 && have_files && have_bytes && have_hash;
}

bool engine_asset_manifest_verify(const char *asset_root,
                                  EngineAssetManifestReport *report)
{
    if (!asset_root || !report) return false;
    memset(report, 0, sizeof(*report));

    char manifest_path[1024];
    const int length = snprintf(manifest_path, sizeof(manifest_path), "%s/%s",
                                asset_root, MANIFEST_TEXT_NAME);
    if (length < 0 || (size_t)length >= sizeof(manifest_path)) return false;
    FILE *file = fopen(manifest_path, "r");
    if (!file) return false;
    report->manifest_present = true;
    report->manifest_valid = parse_manifest(file, report);
    fclose(file);
    if (!report->manifest_valid) return false;

    walk_tree(asset_root, &report->observed_files, &report->observed_bytes, 0);
    report->counts_match = report->declared_files == report->observed_files &&
                           report->declared_bytes == report->observed_bytes;
    return report->counts_match;
}
