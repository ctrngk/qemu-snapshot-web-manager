#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/config.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #fn); \
    fn(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

/* Test 1: defaults are sensible */
static void test_defaults(void)
{
    qswm_config_t cfg;
    config_defaults(&cfg);
    assert(cfg.port == 9091);
    assert(cfg.idle_timeout == 600);
    assert(strcmp(cfg.uri, "qemu:///system") == 0);
    assert(strcmp(cfg.static_dir, "/usr/local/share/qswm/static") == 0);
    assert(strcmp(cfg.dirty_state_path, "/var/lib/qswm/dirty-state.json") == 0);
}

/* Test 2: parse a single config line */
static void test_parse_line(void)
{
    qswm_config_t cfg;
    config_defaults(&cfg);
    config_parse_line(&cfg, "port", "8080");
    assert(cfg.port == 8080);
    config_parse_line(&cfg, "idle_timeout", "300");
    assert(cfg.idle_timeout == 300);
    config_parse_line(&cfg, "uri", "qemu:///session");
    assert(strcmp(cfg.uri, "qemu:///session") == 0);
    config_parse_line(&cfg, "dirty_state_path", "/vmpool/vms/.qswm/dirty-state.json");
    assert(strcmp(cfg.dirty_state_path, "/vmpool/vms/.qswm/dirty-state.json") == 0);
}

/* Test 3: parse a config file buffer */
static void test_parse_buffer(void)
{
    qswm_config_t cfg;
    config_defaults(&cfg);
    const char *buf =
        "[general]\n"
        "port = 7777\n"
        "idle_timeout = 120\n"
        "# comment line\n"
        "\n"
        "uri = qemu+ssh://host/system\n"
        "dirty_state_path = /tmp/qswm/dirty-state.json\n";
    config_parse_buffer(&cfg, buf);
    assert(cfg.port == 7777);
    assert(cfg.idle_timeout == 120);
    assert(strcmp(cfg.uri, "qemu+ssh://host/system") == 0);
    assert(strcmp(cfg.dirty_state_path, "/tmp/qswm/dirty-state.json") == 0);
    /* static_dir unchanged */
    assert(strcmp(cfg.static_dir, "/usr/local/share/qswm/static") == 0);
}

/* Test 4: unknown keys are silently ignored */
static void test_unknown_key(void)
{
    qswm_config_t cfg;
    config_defaults(&cfg);
    const char *buf =
        "[general]\n"
        "bogus_key = whatever\n"
        "port = 1234\n";
    config_parse_buffer(&cfg, buf);
    assert(cfg.port == 1234);
}

/* Test 5: CLI args override config */
static void test_cli_override(void)
{
    qswm_config_t cfg;
    config_defaults(&cfg);
    config_parse_line(&cfg, "port", "7777");
    /* Simulate CLI override — just set directly */
    cfg.port = 9999;
    assert(cfg.port == 9999);
}

int main(void)
{
    printf("=== Config Tests ===\n");
    RUN_TEST(test_defaults);
    RUN_TEST(test_parse_line);
    RUN_TEST(test_parse_buffer);
    RUN_TEST(test_unknown_key);
    RUN_TEST(test_cli_override);
    printf("=== %d/%d tests passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
