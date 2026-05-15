/* ICSim WebSocket bridge — browser-accessible CAN dashboard
 *
 * Uses Mongoose (MIT) to serve an HTML dashboard and stream CAN frames
 * over WebSocket on port 8080.  Works alongside the existing GVRET bridge
 * (port 23) or standalone.
 */
#include "can_platform.h"

#include "mongoose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define snprintf _snprintf
#endif

#define WS_PORT 8080

/* Lightweight JSON helpers — avoids a full JSON library dependency */
static void json_key(char *buf, size_t *pos, size_t sz, const char *key)
{
	int n = snprintf(buf + *pos, sz - *pos, "\"%s\":", key);
	if (n > 0) *pos += n;
}

static void json_int(char *buf, size_t *pos, size_t sz, const char *key, int v)
{
	json_key(buf, pos, sz, key);
	int n = snprintf(buf + *pos, sz - *pos, "%d", v);
	if (n > 0) *pos += n;
}

static void json_hex(char *buf, size_t *pos, size_t sz, const char *key, unsigned int v)
{
	json_key(buf, pos, sz, key);
	int n = snprintf(buf + *pos, sz - *pos, "\"0x%X\"", v);
	if (n > 0) *pos += n;
}

/* -------- inline dashboard HTML -------- */
static const char *html_dashboard =
"<!DOCTYPE html>"
"<html><head><meta charset='UTF-8'><title>ICSim Dashboard</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font:14px monospace;background:#111;color:#0f0;padding:20px}"
"h1{font-size:18px;margin-bottom:16px;color:#0ff}"
"#frames{margin:8px 0;color:#888}"
"table{width:100%;border-collapse:collapse;margin-top:12px}"
"th,td{padding:4px 8px;text-align:left;border-bottom:1px solid #222}"
"th{color:#0ff;font-weight:bold}"
"#controls{margin-top:20px;display:flex;gap:10px;flex-wrap:wrap}"
"input,button{padding:6px 12px;font:14px monospace;background:#222;color:#0f0;border:1px solid #0f0}"
"button:hover{background:#0f0;color:#000}"
"#status{font-size:12px;margin-top:8px;color:#666}"
"</style></head>"
"<body>"
"<h1>ICSim CAN Dashboard</h1>"
"<div id='frames'>Waiting for CAN frames...</div>"
"<div id='status'>Disconnected</div>"
"<table><thead><tr><th>Time</th><th>ID</th><th>DLC</th><th>Data</th></tr></thead>"
"<tbody id='tbody'></tbody></table>"
"<div id='controls'>"
"<input id='canid' placeholder='CAN ID (hex)' size=6>"
"<input id='candata' placeholder='Data (hex)' size=20>"
"<button onclick='sendFrame()'>Send</button>"
"</div>"
"<script>"
"var ws, rowCount=0, maxRows=50;"
"function connect(){"
"ws=new WebSocket('ws://'+location.host+'/ws');"
"ws.onopen=function(){document.getElementById('status').textContent='Connected';};"
"ws.onclose=function(){document.getElementById('status').textContent='Disconnected - reconnecting in 2s...';setTimeout(connect,2000);};"
"ws.onerror=function(){ws.close();};"
"ws.onmessage=function(ev){"
"var f=JSON.parse(ev.data);"
"document.getElementById('frames').textContent='Frames: '+(f.rx||0)+' out / '+(f.tx||0)+' in';"
"if(!f.can_id)return;"
"var tr=document.createElement('tr');"
"tr.innerHTML='<td>'+f.time+'</td><td>'+f.can_id+'</td><td>'+f.len+'</td><td>'+f.data+'</td>';"
"var tbody=document.getElementById('tbody');"
"tbody.insertBefore(tr,tbody.firstChild);"
"if(++rowCount>maxRows){tbody.removeChild(tbody.lastChild);rowCount--;}"
"};}"
"function sendFrame(){"
"if(!ws||ws.readyState!==1)return;"
"ws.send(document.getElementById('canid').value+','+document.getElementById('candata').value);"
"}"
"connect();"
"</script></body></html>";

/* -------- bridge context -------- */
struct ws_bridge {
	can_bus_t   *can;
	struct mg_connection *ws_client;  /* active WebSocket client (or NULL) */
	int          frames_rx;
	int          frames_tx;
};

/* -------- WebSocket CAN frame writer -------- */
static void ws_send_canframe(struct mg_connection *c, struct ws_bridge *br,
			     const struct canfd_frame *frame)
{
	char buf[512];
	size_t pos = 0;

	buf[pos++] = '{';
	json_int(buf, &pos, sizeof(buf), "time", (int)mg_millis());
	json_hex(buf, &pos, sizeof(buf), "can_id", frame->can_id & CAN_SFF_MASK);
	json_int(buf, &pos, sizeof(buf), "len", frame->len);
	buf[pos++] = ',';
	json_key(buf, &pos, sizeof(buf), "data");
	buf[pos++] = '"';
	for (unsigned int i = 0; i < frame->len && i < CAN_MAX_DLEN; i++) {
		int n = snprintf(buf + pos, sizeof(buf) - pos, "%02X", frame->data[i]);
		if (n > 0) pos += n;
	}
	buf[pos++] = '"';
	buf[pos++] = ',';
	json_int(buf, &pos, sizeof(buf), "rx", br->frames_rx);
	buf[pos++] = ',';
	json_int(buf, &pos, sizeof(buf), "tx", br->frames_tx);
	buf[pos++] = '}';
	buf[pos] = '\0';

	mg_ws_send(c, buf, pos, WEBSOCKET_OP_TEXT);
	br->frames_rx++;
}

