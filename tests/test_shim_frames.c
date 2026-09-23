/* Unit tests for the shim's minimal MCP frame awareness (shim_frames.h):
 * framing must match what the daemon reader (cbm_mcp_server_run) accepts, and
 * classification must find exactly what reconnect needs (request ids,
 * initialize, notifications/initialized), nothing more. */
#include "test_framework.h"
#include "daemon/shim_frames.h"

#include <string.h>

#define FMAX ((size_t)CBM_SHIM_FRAME_MAX)

TEST(shim_frame_newline_complete_and_partial) {
    cbm_shim_frame_t fr;
    const char *two = "{\"id\":1}\r\n{\"id\":2}\n";
    ASSERT_EQ(cbm_shim_frame_scan(two, strlen(two), FMAX, &fr), 1);
    ASSERT_EQ(fr.total, 10u);
    ASSERT_EQ(fr.body_off, 0u);
    ASSERT_EQ(fr.body_len, 8u); /* CR trimmed */
    ASSERT_EQ(fr.content_length, 0);
    ASSERT_EQ(cbm_shim_frame_scan(two + fr.total, strlen(two) - fr.total, FMAX, &fr), 1);
    ASSERT_EQ(fr.total, 9u);

    const char *partial = "{\"id\":3";
    ASSERT_EQ(cbm_shim_frame_scan(partial, strlen(partial), FMAX, &fr), 0);
    /* Unterminated line longer than the cap: stop framing, relay raw. */
    ASSERT_EQ(cbm_shim_frame_scan(partial, strlen(partial), 4, &fr), -1);
    PASS();
}

TEST(shim_frame_content_length) {
    cbm_shim_frame_t fr;
    const char *msg = "Content-Length: 8\r\nContent-Type: x\r\n\r\n{\"id\":1}NEXT";
    ASSERT_EQ(cbm_shim_frame_scan(msg, strlen(msg), FMAX, &fr), 1);
    ASSERT_EQ(fr.content_length, 1);
    ASSERT_EQ(fr.body_len, 8u);
    ASSERT_MEM_EQ(msg + fr.body_off, "{\"id\":1}", 8);
    ASSERT_EQ(fr.total, strlen(msg) - 4);

    /* Body not fully arrived yet; header split mid-prefix too. */
    ASSERT_EQ(cbm_shim_frame_scan(msg, strlen(msg) - 8, FMAX, &fr), 0);
    ASSERT_EQ(cbm_shim_frame_scan("Content-Le", 10, FMAX, &fr), 0);
    ASSERT_EQ(cbm_shim_frame_scan("Content-Length: 8\r\n", 19, FMAX, &fr), 0);

    /* Non-positive or oversized length: never guess a frame boundary. */
    const char *zero = "Content-Length: 0\r\n\r\n";
    ASSERT_EQ(cbm_shim_frame_scan(zero, strlen(zero), FMAX, &fr), -1);
    ASSERT_EQ(cbm_shim_frame_scan(msg, strlen(msg), 4, &fr), -1);
    PASS();
}

TEST(shim_msg_classifies_requests_notifications_responses) {
    cbm_shim_msg_t m;
    const char *init =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";
    cbm_shim_msg_inspect(init, strlen(init), &m);
    ASSERT_EQ(m.kind, CBM_SHIM_MSG_REQUEST);
    ASSERT_TRUE(m.is_initialize);
    ASSERT_STR_EQ(m.id, "1");

    const char *note = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    cbm_shim_msg_inspect(note, strlen(note), &m);
    ASSERT_EQ(m.kind, CBM_SHIM_MSG_NOTIFICATION);
    ASSERT_TRUE(m.is_initialized);
    ASSERT_STR_EQ(m.id, "");

    const char *call = "{\"jsonrpc\":\"2.0\",\"id\":\"req-7\",\"method\":\"tools/call\"}";
    cbm_shim_msg_inspect(call, strlen(call), &m);
    ASSERT_EQ(m.kind, CBM_SHIM_MSG_REQUEST);
    ASSERT_FALSE(m.is_initialize);
    ASSERT_STR_EQ(m.id, "\"req-7\"");

    const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":\"req-7\",\"result\":{}}";
    cbm_shim_msg_inspect(resp, strlen(resp), &m);
    ASSERT_EQ(m.kind, CBM_SHIM_MSG_RESPONSE);
    ASSERT_STR_EQ(m.id, "\"req-7\"");

    const char *junk = "not json";
    cbm_shim_msg_inspect(junk, strlen(junk), &m);
    ASSERT_EQ(m.kind, CBM_SHIM_MSG_OTHER);
    const char *batch = "[{\"id\":1,\"method\":\"ping\"}]";
    cbm_shim_msg_inspect(batch, strlen(batch), &m);
    ASSERT_EQ(m.kind, CBM_SHIM_MSG_OTHER);
    const char *nullid = "{\"id\":null,\"method\":\"ping\"}";
    cbm_shim_msg_inspect(nullid, strlen(nullid), &m);
    ASSERT_EQ(m.kind, CBM_SHIM_MSG_NOTIFICATION);
    PASS();
}

TEST(shim_lost_error_keeps_id_and_framing) {
    char out[1024];
    int n = cbm_shim_format_lost_error(out, sizeof(out), "\"req-7\"", 0);
    ASSERT_GT(n, 0);
    ASSERT_EQ(out[n - 1], '\n');
    ASSERT_NOT_NULL(strstr(out, "\"id\":\"req-7\""));
    ASSERT_NOT_NULL(strstr(out, "\"code\":-32000"));
    ASSERT_NOT_NULL(strstr(out, "daemon restarted"));

    /* The error must itself parse as a response to the same id. */
    cbm_shim_msg_t m;
    cbm_shim_msg_inspect(out, (size_t)n - 1, &m);
    ASSERT_EQ(m.kind, CBM_SHIM_MSG_RESPONSE);
    ASSERT_STR_EQ(m.id, "\"req-7\"");

    n = cbm_shim_format_lost_error(out, sizeof(out), "42", 1);
    ASSERT_GT(n, 0);
    cbm_shim_frame_t fr;
    ASSERT_EQ(cbm_shim_frame_scan(out, (size_t)n, FMAX, &fr), 1);
    ASSERT_EQ(fr.total, (size_t)n);
    ASSERT_EQ(fr.content_length, 1);

    ASSERT_EQ(cbm_shim_format_lost_error(out, 8, "42", 0), -1);
    ASSERT_EQ(cbm_shim_format_lost_error(out, sizeof(out), "", 0), -1);
    PASS();
}

SUITE(shim_frames) {
    RUN_TEST(shim_frame_newline_complete_and_partial);
    RUN_TEST(shim_frame_content_length);
    RUN_TEST(shim_msg_classifies_requests_notifications_responses);
    RUN_TEST(shim_lost_error_keeps_id_and_framing);
}
