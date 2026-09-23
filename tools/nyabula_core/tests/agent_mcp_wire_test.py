#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Host sanitizer test of actual Core MCP protocol code, with a fake transport."""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('core', type=pathlib.Path)
parser.add_argument('agent', type=pathlib.Path)
parser.add_argument('cjson', type=pathlib.Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='nyabot-mcp-wire-') as directory:
    root = pathlib.Path(directory)
    (root / 'nuttx').mkdir()
    (root / 'nuttx/config.h').write_text('#define CONFIG_AI_AGENT_SIM_HTTP_FIXTURE 1\n')
    (root / 'netutils').mkdir()
    (root / 'netutils/cJSON.h').symlink_to((args.cjson / 'cJSON.h').resolve())
    test = root / 'test.c'
    test.write_text(r'''
#include "ny_agent_mcp_wire.h"
#include "infra/vela_tls.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int calls, mode, initialized;
int ny_product_json_check(const char *text, size_t length) {
    return !text || !length || memchr(text, 0, length) ? -1 : 0;
}
static int transport(const vela_header_t *headers, const char *body,
    char *output, size_t capacity, struct vela_http_response_s *meta,
    vela_http_event_t event, void *context) {
    calls++;
    memset(meta, 0, sizeof(*meta));
    output[0] = 0;
    cJSON *request = cJSON_Parse(body);
    assert(request);
    const char *method = cJSON_GetObjectItemCaseSensitive(request, "method")->valuestring;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
    int session = 0, version = 0, accept = 0;
    for (const vela_header_t *h = headers; h->name; h++) {
        if (!strcmp(h->name, "Mcp-Session-Id")) { assert(!strcmp(h->value, "session-test")); session++; }
        if (!strcmp(h->name, "MCP-Protocol-Version")) { assert(!strcmp(h->value, NY_MCP_PROTOCOL)); version++; }
        if (!strcmp(h->name, "Accept")) { assert(strstr(h->value, "text/event-stream")); accept++; }
    }
    assert(version == 1 && accept == 1);
    if (!strcmp(method, "notifications/initialized")) {
        assert(!id && session == 1);
        initialized = 1;
        cJSON_Delete(request);
        return 202;
    }
    if (mode == 6) { cJSON_Delete(request); return VELA_TLS_ERR_READ; }
    const char *result;
    if (!strcmp(method, "initialize")) {
        assert(!session);
        strcpy(meta->session_id, "session-test");
        result = mode == 2 ? "{\"protocolVersion\":\"old\",\"capabilities\":{}}" :
            "{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"tools\":{},\"resources\":{},\"prompts\":{}}}";
    } else {
        assert(initialized && session == 1);
        if (!strcmp(method, "tools/list")) {
            result = mode == 3 ? "{\"tools\":[{\"name\":\"write\"}]}" :
                mode == 4 ? "{\"tools\":[],\"nextCursor\":\"same\"}" :
                "{\"tools\":[{\"name\":\"write\",\"inputSchema\":{\"type\":\"object\"}}]}";
        } else if (!strcmp(method, "resources/list")) result = "{\"resources\":[{\"uri\":\"test://one\",\"name\":\"one\"}]}";
        else if (!strcmp(method, "prompts/list")) result = "{\"prompts\":[{\"name\":\"summarize\"}]}";
        else result = "{\"content\":[{\"type\":\"text\",\"text\":\"executed\"}]}";
    }
    char json[2048];
    snprintf(json, sizeof(json), "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"result\":%s}",
        mode == 1 ? "wrong-id" : id->valuestring, result);
    if (mode == 7) snprintf(json, sizeof(json), "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"id\":\"%s\",\"result\":{}}", id->valuestring, id->valuestring);
    cJSON_Delete(request);
    if (mode == 5) {
        strcpy(meta->content_type, "text/event-stream");
        snprintf(output, capacity, ": comment\r\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\r\n\r\ndata: %s\n\n", json);
        meta->body_length = strlen(output);
        for (size_t n = 1; n <= meta->body_length; n++) {
            if (output[n - 1] == '\n') {
                int ret = event(context, output, n);
                if (ret) { assert(ret == 1 && n == meta->body_length); break; }
            }
        }
    } else {
        strcpy(meta->content_type, "application/json; charset=utf-8");
        snprintf(output, capacity, "%s", json);
        meta->body_length = strlen(output);
    }
    return 200;
}
int vela_https_request_once(const char *host, const char *port, const char *path,
    const vela_header_t *headers, const char *body, char *output, size_t capacity,
    struct vela_http_response_s *meta, vela_http_event_t event, void *context) {
    (void)host; (void)port; (void)path;
    return transport(headers, body, output, capacity, meta, event, context);
}
int vela_http_loopback_post_once(const char *port, const char *path,
    const vela_header_t *headers, const char *body, char *output, size_t capacity,
    struct vela_http_response_s *meta, vela_http_event_t event, void *context) {
    return vela_https_request_once("127.0.0.1", port, path, headers, body, output,
        capacity, meta, event, context);
}
int main(void) {
    struct ny_mcp_peer_s peer;
    const char *bad[] = {"http://example.org/mcp", "https://u:p@host/mcp",
        "https://host:abc/mcp", "https://host:65536/mcp", "https://host/mcp#frag",
        "https://host/\r\nX:evil", "https:///mcp", "https://host?key=secret"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++)
        assert(ny_mcp_peer_configure(&peer, bad[i], "") < 0);
    assert(ny_mcp_peer_configure(&peer, "https://host/mcp", "bad\r\nkey") < 0);
    assert(ny_mcp_peer_configure(&peer, "http://127.0.0.1:1234/mcp", "") == 0 && peer.fixture);
    for (mode = 0; mode <= 7; mode++) {
        initialized = 0;
        assert(ny_mcp_peer_configure(&peer, "https://host:443/mcp", "key") == 0);
        int before = calls;
        int ret = ny_mcp_peer_initialize(&peer);
        if (mode == 1 || mode == 2 || mode == 6 || mode == 7) {
            assert(ret < 0 && calls == before + 1 && !initialized);
            continue;
        }
        assert(ret == 0 && calls == before + 2 && initialized);
        cJSON *catalog = NULL;
        ret = ny_mcp_peer_catalog(&peer, &catalog);
        if (mode == 3 || mode == 4) { assert(ret < 0 && !catalog); continue; }
        assert(ret == 0 && catalog);
        assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(catalog, "tools")) == 1);
        assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(catalog, "resources")) == 1);
        assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(catalog, "prompts")) == 1);
        cJSON_Delete(catalog);
        cJSON *result = NULL;
        assert(ny_mcp_peer_request(&peer, "tools/call", NULL, &result) == 0);
        assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(result, "content")));
        cJSON_Delete(result);
    }
    puts("PASS: MCP URL guards, initialize/notification/session, JSON/SSE, exact IDs, catalogs, bad versions/schemas/cursors, no exchange retry");
    return 0;
}
''')
    binary = root / 'test'
    subprocess.run(['cc', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(root),
                    '-I', str(args.core), '-I', str(args.agent / 'src'),
                    str(test), str(args.core / 'ny_agent_mcp_wire.c'),
                    str(args.cjson / 'cJSON.c'), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