/* -------- CAN poll timer — fires every ~10ms -------- */
static void can_poll_timer(void *arg)
{
	struct ws_bridge *br = arg;
	struct mg_connection *c = br->ws_client;

	if (!c) return;  /* no client connected */

	/* Drain up to 32 frames per poll to avoid monopolizing */
	for (int i = 0; i < 32; i++) {
		struct canfd_frame frame;
		size_t mtu = 0;

		if (can_bus_recv(br->can, &frame, &mtu) < 0) {
			if (can_bus_error_is_would_block())
				break;
			return;
		}
		if (mtu >= CAN_MTU && !(frame.can_id & CAN_ERR_FLAG))
			ws_send_canframe(c, br, &frame);
	}
}

/* -------- mongoose event handler -------- */
static void ws_handler(struct mg_connection *c, int ev, void *ev_data)
{
	struct ws_bridge *br = c->fn_data;

	switch (ev) {
	case MG_EV_HTTP_MSG: {
		struct mg_http_message *hm = ev_data;
		if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
			mg_ws_upgrade(c, hm, NULL);
			br->ws_client = c;
		} else {
			mg_http_reply(c, 200, "Content-Type: text/html\r\n",
				      "%s", html_dashboard);
		}
		break;
	}

	case MG_EV_WS_MSG: {
		struct mg_ws_message *wm = ev_data;
		char text[128] = {0};
		int len = wm->data.len < (int)sizeof(text) - 1
			? (int)wm->data.len : (int)sizeof(text) - 1;
		memcpy(text, wm->data.buf, len);

		char *comma = strchr(text, ',');
		if (!comma) break;

		*comma = '\0';
		const char *hexid = text;
		const char *hexdata = comma + 1;

		struct canfd_frame frame;
		memset(&frame, 0, sizeof(frame));
		frame.can_id = (canid_t)strtoul(hexid, NULL, 16) & CAN_SFF_MASK;

		const char *p = hexdata;
		uint8_t *d = frame.data;
		while (*p && frame.len < CAN_MAX_DLEN) {
			char byte[3] = {0};
			byte[0] = *p++;
			if (!*p) break;
			byte[1] = *p++;
			*d++ = (uint8_t)strtoul(byte, NULL, 16);
			frame.len++;
		}

		if (frame.len > 0 && can_bus_send(br->can, &frame, CAN_MTU) == 0)
			br->frames_tx++;
		break;
	}

	case MG_EV_CLOSE:
		if (c == br->ws_client)
			br->ws_client = NULL;
		break;
	}
}

/* -------- main -------- */
static void usage(const char *name)
{
	printf("Usage: %s [--port N] [bus]\n", name);
	printf("Example: %s --port 8080 vcan0\n", name);
}

int main(int argc, char **argv)
{
	const char *bus_name = "vcan0";
	int port = WS_PORT;
	struct mg_mgr mgr;
	struct ws_bridge br;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
			port = atoi(argv[++i]);
		} else if (argv[i][0] == '-') {
			usage(argv[0]);
			return 1;
		} else {
			bus_name = argv[i];
		}
	}

	if (can_bus_open(&br.can, bus_name) < 0) {
		fprintf(stderr, "%s\n", can_bus_error());
		return 1;
	}
	br.ws_client = NULL;
	br.frames_rx = 0;
	br.frames_tx = 0;

	mg_mgr_init(&mgr);

	char listen_addr[32];
	snprintf(listen_addr, sizeof(listen_addr), "http://0.0.0.0:%d", port);

	struct mg_connection *c = mg_http_listen(&mgr, listen_addr, ws_handler, &br);
	if (!c) {
		fprintf(stderr, "Failed to listen on 0.0.0.0:%d\n", port);
		can_bus_close(br.can);
		mg_mgr_free(&mgr);
		return 1;
	}
	c->fn_data = &br;

	/* Periodic CAN poll timer — 10ms interval */
	mg_timer_add(&mgr, 10, MG_TIMER_REPEAT, can_poll_timer, &br);

	printf("ICSim WebSocket bridge listening on 0.0.0.0:%d, bus %s\n",
	       port, bus_name);
	printf("Open http://127.0.0.1:%d in a browser.\n", port);

	for (;;)
		mg_mgr_poll(&mgr, 50);

	can_bus_close(br.can);
	mg_mgr_free(&mgr);
	return 0;
}
