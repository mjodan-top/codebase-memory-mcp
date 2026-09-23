#include "daemon/shim_frames.h"

#include <yyjson/yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char CL_PREFIX[] = "Content-Length:";

/* Content-Length framed frame starting at buf[0]. Header parsing matches the
 * daemon: the first line carries the length, then lines are consumed until
 * one that is empty after trimming CR/LF. */
static int scan_content_length(const char *buf, size_t len, size_t max_frame,
                               cbm_shim_frame_t *out) {
    size_t pos = 0;
    long long body_len = -1;
    int first = 1;
    for (;;) {
        const char *nl = memchr(buf + pos, '\n', len - pos);
        if (!nl) {
            return (len > CBM_SHIM_FRAME_HEADER_MAX) ? -1 : 0;
        }
        size_t line_end = (size_t)(nl - buf); /* index of '\n' */
        size_t content_end = line_end;
        while (content_end > pos && (buf[content_end - 1] == '\r' || buf[content_end - 1] == '\n')) {
            content_end--;
        }
        if (first) {
            char num[32];
            size_t start = pos + sizeof(CL_PREFIX) - 1;
            size_t nlen = content_end > start ? content_end - start : 0;
            if (nlen >= sizeof(num)) {
                return -1;
            }
            memcpy(num, buf + start, nlen);
            num[nlen] = '\0';
            body_len = strtoll(num, NULL, 10);
            first = 0;
        } else if (content_end == pos) {
            pos = line_end + 1;
            break; /* blank line: body follows */
        }
        pos = line_end + 1;
        if (pos > CBM_SHIM_FRAME_HEADER_MAX) {
            return -1;
        }
    }
    if (body_len <= 0 || (size_t)body_len > max_frame) {
        return -1;
    }
    if (len - pos < (size_t)body_len) {
        return 0;
    }
    out->total = pos + (size_t)body_len;
    out->body_off = pos;
    out->body_len = (size_t)body_len;
    out->content_length = 1;
    return 1;
}

int cbm_shim_frame_scan(const char *buf, size_t len, size_t max_frame, cbm_shim_frame_t *out) {
    if (!buf || !out || len == 0) {
        return 0;
    }
    size_t plen = sizeof(CL_PREFIX) - 1;
    size_t cmp = len < plen ? len : plen;
    if (memcmp(buf, CL_PREFIX, cmp) == 0) {
        if (len < plen) {
            return 0; /* could still become a Content-Length header */
        }
        return scan_content_length(buf, len, max_frame, out);
    }
    const char *nl = memchr(buf, '\n', len);
    if (!nl) {
        return (len > max_frame) ? -1 : 0;
    }
    size_t end = (size_t)(nl - buf);
    size_t body_len = end;
    while (body_len > 0 && buf[body_len - 1] == '\r') {
        body_len--;
    }
    out->total = end + 1;
    out->body_off = 0;
    out->body_len = body_len;
    out->content_length = 0;
    return 1;
}

void cbm_shim_msg_inspect(const char *body, size_t len, cbm_shim_msg_t *out) {
    memset(out, 0, sizeof(*out));
    if (!body || len == 0) {
        return;
    }
    yyjson_doc *doc = yyjson_read(body, len, 0);
    if (!doc) {
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return;
    }
    yyjson_val *method = yyjson_obj_get(root, "method");
    yyjson_val *id = yyjson_obj_get(root, "id");
    int has_id = id && !yyjson_is_null(id) && (yyjson_is_str(id) || yyjson_is_num(id));
    if (has_id) {
        size_t wlen = 0;
        char *txt = yyjson_val_write(id, 0, &wlen);
        if (txt && wlen < sizeof(out->id)) {
            memcpy(out->id, txt, wlen);
            out->id[wlen] = '\0';
        } else {
            has_id = 0; /* absurdly long id: not trackable */
        }
        free(txt);
    }
    const char *m = yyjson_is_str(method) ? yyjson_get_str(method) : NULL;
    if (m && has_id) {
        out->kind = CBM_SHIM_MSG_REQUEST;
        out->is_initialize = strcmp(m, "initialize") == 0;
    } else if (m) {
        out->kind = CBM_SHIM_MSG_NOTIFICATION;
        out->is_initialized = strcmp(m, "notifications/initialized") == 0;
    } else if (has_id) {
        out->kind = CBM_SHIM_MSG_RESPONSE;
    }
    if (out->kind != CBM_SHIM_MSG_REQUEST && out->kind != CBM_SHIM_MSG_RESPONSE) {
        out->id[0] = '\0';
    }
    yyjson_doc_free(doc);
}

int cbm_shim_format_lost_error(char *out, size_t cap, const char *id_json, int content_length) {
    if (!out || cap == 0 || !id_json || !id_json[0]) {
        return -1;
    }
    char body[512];
    int bn = snprintf(body, sizeof(body),
                      "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
                      id_json, CBM_SHIM_LOST_ERROR_CODE, CBM_SHIM_LOST_ERROR_MESSAGE);
    if (bn < 0 || (size_t)bn >= sizeof(body)) {
        return -1;
    }
    int n = content_length ? snprintf(out, cap, "Content-Length: %d\r\n\r\n%s", bn, body)
                           : snprintf(out, cap, "%s\n", body);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return n;
}
