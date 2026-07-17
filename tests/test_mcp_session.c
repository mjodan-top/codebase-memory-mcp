#include "test_framework.h"
#include "mcp/mcp.h"

#include <stdlib.h>

TEST(mcp_session_lifecycle_is_per_connection) {
    cbm_mcp_core_t *core = cbm_mcp_core_new(NULL);
    cbm_mcp_server_t *a = cbm_mcp_server_new_with_core(core);
    cbm_mcp_server_t *b = cbm_mcp_server_new_with_core(core);
    ASSERT_NOT_NULL(core);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    cbm_mcp_core_free(core); /* servers now own the remaining references */
    ASSERT_TRUE(cbm_mcp_server_store(a) == cbm_mcp_server_store(b));
    ASSERT_EQ(cbm_mcp_server_session_phase(a), CBM_MCP_SESSION_NEW);
    ASSERT_EQ(cbm_mcp_server_session_phase(b), CBM_MCP_SESSION_NEW);

    char *resp = cbm_mcp_server_handle(
        a, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
    ASSERT_NOT_NULL(resp);
    free(resp);
    ASSERT_EQ(cbm_mcp_server_session_phase(a), CBM_MCP_SESSION_INITIALIZING);
    ASSERT_EQ(cbm_mcp_server_session_phase(b), CBM_MCP_SESSION_NEW);

    resp =
        cbm_mcp_server_handle(a, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    ASSERT_NULL(resp);
    ASSERT_EQ(cbm_mcp_server_session_phase(a), CBM_MCP_SESSION_READY);
    ASSERT_EQ(cbm_mcp_server_session_phase(b), CBM_MCP_SESSION_NEW);

    cbm_mcp_server_free(a);
    cbm_mcp_server_free(b);
    PASS();
}

TEST(mcp_shared_core_survives_connection_release) {
    cbm_mcp_core_t *core = cbm_mcp_core_new(NULL);
    ASSERT_NOT_NULL(core);
    cbm_mcp_server_t *a = cbm_mcp_server_new_with_core(core);
    cbm_mcp_server_t *b = cbm_mcp_server_new_with_core(core);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    cbm_mcp_core_free(core);
    cbm_store_t *shared = cbm_mcp_server_store(b);
    ASSERT_NOT_NULL(shared);
    ASSERT_TRUE(cbm_mcp_server_store(a) == shared);

    cbm_mcp_server_free(a);
    ASSERT_TRUE(cbm_mcp_server_store(b) == shared);
    char *resp = cbm_mcp_server_handle(b, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}");
    ASSERT_NOT_NULL(resp);
    free(resp);

    cbm_mcp_server_free(b);
    PASS();
}

SUITE(mcp_session) {
    RUN_TEST(mcp_session_lifecycle_is_per_connection);
    RUN_TEST(mcp_shared_core_survives_connection_release);
}
