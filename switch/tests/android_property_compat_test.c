#include "android_property_compat.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const AndroidPresentedProperty sample[] = {
    { "ro.build.version.sdk", "33", "test fixture" },
    { "ro.product.model", "HAC-001", "test fixture" },
    { "persist.sys.timezone", "America/Chicago", "test fixture" },
};

int main(void)
{
    // The production table ships EMPTY until a console run observes real
    // queries; the provider must keep answering bionic's not-found result.
    assert(android_presented_properties_count == 0);
    assert(!android_property_find(android_presented_properties,
                                  android_presented_properties_count,
                                  "ro.build.version.sdk"));

    // The pure lookup core is exercised with test-owned rows.
    const AndroidPresentedProperty *hit =
        android_property_find(sample, 3, "ro.product.model");
    assert(hit && !strcmp(hit->value, "HAC-001") && hit->provenance);
    assert(!android_property_find(sample, 3, "ro.product.brand"));
    assert(!android_property_find(sample, 3, ""));
    assert(!android_property_find(sample, 3, NULL));
    assert(!android_property_find(NULL, 0, "ro.product.model"));
    // First match wins; duplicates are a table-authoring bug the test sees.
    const AndroidPresentedProperty dup[] = {
        { "a", "first", "fixture" },
        { "a", "second", "fixture" },
    };
    assert(!strcmp(android_property_find(dup, 2, "a")->value, "first"));

    puts("android_property_compat_test: empty production table and lookup core passed");
    return 0;
}
