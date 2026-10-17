/* SPDX-License-Identifier: Apache-2.0 */
/* Host boundary tests against the actual bounded HTTP response reader. */
#include "infra/http_response.h"
#include "infra/vela_tls.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct input
{
  const char *data;
  size_t size;
  size_t offset;
  size_t fragment;
  int end;
};
static int read_input(void *context, void *buffer, size_t size)
{
  struct input *input = context;
  size_t count = input->size - input->offset;
  if (!count)
    return input->end;
  if (count > size)
    count = size;
  if (count > input->fragment)
    count = input->fragment;
  memcpy(buffer, input->data + input->offset, count);
  input->offset += count;
  return (int)count;
}
static int accept_event(void *context, const char *body, size_t size)
{
  (void)context;
  return size >= 2 && !memcmp(body + size - 2, "\n\n", 2);
}
static int run(const char *wire, size_t size, size_t fragment, size_t capacity,
               int end, vela_http_event_t event, char *body,
               struct vela_http_response_s *meta)
{
  struct input input = { wire, size, 0, fragment, end };
  return vela_http_response_read(read_input, &input, body, capacity, meta,
                                 event, NULL);
}
int main(void)
{
  char body[128];
  struct vela_http_response_s meta;
  const char *valid[] = {
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Type: "
    "application/json\r\nMcp-Session-Id: safe-123\r\n\r\n{}",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: "
    "chunked\r\n\r\n1\r\n{\r\n1;abc=def\r\n}\r\n0\r\n\r\n"
  };
  for (size_t v = 0; v < sizeof(valid) / sizeof(valid[0]); v++)
    {
      for (size_t f = 1; f <= 32; f++)
        {
          assert(run(valid[v], strlen(valid[v]), f, sizeof(body), 0, NULL,
                     body, &meta) == 200);
          assert(meta.body_length == 2 && !strcmp(body, "{}"));
          if (v == 0)
            assert(!strcmp(meta.session_id, "safe-123"));
          for (size_t n = 0; n < strlen(valid[v]); n++)
            assert(run(valid[v], n, f, sizeof(body), 0, NULL, body, &meta) <
                   0);
          assert(run(valid[v], strlen(valid[v]), f, 2, 0, NULL, body, &meta) ==
                 VELA_TLS_ERR_OVERFLOW);
          assert(run(valid[v], strlen(valid[v]), f, 3, 0, NULL, body, &meta) ==
                 200);
        }
    }
  const char *bad[] = {
    "HTTP/1.1 200x\r\n\r\n",
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}",
    "HTTP/1.1 200 OK\r\nContent-Length: +2\r\n\r\n{}",
    "HTTP/1.1 200 OK\r\nContent-Length: 2junk\r\n\r\n{}",
    "HTTP/1.1 200 OK\r\nContent-Length: 184467440737095516160\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: "
    "2\r\n\r\n{}",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
    "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n",
    "HTTP/1.1 200 OK\r\nMcp-Session-Id: contains space\r\n\r\n",
    "HTTP/1.1 200 OK\r\nMcp-Session-Id: x\r\nMcp-Session-Id: y\r\n\r\n",
    "HTTP/1.1 200 OK\r\n Content-Length: 0\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n-1\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}X\n0\r\n\r\n",
    "HTTP/1.1 204 No Content\r\nContent-Length: 2\r\n\r\n{}"
  };
  for (size_t b = 0; b < sizeof(bad) / sizeof(bad[0]); b++)
    for (size_t f = 1; f <= 32; f++)
      assert(run(bad[b], strlen(bad[b]), f, sizeof(body), 0, NULL, body,
                 &meta) < 0);
  const char *close_body = "HTTP/1.0 200 OK\r\n\r\n{}";
  assert(run(close_body, strlen(close_body), 1, sizeof(body), 0, NULL, body,
             &meta) == 200);
  assert(run(close_body, strlen(close_body), 1, sizeof(body), -4, NULL, body,
             &meta) < 0);
  const char *sse = "HTTP/1.1 200 OK\r\nContent-Type: "
                    "text/event-stream\r\nTransfer-Encoding: "
                    "chunked\r\n\r\na\r\ndata: {}\n\n\r\n";
  for (size_t f = 1; f <= 32; f++)
    {
      assert(run(sse, strlen(sse), f, sizeof(body), -4, accept_event, body,
                 &meta) == 200);
      assert(!strcmp(body, "data: {}\n\n"));
      assert(run(sse, strlen(sse), f, sizeof(body), -4, NULL, body, &meta) <
             0);
    }
  /* Arbitrary bytes and fragment boundaries must not escape caller buffers. */
  srand(12345);
  for (size_t iteration = 0; iteration < 20000; iteration++)
    {
      char fuzz[512];
      size_t size = (size_t)rand() % sizeof(fuzz);
      for (size_t j = 0; j < size; j++)
        fuzz[j] = (char)rand();
      run(fuzz, size, 1 + (size_t)rand() % 32,
          1 + (size_t)rand() % sizeof(body), 0, NULL, body, &meta);
    }
  puts("PASS: HTTP fragmentation, truncation, framing conflicts, capacity, "
       "SSE, 20000 fuzz cases");
  return 0;
}
