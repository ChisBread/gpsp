/*
 * gpsp ESP32-P4 — H.264 + audio live streaming over WebSocket
 *
 * Extracted from web_server.c.  Provides the /ws/stream endpoint
 * handler and all encoder / PPA / buffer management.
 */

#ifndef AV_STREAM_H
#define AV_STREAM_H

#include <esp_err.h>
#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Store the httpd server handle so the stream task can use
 * httpd_ws_send_data() for thread-safe WebSocket sends.
 */
void av_stream_set_server(httpd_handle_t server);

/**
 * WebSocket handler for /ws/stream.
 * Register with httpd as an is_websocket endpoint.
 */
esp_err_t av_stream_ws_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif /* AV_STREAM_H */
