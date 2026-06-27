// ai_block unit tests: context format, byte-budget trim, question-by-exit-code.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ai_block.h"

static int failures = 0;
static int total = 0;

#define CHECK(cond, msg) do {                                          \
    total++;                                                           \
    if (!(cond)) {                                                     \
        fprintf(stderr, "FAIL [%d] %s\n", total, msg);                 \
        failures++;                                                    \
    } else {                                                           \
        fprintf(stderr, "ok   [%d] %s\n", total, msg);                 \
    }                                                                  \
} while(0)

#define CHECK_STR_EQ(a, b, msg) CHECK(strcmp(a, b) == 0, msg)

static void test_context_format(void)
{
    char buf[4096];
    int n;

    n = ai_block_build_context("ls -la", "file1\nfile2\n", 0, buf, sizeof(buf));
    CHECK(n > 0, "build_context returns positive length for success");
    CHECK_STR_EQ(ai_block_default_question(0), "Explain this output.",
                 "default question for exit_code=0");
    CHECK(strstr(buf, "$ ls -la") != NULL, "context contains the command");
    CHECK(strstr(buf, "exited with status 0") != NULL, "context mentions exit code 0");
    CHECK(strstr(buf, "file1") != NULL, "context contains output");
    CHECK(strstr(buf, "file2") != NULL, "context contains full output");

    n = ai_block_build_context("git status", "modified: foo.c", 1, buf, sizeof(buf));
    CHECK(n > 0, "build_context for exit_code=1");
    CHECK(strstr(buf, "$ git status") != NULL, "context for git status");
    CHECK(strstr(buf, "exited with status 1") != NULL, "mentions status 1");
    CHECK_STR_EQ(ai_block_default_question(1), "Why did this command fail?",
                 "default question for exit_code>0");
}

static void test_context_unknown_exit(void)
{
    char buf[4096];

    ai_block_build_context("echo hi", NULL, -1, buf, sizeof(buf));
    CHECK(strstr(buf, "exited with status -1") != NULL, "unknown exit code shown");
    CHECK_STR_EQ(ai_block_default_question(-1), "Explain this command.",
                 "default question for exit_code<0");
}

static void test_output_null(void)
{
    char buf[4096];
    int n = ai_block_build_context("date", NULL, 0, buf, sizeof(buf));
    CHECK(n > 0, "null output is handled without crash");
    CHECK(strstr(buf, "$ date") != NULL, "null output: command still visible");
}

static void test_command_null(void)
{
    char buf[4096];
    int n = ai_block_build_context(NULL, "output", 0, buf, sizeof(buf));
    CHECK(n > 0, "null command is handled without crash");
}

static void test_context_trim(void)
{
    // Generate output that overflows a small buffer to force trimming.
    char big_out[2048];
    memset(big_out, 'x', sizeof(big_out) - 1);
    big_out[sizeof(big_out) - 1] = '\0';

    char small_buf[256];
    int n = ai_block_build_context("cmd", big_out, 0, small_buf, (int)sizeof(small_buf));
    CHECK(n > 0, "trimmed context still succeeds");
    CHECK(n < (int)sizeof(small_buf), "trimmed output fits in buffer");
    CHECK(strstr(small_buf, "$ cmd") != NULL, "trimmed: command visible");
    CHECK(strstr(small_buf, "exited with status 0") != NULL, "trimmed: exit code visible");

    // The output should be truncated (fewer 'x' chars than the original).
    // Count the x's in the result.
    int x_count = 0;
    for (int i = 0; small_buf[i]; i++)
        if (small_buf[i] == 'x') x_count++;
    CHECK(x_count < (int)sizeof(big_out) - 1, "output was actually trimmed");
    CHECK(x_count > 0, "some output survived trimming");
}

static void test_large_output_not_capped(void)
{
    // Output larger than the old fixed 4096-byte trim buffer, with a cap big
    // enough to hold well over 4096 bytes of it. The trim path must size to the
    // cap, not a fixed 4096 — regression test for the removed stack-buffer cap.
    char *big = (char *)malloc(20000);
    memset(big, 'x', 19999);
    big[19999] = '\0';

    char buf[10000];
    int n = ai_block_build_context("cmd", big, 0, buf, (int)sizeof(buf));
    CHECK(n > 0, "large output: build succeeds");
    CHECK(n < (int)sizeof(buf), "large output: fits in cap");

    int x_count = 0;
    for (int i = 0; buf[i]; i++)
        if (buf[i] == 'x') x_count++;
    CHECK(x_count > 5000, "large output: carries >5000 bytes (not capped at 4096)");

    free(big);
}

static void test_cap_too_small(void)
{
    char tiny[16];
    int n = ai_block_build_context("a-long-command-name", "output", 0, tiny, (int)sizeof(tiny));
    CHECK(n < 0, "returns -1 when cap too small for boilerplate");
}

int main(void)
{
    fprintf(stderr, "ai_block_tests:\n");
    test_context_format();
    test_context_unknown_exit();
    test_output_null();
    test_command_null();
    test_context_trim();
    test_large_output_not_capped();
    test_cap_too_small();

    fprintf(stderr, "\n%d / %d passed, %d failed\n",
            total - failures, total, failures);
    return failures > 0 ? 1 : 0;
}
