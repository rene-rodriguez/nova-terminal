// ui_toast unit tests
#include "../src/ui_toast.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int failures = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [line %d]: %s\n", __LINE__, msg); \
        failures++; \
    } \
} while(0)

#define ASSERT_NEAR(a, b, tol, msg) do { \
    if (fabs((double)(a) - (double)(b)) > (tol)) { \
        fprintf(stderr, "FAIL [line %d]: %s (%f vs %f)\n", __LINE__, msg, (double)(a), (double)(b)); \
        failures++; \
    } \
} while(0)

static void test_empty(void)
{
    ASSERT(toast_count() == 0, "count should be 0 initially");
    ToastLevel l;
    const char *m;
    float a;
    ASSERT(!toast_get(0, &l, &m, &a), "get(0) on empty should fail");
}

static void test_push_pop(void)
{
    toast_push(TOAST_INFO, "hello");
    ASSERT(toast_count() == 1, "count should be 1 after push");
    ToastLevel l;
    const char *m;
    float a;
    ASSERT(toast_get(0, &l, &m, &a), "get(0) should succeed");
    ASSERT(l == TOAST_INFO, "level should be TOAST_INFO");
    ASSERT(m && strcmp(m, "hello") == 0, "msg should match");
    ASSERT_NEAR(a, 1.0f, 0.01f, "alpha should be 1.0");
    toast_clear();
    ASSERT(toast_count() == 0, "count should be 0 after clear");
}

static void test_ttl_expiry(void)
{
    toast_push(TOAST_WARN, "warning");
    ASSERT(toast_count() == 1, "count 1 after push");
    // Advance past TTL
    toast_tick(TOAST_TTL_WARN + 0.5);
    ASSERT(toast_count() == 0, "count should be 0 after TTL expiry");
    toast_clear();
}

static void test_alpha_fade(void)
{
    toast_push(TOAST_ERROR, "error");
    // Half TTL
    toast_tick(TOAST_TTL_ERROR / 2.0);
    float a;
    ASSERT(toast_get(0, NULL, NULL, &a), "get should succeed");
    ASSERT_NEAR(a, 0.5f, 0.05f, "alpha should be ~0.5 at half TTL");
    toast_clear();
}

static void test_over_capacity(void)
{
    toast_clear();
    // Fill ring + 1
    int over = 20;
    for (int i = 0; i < over; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "msg %d", i);
        toast_push(TOAST_INFO, buf);
    }
    // Should have TOAST_RING entries, oldest 4 should be gone
    ASSERT(toast_count() == 16, "ring should cap at TOAST_RING");
    // Newest should be msg 19
    const char *m;
    ASSERT(toast_get(0, NULL, &m, NULL), "get newest");
    ASSERT(m && strcmp(m, "msg 19") == 0, "newest should be msg 19");
    // Oldest available should be msg 4 (since 0-3 were dropped)
    ASSERT(toast_get(15, NULL, &m, NULL), "get oldest");
    ASSERT(m && strcmp(m, "msg 4") == 0, "oldest should be msg 4");
    toast_clear();
}

static void test_newest_first(void)
{
    toast_clear();
    toast_push(TOAST_INFO, "first");
    toast_push(TOAST_ERROR, "second");
    const char *m;
    ASSERT(toast_get(0, NULL, &m, NULL), "get newest");
    ASSERT(m && strcmp(m, "second") == 0, "newest should be second");
    ASSERT(toast_get(1, NULL, &m, NULL), "get oldest");
    ASSERT(m && strcmp(m, "first") == 0, "oldest should be first");
    toast_clear();
}

int main(void)
{
    test_empty();
    test_push_pop();
    test_ttl_expiry();
    test_alpha_fade();
    test_over_capacity();
    test_newest_first();

    if (failures > 0)
        fprintf(stderr, "\n%d test(s) FAILED\n", failures);
    else
        printf("All toast_tests PASSED\n");
    return failures ? 1 : 0;
}
