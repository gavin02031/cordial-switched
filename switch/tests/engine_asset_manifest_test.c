#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../source/engine_asset_manifest.h"

static char root[512];

static void make_dir(const char *relative)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    mkdir(path, 0777);
}

static void write_file(const char *relative, const char *contents)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    FILE *file = fopen(path, "wb");
    assert(file);
    fwrite(contents, 1, strlen(contents), file);
    fclose(file);
}

static void write_manifest(unsigned long long files, unsigned long long bytes)
{
    char body[256];
    snprintf(body, sizeof(body),
             "format 1\nfile_count %llu\ntotal_bytes %llu\n"
             "combined_sha256 %064d\n", files, bytes, 0);
    write_file("asset-manifest.txt", body);
    write_file("asset-manifest.json", "{}");
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/morrow-asset-manifest-%d", getpid());
    mkdir(root, 0777);

    // Missing manifest: present=false, everything else zero.
    EngineAssetManifestReport report;
    assert(!engine_asset_manifest_verify(root, &report));
    assert(!report.manifest_present && !report.manifest_valid);
    assert(!engine_asset_manifest_verify(NULL, &report));
    assert(!engine_asset_manifest_verify(root, NULL));

    // Matching tree: two guest files, manifests excluded from the counts.
    make_dir("shaders");
    write_file("shaders/a.frag", "frag");
    write_file("b.png", "pngbytes!");
    write_manifest(2, 4 + 9);
    assert(engine_asset_manifest_verify(root, &report));
    assert(report.manifest_present && report.manifest_valid);
    assert(report.declared_files == 2 && report.observed_files == 2);
    assert(report.declared_bytes == 13 && report.observed_bytes == 13);
    assert(report.counts_match);

    // A partial copy (declared count above observed) must not pass.
    write_manifest(3, 4 + 9);
    assert(!engine_asset_manifest_verify(root, &report));
    assert(report.manifest_valid && !report.counts_match);

    // An extra unmanifested file must not pass either.
    write_file("extra.bin", "x");
    write_manifest(2, 4 + 9);
    assert(!engine_asset_manifest_verify(root, &report));

    // A corrupt manifest fails valid but stays present.
    char extra_path[600];
    snprintf(extra_path, sizeof(extra_path), "%s/extra.bin", root);
    assert(remove(extra_path) == 0);
    write_file("asset-manifest.txt", "not a manifest\n");
    assert(!engine_asset_manifest_verify(root, &report));
    assert(report.manifest_present && !report.manifest_valid);

    // A stale-format manifest is invalid.
    write_file("asset-manifest.txt",
               "format 2\nfile_count 2\ntotal_bytes 13\n"
               "combined_sha256 "
               "0000000000000000000000000000000000000000000000000000000000000000\n");
    assert(!engine_asset_manifest_verify(root, &report));
    assert(!report.manifest_valid);

    puts("engine_asset_manifest_test: OK");
    return 0;
}
