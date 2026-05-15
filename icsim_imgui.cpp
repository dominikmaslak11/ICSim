/* ICSIm — Dear ImGui version (unified launcher)
 *
 * Single binary with two modes:
 *   icsim_imgui vcan0           → dashboard (CAN receiver)
 *   icsim_imgui --controls vcan0 → control panel (CAN sender)
 *
 * Dashboard: circular speedometer + RPM gauge, fuel/temp bars,
 *   turn signal indicators, door status, debug overlay.
 *
 * Controls: throttle slider, RPM/temp/fuel sliders,
 *   turn signal + door toggles, "Send All" button.
 */
#include "can_platform.h"
#include "can_log.h"
#include "config.h"
#include "lib.h"

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_sdl2.h"
#include "imgui/backends/imgui_impl_sdlrenderer2.h"

#include <SDL2/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

extern "C" {
#include "getopt_compat.h"
}

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef DATA_DIR
#define DATA_DIR "./data/"
#endif
#define DEFAULT_CAN_TRAFFIC DATA_DIR "sample-can.log"

/* ---------- shared state ---------- */
static icsim_config_t g_cfg;

/* dashboard state */
static int          debug = 0;
static int          randomize = 0;
static int          seed = 0;
static long         current_speed = 0;
static int          engine_rpm = 800;
static int          coolant_temp = 80;
static int          fuel_level = 75;
static int          door_status[4] = {0, 0, 0, 0};
static int          turn_status[2] = {0, 0};
static char        *model = NULL;
static char        *record_path = NULL;
static char        *replay_path = NULL;
static int          headless = 0, headless_duration = 0;
static can_log_t   *can_recorder = NULL;
static can_log_t   *can_replayer = NULL;
static int          frames_total = 0;
static Uint32       last_can_activity = 0;
static Uint32       replay_base_tick = 0;

/* controls state */
static float        throttle_val = 0.0f;
static int          door_state = 0x0F;
static int          turn_left = 0, turn_right = 0;
static int          ctrl_door_id, ctrl_signal_id, ctrl_speed_id;
static int          ctrl_rpm_id,  ctrl_temp_id,   ctrl_fuel_id;
static int          ctrl_door_pos, ctrl_signal_pos, ctrl_speed_pos;
static can_bus_t   *ctrl_can = NULL;
static float        ctrl_speed_mph = 0.0f;
static char        *traffic_log = (char *)DEFAULT_CAN_TRAFFIC;
static int          play_traffic = 1;
static volatile int traffic_running = 1;

/* CAN IDs (updated on model switch) */
static canid_t g_door_id, g_signal_id, g_speed_id;
static canid_t g_rpm_id,  g_temp_id,   g_fuel_id;

/* mode */
static bool controls_mode = false;
static bool show_debug = false;
static bool show_help = false;
static bool debug_can = false;
static bool fullscreen = false;
static float replay_speed = 1.0f;

/* model switching */
static std::vector<std::string> model_list;
static std::vector<std::string> model_names;
static int           current_model_idx = 0;
static char          current_model_path[256] = {0};

#define DOOR_LOCKED   0
#define DOOR_UNLOCKED 1
#define OFF 0
#define ON  1

#define MAX_SPEED   90.0f
#define ACCEL_RATE  8.0f

/* ---------- CAN frame handlers ---------- */

static void update_speed_status(struct canfd_frame *cf, int maxdlen) {
	int len = (cf->len > maxdlen) ? maxdlen : cf->len;
	int sp = g_cfg.can.speed_pos;
	if (len <= sp + 1) return;
	if (model && !strncmp(model, "bmw", 3)) {
		current_speed = (((cf->data[sp + 1] - 208) * 256) + cf->data[sp]) / 16;
	} else {
		int raw = (cf->data[sp] << 8) | cf->data[sp + 1];
		current_speed = (long)((double)raw * g_cfg.speed.scaling / g_cfg.speed.divisor);
	}
}

static void update_rpm_status(struct canfd_frame *cf, int maxdlen) {
	int len = (cf->len > maxdlen) ? maxdlen : cf->len;
	int pos = g_cfg.rpm.rpm_pos;
	if (len <= pos + 1) return;
	int raw = (cf->data[pos] << 8) | cf->data[pos + 1];
	engine_rpm = (int)((double)raw * g_cfg.rpm.scaling / g_cfg.rpm.divisor);
}

static void update_temp_status(struct canfd_frame *cf, int maxdlen) {
	int len = (cf->len > maxdlen) ? maxdlen : cf->len;
	int pos = g_cfg.temp.temp_pos;
	if (len <= pos) return;
	coolant_temp = (int)((double)cf->data[pos] * g_cfg.temp.scaling / g_cfg.temp.divisor);
}

static void update_fuel_status(struct canfd_frame *cf, int maxdlen) {
	int len = (cf->len > maxdlen) ? maxdlen : cf->len;
	int pos = g_cfg.fuel.fuel_pos;
	if (len <= pos) return;
	fuel_level = (int)((double)cf->data[pos] * g_cfg.fuel.scaling / g_cfg.fuel.divisor);
}

static void update_signal_status(struct canfd_frame *cf, int maxdlen) {
	int len = (cf->len > maxdlen) ? maxdlen : cf->len;
	int pos = g_cfg.can.signal_pos;
	if (len <= pos) return;
	turn_status[0] = (cf->data[pos] & ICSIM_TURN_LEFT)  ? ON : OFF;
	turn_status[1] = (cf->data[pos] & ICSIM_TURN_RIGHT) ? ON : OFF;
}

static void update_door_status(struct canfd_frame *cf, int maxdlen) {
	int len = (cf->len > maxdlen) ? maxdlen : cf->len;
	int pos = g_cfg.can.door_pos;
	if (len <= pos) return;
	unsigned char d = cf->data[pos];
	door_status[0] = (d & ICSIM_DOOR1) ? DOOR_LOCKED : DOOR_UNLOCKED;
	door_status[1] = (d & ICSIM_DOOR2) ? DOOR_LOCKED : DOOR_UNLOCKED;
	door_status[2] = (d & ICSIM_DOOR3) ? DOOR_LOCKED : DOOR_UNLOCKED;
	door_status[3] = (d & ICSIM_DOOR4) ? DOOR_LOCKED : DOOR_UNLOCKED;
}

/* ---------- controls CAN send helpers ---------- */
static void send_pkt(can_bus_t *bus, struct canfd_frame *cf, int mtu) {
	if (debug_can) fprintf(stderr, "[CAN TX] id=0x%03X len=%d\n", cf->can_id, cf->len);
	if (can_bus_send(bus, cf, (size_t)mtu) < 0)
		fprintf(stderr, "%s\n", can_bus_error());
}

static void controls_send_speed() {
	float kph = current_speed / 0.6213751f * 100.0f;
	struct canfd_frame cf;
	memset(&cf, 0, sizeof(cf));
	cf.can_id = ctrl_speed_id;
	cf.len = (uint8_t)(ctrl_speed_pos + 2);
	cf.data[ctrl_speed_pos + 1] = (uint8_t)((int)kph & 0xFF);
	cf.data[ctrl_speed_pos]     = (uint8_t)(((int)kph >> 8) & 0xFF);
	send_pkt(ctrl_can, &cf, CAN_MTU);
}

static void controls_send_doors() {
	struct canfd_frame cf;
	memset(&cf, 0, sizeof(cf));
	cf.can_id = ctrl_door_id;
	cf.len = (uint8_t)(ctrl_door_pos + 1);
	cf.data[ctrl_door_pos] = (uint8_t)door_state;
	send_pkt(ctrl_can, &cf, CAN_MTU);
}

static void controls_send_signals() {
	uint8_t sig = 0;
	if (turn_left)  sig |= ICSIM_TURN_LEFT;
	if (turn_right) sig |= ICSIM_TURN_RIGHT;
	struct canfd_frame cf;
	memset(&cf, 0, sizeof(cf));
	cf.can_id = ctrl_signal_id;
	cf.len = (uint8_t)(ctrl_signal_pos + 1);
	cf.data[ctrl_signal_pos] = sig;
	send_pkt(ctrl_can, &cf, CAN_MTU);
}

static void controls_send_rpm() {
	int raw = engine_rpm * g_cfg.rpm.divisor;
	struct canfd_frame cf;
	memset(&cf, 0, sizeof(cf));
	cf.can_id = ctrl_rpm_id;
	cf.len = (uint8_t)(g_cfg.rpm.rpm_pos + 2);
	cf.data[g_cfg.rpm.rpm_pos + 1] = raw & 0xFF;
	cf.data[g_cfg.rpm.rpm_pos]     = (raw >> 8) & 0xFF;
	send_pkt(ctrl_can, &cf, CAN_MTU);
}

static void controls_send_temp() {
	struct canfd_frame cf;
	memset(&cf, 0, sizeof(cf));
	cf.can_id = ctrl_temp_id;
	cf.len = (uint8_t)(g_cfg.temp.temp_pos + 1);
	cf.data[g_cfg.temp.temp_pos] = (uint8_t)coolant_temp;
	send_pkt(ctrl_can, &cf, CAN_MTU);
}

static void controls_send_fuel() {
	struct canfd_frame cf;
	memset(&cf, 0, sizeof(cf));
	cf.can_id = ctrl_fuel_id;
	cf.len = (uint8_t)(g_cfg.fuel.fuel_pos + 1);
	cf.data[g_cfg.fuel.fuel_pos] = (uint8_t)fuel_level;
	send_pkt(ctrl_can, &cf, CAN_MTU);
}

static void controls_send_all() {
	if (!ctrl_can) return;
	controls_send_speed();
	controls_send_rpm();
	controls_send_temp();
	controls_send_fuel();
	controls_send_doors();
	controls_send_signals();
}

static int play_can_traffic_thread(void *unused) {
	(void)unused;

	while (traffic_running) {
		FILE *traffic = fopen(traffic_log, "r");
		char line[256];

		if (!traffic) {
			fprintf(stderr, "WARNING: Could not open CAN traffic file: %s\n", traffic_log);
			return 1;
		}

		while (traffic_running && fgets(line, sizeof(line), traffic)) {
			char *hash = strchr(line, '#');
			char *frame_text;
			struct canfd_frame traffic_frame;
			int traffic_mtu;

			if (!hash) continue;
			frame_text = hash;
			while (frame_text > line && frame_text[-1] != ' ' && frame_text[-1] != '\t')
				frame_text--;

			traffic_mtu = parse_canframe(frame_text, &traffic_frame);
			if (traffic_mtu)
				can_bus_send(ctrl_can, &traffic_frame, (size_t)traffic_mtu);
			SDL_Delay(2);
		}
		fclose(traffic);
	}

	return 0;
}

/* ---------- model scanning & switching ---------- */

static void scan_models() {
	model_list.clear();
	model_names.clear();
#ifdef _WIN32
	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFile("models\\*.toml", &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			std::string name = fd.cFileName;
			char path[320];
			snprintf(path, sizeof(path), "models/%s", fd.cFileName);
			model_list.push_back(path);
			size_t dot = name.rfind('.');
			if (dot != std::string::npos)
				name = name.substr(0, dot);
			model_names.push_back(name);
		} while (FindNextFile(h, &fd));
		FindClose(h);
	}
#else
	DIR *d = opendir("models");
	if (d) {
		struct dirent *de;
		while ((de = readdir(d))) {
			const char *n = de->d_name;
			size_t len = strlen(n);
			if (len > 5 && !strcmp(n + len - 5, ".toml")) {
				char path[320];
				snprintf(path, sizeof(path), "models/%s", n);
				model_list.push_back(path);
				std::string name(n, len - 5);
				model_names.push_back(name);
			}
		}
		closedir(d);
	}
#endif
	if (model_list.empty()) {
		model_list.push_back("default");
		model_names.push_back("default");
	}
}

static void switch_model(int idx) {
	if (idx < 0 || idx >= (int)model_list.size()) return;
	current_model_idx = idx;
	const char *path = model_list[idx].c_str();

	icsim_config_load(&g_cfg, path);
	printf("Switched to: %s (CAN IDs updated)\n", g_cfg.name);

	/* update global CAN IDs */
	g_door_id   = g_cfg.can.door_id;
	g_signal_id = g_cfg.can.signal_id;
	g_speed_id  = g_cfg.can.speed_id;
	g_rpm_id    = g_cfg.rpm.rpm_id;
	g_temp_id   = g_cfg.temp.temp_id;
	g_fuel_id   = g_cfg.fuel.fuel_id;

	/* update controls IDs */
	ctrl_door_id   = g_door_id;
	ctrl_signal_id = g_signal_id;
	ctrl_speed_id  = g_speed_id;
	ctrl_rpm_id    = g_rpm_id;
	ctrl_temp_id   = g_temp_id;
	ctrl_fuel_id   = g_fuel_id;
	ctrl_door_pos   = g_cfg.can.door_pos;
	ctrl_signal_pos = g_cfg.can.signal_pos;
	ctrl_speed_pos  = g_cfg.can.speed_pos;

	snprintf(current_model_path, sizeof(current_model_path), "%s", path);

	/* persist choice */
	FILE *f = fopen("icsim_model.txt", "w");
	if (f) { fprintf(f, "%s", path); fclose(f); }
}

/* ---------- ImGui rendering ---------- */

/* ---- Color scheme ---- */
#define COL_BG       ImColor(10, 10, 18, 255)
#define COL_ACCENT   ImColor(0, 180, 240, 255)
#define COL_NEEDLE   ImColor(255, 70, 70, 255)
#define COL_REDZONE  ImColor(255, 40, 40, 255)
#define COL_WHITE    ImColor(230, 230, 240, 255)
#define COL_DIM      ImColor(50, 50, 60, 255)
#define COL_FUEL     ImColor(80, 200, 80, 255)
#define COL_FUEL_LOW ImColor(255, 60, 60, 255)
#define COL_TEMP     ImColor(60, 180, 240, 255)
#define COL_TEMP_HI  ImColor(255, 60, 40, 255)
#define COL_DOOR_CL  ImColor(40, 200, 40, 255)
#define COL_DOOR_OP  ImColor(255, 50, 50, 255)
#define COL_SIGNAL   ImColor(0, 220, 100, 255)

/* ---- Helper: circular gauge ---- */
static void draw_circular_gauge(
	float cx, float cy, float radius,
	float value, float vmin, float vmax,
	float start_angle, float sweep_angle,
	const char *label_below,
	int redline_start,
	void (*draw_custom)(float, float, float, float, float))
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float PI = 3.14159265f;
	float arc_min  = start_angle * PI / 180.0f;
	float arc_max  = (start_angle + sweep_angle) * PI / 180.0f;
	float inner_r  = radius - 16.0f;
	float tick_r   = radius - 5.0f;
	float tick_mini= radius - 9.0f;

	dl->PathArcTo(ImVec2(cx, cy), radius,  arc_min, arc_max, 48);
	dl->PathArcTo(ImVec2(cx, cy), inner_r, arc_max, arc_min, 48);
	dl->PathFillConvex(COL_BG);

	dl->PathArcTo(ImVec2(cx, cy), radius + 3, arc_min, arc_max, 48);
	dl->PathStroke(ImColor(30, 30, 40, 255), false, 2.0f);

	float frac = (value - vmin) / (vmax - vmin);
	if (frac < 0.0f) frac = 0.0f;
	if (frac > 1.0f) frac = 1.0f;
	float val_arc = arc_min + frac * sweep_angle * PI / 180.0f;

	ImU32 col_active = (value >= redline_start) ? COL_REDZONE : COL_ACCENT;
	dl->PathArcTo(ImVec2(cx, cy), radius - 8, arc_min, val_arc, 32);
	dl->PathStroke(col_active, false, 3.0f);

	if (redline_start < vmax) {
		float red_a = arc_min + ((redline_start - vmin) / (vmax - vmin)) * sweep_angle * PI / 180.0f;
		dl->AddLine(ImVec2(cx + cosf(red_a) * (radius - 4), cy + sinf(red_a) * (radius - 4)),
			ImVec2(cx + cosf(red_a) * (inner_r + 4), cy + sinf(red_a) * (inner_r + 4)),
			COL_REDZONE, 1.5f);
	}

	int num_ticks = 25;
	for (int i = 0; i <= num_ticks; i++) {
		float frac_i = i / (float)num_ticks;
		float a = arc_min + frac_i * sweep_angle * PI / 180.0f;
		float r1 = (i % 5 == 0) ? tick_r : tick_mini;
		float r0 = inner_r + 1.0f;
		dl->AddLine(ImVec2(cx + cosf(a) * r0, cy + sinf(a) * r0),
			ImVec2(cx + cosf(a) * r1, cy + sinf(a) * r1),
			(i % 5 == 0) ? COL_WHITE : COL_DIM, (i % 5 == 0) ? 1.5f : 0.8f);
	}

	if (draw_custom)
		draw_custom(cx, cy, radius, arc_min, sweep_angle);

	float n_angle = arc_min + frac * sweep_angle * PI / 180.0f;
	float nl = radius * 0.62f;
	dl->AddLine(ImVec2(cx, cy),
		ImVec2(cx + cosf(n_angle) * nl, cy + sinf(n_angle) * nl),
		COL_NEEDLE, 2.5f);

	float cw_a = n_angle + PI;
	dl->AddLine(ImVec2(cx, cy),
		ImVec2(cx + cosf(cw_a) * radius * 0.18f, cy + sinf(cw_a) * radius * 0.18f),
		COL_NEEDLE, 2.0f);

	dl->AddCircleFilled(ImVec2(cx, cy), 7.0f, ImColor(40, 40, 50, 255));
	dl->AddCircleFilled(ImVec2(cx, cy), 4.0f, COL_DIM);

	if (label_below) {
		ImVec2 ts = ImGui::CalcTextSize(label_below);
		dl->AddText(ImVec2(cx - ts.x * 0.5f, cy + radius * 0.45f), COL_WHITE, label_below);
	}
}

/* Speedometer number labels */
static void speedo_labels(float cx, float cy, float radius, float arc_min, float sweep) {
	const float PI = 3.14159265f;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	float num_r = radius * 0.78f;
	for (int v = 0; v <= 260; v += 20) {
		if (v % 40 != 0) continue;
		float a = arc_min + (v / 260.0f) * sweep * PI / 180.0f;
		char buf[8];
		snprintf(buf, sizeof(buf), "%d", v);
		ImVec2 sz = ImGui::CalcTextSize(buf);
		dl->AddText(ImVec2(cx + cosf(a) * num_r - sz.x * 0.5f, cy + sinf(a) * num_r - sz.y * 0.5f),
			COL_WHITE, buf);
	}
}

/* RPM number labels */
static void rpm_labels(float cx, float cy, float radius, float arc_min, float sweep) {
	const float PI = 3.14159265f;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	float num_r = radius * 0.78f;
	for (int v = 0; v <= 7; v++) {
		float a = arc_min + (v / 7.0f) * sweep * PI / 180.0f;
		char buf[8];
		snprintf(buf, sizeof(buf), "%d", v);
		ImVec2 sz = ImGui::CalcTextSize(buf);
		dl->AddText(ImVec2(cx + cosf(a) * num_r - sz.x * 0.5f, cy + sinf(a) * num_r - sz.y * 0.5f),
			(v >= 6) ? COL_REDZONE : COL_WHITE, buf);
	}
}

/* Vertical bar gauge (fuel / temp) */
static void draw_bar_gauge(float x, float y, float w, float h,
	float value, float vmin, float vmax,
	const char *label, const char *unit,
	ImU32 col_normal, ImU32 col_warn, float warn_threshold, bool warn_high)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	float frac = (value - vmin) / (vmax - vmin);
	if (frac < 0.0f) frac = 0.0f;
	if (frac > 1.0f) frac = 1.0f;
	bool warn = warn_high ? (value >= warn_threshold) : (value <= warn_threshold);

	dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), COL_DIM, 3.0f);

	float fill_h = h * frac;
	dl->AddRectFilled(ImVec2(x, y + h - fill_h), ImVec2(x + w, y + h),
		warn ? col_warn : col_normal, 3.0f);

	char val_buf[16];
	snprintf(val_buf, sizeof(val_buf), "%s", unit);
	ImVec2 us = ImGui::CalcTextSize(val_buf);
	dl->AddText(ImVec2(x + (w - us.x) * 0.5f, y + h + 2), warn ? col_warn : col_normal, val_buf);

	ImVec2 ls = ImGui::CalcTextSize(label);
	dl->AddText(ImVec2(x + (w - ls.x) * 0.5f, y - ls.y - 2), COL_WHITE, label);
}

/* Turn signal arrow */
static void draw_signal_arrow(float cx, float cy, bool on, bool left) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImU32 col = on ? ImColor(0, 255, 80, 255) : ImColor(20, 25, 20, 255);
	float dir = left ? -1.0f : 1.0f;
	ImVec2 pts[3] = {
		ImVec2(cx - dir * 8, cy - 6),
		ImVec2(cx - dir * 8, cy + 6),
		ImVec2(cx + dir * 8, cy)
	};
	dl->AddTriangleFilled(pts[0], pts[1], pts[2], col);
	if (on)
		dl->AddTriangle(pts[0], pts[1], pts[2], ImColor(0, 255, 80, 60), 2.0f);
}

/* Door indicator dot */
static void draw_door_dot(float x, float y, int door_idx) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	bool open = (door_status[door_idx] == DOOR_UNLOCKED);
	ImU32 col = open ? COL_DOOR_OP : COL_DOOR_CL;
	dl->AddCircleFilled(ImVec2(x, y), 6.0f, col);
	if (open)
		dl->AddCircle(ImVec2(x, y), 8.0f, ImColor(255, 50, 50, 100), 12, 1.5f);
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", door_idx + 1);
	ImVec2 ns = ImGui::CalcTextSize(buf);
	dl->AddText(ImVec2(x - ns.x * 0.5f, y + 10), COL_WHITE, buf);
}

/* ===== Dashboard render ===== */
static void render_dashboard() {
	ImGuiIO &io = ImGui::GetIO();
	float W = io.DisplaySize.x;
	float H = io.DisplaySize.y;

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(W, H));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 2));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 2));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::Begin("ICSim Dashboard", NULL,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoBringToFrontOnFocus);

	ImDrawList *dl = ImGui::GetWindowDrawList();

	/* top bar with model switcher */
	ImGui::SetNextItemWidth(W * 0.14f);
	if (ImGui::Combo("##model", &current_model_idx,
		[](void*, int idx) -> const char* {
			return model_names[idx].c_str();
		}, nullptr, (int)model_names.size())) {
		switch_model(current_model_idx);
	}
	ImGui::SameLine(W * 0.21f);
	int can_active = (SDL_GetTicks() - last_can_activity) < 800;
	ImGui::TextColored(can_active ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0.25f, 0.25f, 1),
		can_active ? "\xe2\x97\x8f" : "\xe2\x97\x8b");
	ImGui::SameLine();
	ImGui::TextColored(can_active ? ImVec4(0.3f, 1, 0.3f, 0.9f) : ImVec4(0.5f, 0.3f, 0.3f, 0.9f),
		can_active ? "CAN" : "OFF");
	ImGui::SameLine(W * 0.55f);
	ImGui::Text("f:%d", frames_total);
	if (can_replayer) {
		ImGui::SameLine();
		float rprog = (float)(SDL_GetTicks() - replay_base_tick) / 60000.0f;
		if (rprog > 1.0f) rprog = 1.0f;
		ImGui::ProgressBar(rprog, ImVec2(W * 0.08f, 10), "");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(40);
		const char *speeds[] = {"0.5x","1x","2x","5x","max"};
		static int speed_idx = 1;
		if (ImGui::Combo("##spd", &speed_idx, speeds, 5, 3)) {
			float vals[] = {0.5f, 1.0f, 2.0f, 5.0f, 60.0f};
			replay_speed = vals[speed_idx];
		}
	} else if (!can_replayer) {
		replay_speed = 1.0f;
	}
	if (can_recorder) {
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "\xe2\x97\x8f REC");
	}
	ImGui::SameLine(W * 0.88f);
	ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1.0f),
		"?:help  FPS:%.0f", io.Framerate);
	ImGui::Separator();

	/* gauges */
	float gauge_y   = H * 0.08f;
	float gauge_h   = H * 0.60f;
	float center_y  = gauge_y + gauge_h * 0.55f;
	float speedo_cx = W * 0.28f;
	float rpm_cx    = W * 0.72f;
	float radius    = (gauge_h * 0.50f < W * 0.24f) ? gauge_h * 0.50f : W * 0.24f;

	char speed_buf[16];
	snprintf(speed_buf, sizeof(speed_buf), "%ld", current_speed);
	draw_circular_gauge(speedo_cx, center_y, radius,
		(float)current_speed, 0.0f, 260.0f,
		210.0f, 240.0f, speed_buf, 220, speedo_labels);
	{
		ImVec2 ts = ImGui::CalcTextSize("km/h");
		dl->AddText(ImVec2(speedo_cx - ts.x * 0.5f, center_y + radius * 0.32f),
			ImColor(120, 120, 140, 255), "km/h");
	}

	char rpm_buf[16];
	snprintf(rpm_buf, sizeof(rpm_buf), "%d", engine_rpm);
	draw_circular_gauge(rpm_cx, center_y, radius,
		(float)engine_rpm, 0.0f, 7000.0f,
		210.0f, 240.0f, rpm_buf, 6000, rpm_labels);
	{
		ImVec2 ts = ImGui::CalcTextSize("rpm");
		dl->AddText(ImVec2(rpm_cx - ts.x * 0.5f, center_y + radius * 0.32f),
			ImColor(120, 120, 140, 255), "rpm");
	}

	/* turn signals */
	float sig_y = center_y - radius * 0.55f;
	draw_signal_arrow(W * 0.41f, sig_y, turn_status[0] == ON, true);
	draw_signal_arrow(W * 0.59f, sig_y, turn_status[1] == ON, false);

	/* gear indicator (P/D) between signals */
	{
		const char *gear = (current_speed > 0) ? "D" : "P";
		ImVec2 gs = ImGui::CalcTextSize(gear);
		ImU32 gc = (current_speed > 0) ? ImColor(80, 220, 120, 255) : ImColor(200, 160, 40, 255);
		dl->AddRectFilled(ImVec2(W * 0.5f - 14, sig_y - 6), ImVec2(W * 0.5f + 14, sig_y + 14),
			ImColor(20, 22, 28, 255), 4.0f);
		dl->AddRect(ImVec2(W * 0.5f - 14, sig_y - 6), ImVec2(W * 0.5f + 14, sig_y + 14),
			gc, 4.0f, 0, 1.5f);
		dl->AddText(ImVec2(W * 0.5f - gs.x * 0.5f, sig_y - 2), gc, gear);
	}

	/* bottom: fuel, temp, doors */
	float bot_y = gauge_y + gauge_h + H * 0.01f;
	float bar_h = H * 0.16f;
	float bar_w = W * 0.022f;

	draw_bar_gauge(W * 0.12f, bot_y, bar_w, bar_h,
		(float)fuel_level, 0.0f, 100.0f,
		"F", "%", COL_FUEL, COL_FUEL_LOW, 15.0f, false);
	char fuel_txt[16];
	snprintf(fuel_txt, sizeof(fuel_txt), "%d%%", fuel_level);
	dl->AddText(ImVec2(W * 0.12f + bar_w + 4, bot_y + bar_h * 0.35f), COL_WHITE, fuel_txt);

	draw_bar_gauge(W * 0.22f, bot_y, bar_w, bar_h,
		(float)coolant_temp, 60.0f, 120.0f,
		"C", "\xc2\xb0" "C", COL_TEMP, COL_TEMP_HI, 105.0f, true);
	char temp_txt[16];
	snprintf(temp_txt, sizeof(temp_txt), "%d\xc2\xb0", coolant_temp);
	dl->AddText(ImVec2(W * 0.22f + bar_w + 4, bot_y + bar_h * 0.35f), COL_WHITE, temp_txt);

	float door_x = W * 0.42f;
	float door_y = bot_y + bar_h * 0.15f;
	dl->AddText(ImVec2(door_x, door_y - 13), COL_DIM, "DOORS");
	for (int d = 0; d < 4; d++)
		draw_door_dot(door_x + d * 26.0f, door_y + 10.0f, d);

	ImGui::PopStyleVar(3);
	ImGui::End();
}

/* ===== Controls panel render ===== */
static void render_controls() {
	ImGuiIO &io = ImGui::GetIO();
	float W = io.DisplaySize.x;

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(W, io.DisplaySize.y));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 3));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
	ImGui::Begin("ICSim Controls", NULL,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove);

	/* title + model switcher */
	ImGui::SetNextItemWidth(W * 0.16f);
	if (ImGui::Combo("##model", &current_model_idx,
		[](void*, int idx) -> const char* {
			return model_names[idx].c_str();
		}, nullptr, (int)model_names.size())) {
		switch_model(current_model_idx);
	}
	ImGui::SameLine();
	ImGui::TextColored(ImVec4(0.0f, 0.75f, 1.0f, 1.0f), "  Controls");
	ImGui::SameLine(W - 80);
	ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1.0f), "FPS: %.0f", io.Framerate);

	/* throttle */
	ImGui::PushItemWidth(W - 16);
	ImGui::SliderFloat("##throttle", &throttle_val,
		-1.0f, 1.0f, throttle_val > 0.01f ? "Accel %.0f%%" :
		throttle_val < -0.01f ? "Brake %.0f%%" : "IDLE");
	ImGui::PopItemWidth();
	ImGui::Text("   %.1f mph  (max %.0f)", ctrl_speed_mph, MAX_SPEED);
	ImGui::Separator();

	/* RPM | Temp | Fuel */
	float col_w = (W - 28) / 3.0f;
	ImGui::PushItemWidth(col_w);

	ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "RPM");
	ImGui::SameLine(col_w + 14);
	ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "TEMP");
	ImGui::SameLine(col_w * 2 + 22);
	ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "FUEL");

	bool rpm_changed = ImGui::SliderInt("##rpm", &engine_rpm, 0, 7000, "%d");
	ImGui::SameLine();
	bool tmp_changed = ImGui::SliderInt("##temp", &coolant_temp, 60, 120, "%d");
	ImGui::SameLine();
	bool fuel_changed = ImGui::SliderInt("##fuel", &fuel_level, 0, 100, "%d%%");

	ImGui::PopItemWidth();
	if (rpm_changed) controls_send_rpm();
	if (tmp_changed) controls_send_temp();
	if (fuel_changed) controls_send_fuel();

	ImGui::Separator();

	/* signals + doors */
	ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "SIGNALS");
	ImGui::SameLine();
	bool sig_left = ImGui::Checkbox("\xe2\x97\x84 Left", (bool*)&turn_left);
	ImGui::SameLine();
	bool sig_right = ImGui::Checkbox("Right \xe2\x96\xba", (bool*)&turn_right);
	if (sig_left || sig_right) controls_send_signals();

	ImGui::SameLine(W - 250);
	ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DOORS");
	for (int d = 0; d < 4; d++) {
		bool locked = (door_state >> d) & 1;
		char label[8];
		snprintf(label, sizeof(label), "%d", d + 1);
		ImGui::SameLine();
		bool toggled = ImGui::Checkbox(label, &locked);
		if (toggled) {
			if (locked) door_state |=  (1 << d);
			else        door_state &= ~(1 << d);
			controls_send_doors();
		}
	}

	ImGui::Separator();

	/* send all */
	if (ImGui::Button("Send All Values", ImVec2(W - 16, 24)))
		controls_send_all();

	ImGui::End();
	ImGui::PopStyleVar(3);
}

/* ===== Debug overlay ===== */
static void render_debug_panel() {
	if (!show_debug) return;
	ImGui::SetNextWindowSize(ImVec2(320, 280), ImGuiCond_FirstUseEver);
	ImGui::Begin("CAN Debug", &show_debug);
	ImGui::Text("Speed:   ID 0x%03X  pos %d", g_cfg.can.speed_id, g_cfg.can.speed_pos);
	ImGui::Text("Doors:   ID 0x%03X  pos %d", g_cfg.can.door_id, g_cfg.can.door_pos);
	ImGui::Text("Signals: ID 0x%03X  pos %d", g_cfg.can.signal_id, g_cfg.can.signal_pos);
	ImGui::Text("RPM:     ID 0x%03X  pos %d", g_cfg.rpm.rpm_id, g_cfg.rpm.rpm_pos);
	ImGui::Text("Temp:    ID 0x%03X  pos %d", g_cfg.temp.temp_id, g_cfg.temp.temp_pos);
	ImGui::Text("Fuel:    ID 0x%03X  pos %d", g_cfg.fuel.fuel_id, g_cfg.fuel.fuel_pos);
	ImGui::Separator();
	ImGui::Text("Vehicle: %s", g_cfg.name);
	ImGui::Text("Speed scaling: %.4f / %d", g_cfg.speed.scaling, g_cfg.speed.divisor);
	ImGui::Text("RPM scaling:   %.4f / %d", g_cfg.rpm.scaling, g_cfg.rpm.divisor);
	ImGui::Separator();
	ImGui::Text("Recording: %s", record_path ? record_path : "(none)");
	ImGui::Text("Replaying: %s", replay_path ? replay_path : "(none)");
	ImGui::Separator();
	int can_alive = (SDL_GetTicks() - last_can_activity) < 800;
	ImGui::TextColored(can_alive ? ImVec4(0.3f, 1, 0.3f, 1) : ImVec4(1, 0.4f, 0.4f, 1),
		"CAN: %s", can_alive ? "LIVE" : "OFF");
	ImGui::Text("Frames rx: %d", frames_total);
	ImGui::Text("Replay speed: %.1fx", replay_speed);
	ImGui::Text("Uptime: %.0fs", SDL_GetTicks() / 1000.0);
	ImGui::End();
}

/* ===== Help overlay ===== */
static void render_help_overlay() {
	if (!show_help) return;

	ImGuiIO &io = ImGui::GetIO();
	ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(340, 240), ImGuiCond_Always);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
	ImGui::Begin("Keyboard Shortcuts", &show_help,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

	ImGui::TextColored(ImVec4(0.0f, 0.75f, 1.0f, 1.0f), "ICSim  —  Help");
	ImGui::Separator();
	ImGui::Spacing();

	if (controls_mode) {
		ImGui::BulletText("Mouse drag sliders to adjust values");
		ImGui::BulletText("Checkboxes send CAN frames immediately");
		ImGui::BulletText("\"Send All Values\" pushes all parameters");
		ImGui::BulletText("F11 / Alt+Enter  Fullscreen");
		ImGui::BulletText("? / F1 / Ctrl+H  Help");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1.0f), "Launch dashboard:");
		ImGui::BulletText("icsim_imgui.exe vcan0");
	} else {
		ImGui::BulletText("D          Toggle debug overlay");
		ImGui::BulletText("F11 / Alt+Enter  Fullscreen");
		ImGui::BulletText("? / F1 / Ctrl+H   Help");
		ImGui::BulletText("ESC        Exit");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1.0f), "CLI flags:");
		ImGui::BulletText("-m MODEL   Vehicle (bmw, default)");
		ImGui::BulletText("-R FILE    Record CAN to ASC");
		ImGui::BulletText("-P FILE    Replay CAN from ASC");
		ImGui::BulletText("-r         Randomize CAN IDs");
		ImGui::BulletText("--controls Control panel mode");
		ImGui::BulletText("--headless No GUI (batch mode)");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1.0f), "Recording: %s",
			can_recorder ? "ACTIVE" : "off");
		ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1.0f), "Replaying: %s",
			can_replayer ? "ACTIVE" : "off");
	}

	ImGui::Spacing();
	if (ImGui::Button("Close", ImVec2(80, 24)))
		show_help = false;

	ImGui::End();
	ImGui::PopStyleVar();
}

/* ---------- frame dispatch (dashboard mode) ---------- */
static void dispatch_frame(struct canfd_frame *f, size_t mtu,
	canid_t door_id, canid_t signal_id, canid_t speed_id,
	canid_t rpm_id, canid_t temp_id, canid_t fuel_id)
{
	int md = (mtu == CAN_MTU) ? CAN_MAX_DLEN : CANFD_MAX_DLEN;
	if (f->can_id == door_id)   update_door_status(f, md);
	if (f->can_id == signal_id) update_signal_status(f, md);
	if (f->can_id == speed_id)  update_speed_status(f, md);
	if (f->can_id == rpm_id)    update_rpm_status(f, md);
	if (f->can_id == temp_id)   update_temp_status(f, md);
	if (f->can_id == fuel_id)   update_fuel_status(f, md);
	if (can_recorder) can_log_record(can_recorder, f);
	frames_total++;
	last_can_activity = SDL_GetTicks();
}

/* ---------- main ---------- */
static void Usage(const char *msg) {
	if (msg) printf("%s\n", msg);
	printf("Usage: icsim_imgui [options] <can>\n");
	printf("  -r           randomize IDs\n");
	printf("  -s SEED      seed value\n");
	printf("  -d           debug mode\n");
	printf("  -m MODEL     vehicle model (bmw, default.toml, etc.)\n");
	printf("  -t FILE      background CAN traffic file (controls mode)\n");
	printf("  -X           disable background CAN traffic (controls mode)\n");
	printf("  -R FILE      record to ASC file\n");
	printf("  -P FILE      replay from ASC file\n");
	printf("  --controls   control panel mode (sends CAN, not receives)\n");
	printf("  --headless   run without GUI\n");
	printf("  --duration N run for N seconds\n");
	exit(1);
}

int main(int argc, char *argv[]) {
	int opt;
	can_bus_t *can;
	struct canfd_frame frame;
	struct stat dirstat;
	int running = 1;
	size_t mtu;
	SDL_Thread *traffic_thread = NULL;

	/* handle long flags before getopt (getopt_compat has no long-opt support) */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--controls") == 0)      { controls_mode = true; argv[i] = (char*)""; }
		else if (strcmp(argv[i], "--headless") == 0) { headless = 1;       argv[i] = (char*)""; }
		else if (strcmp(argv[i], "--debug-can") == 0){ debug_can = true;   argv[i] = (char*)""; }
		else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
			headless_duration = atoi(argv[i+1]);
			argv[i] = argv[i+1] = (char*)"";
		}
	}
	{
		int out = 1;
		for (int in = 1; in < argc; in++) {
			if (argv[in][0] != '\0')
				argv[out++] = argv[in];
		}
		argc = out;
		argv[argc] = NULL;
	}

	while ((opt = getopt(argc, argv, "Xrs:dm:t:h?R:P:")) != -1) {
		switch (opt) {
		case 'X': play_traffic = 0; break;
		case 'r': randomize = 1; break;
		case 's': seed = atoi(optarg); break;
		case 'd': debug = 1; break;
		case 'm': model = optarg; break;
		case 't': traffic_log = optarg; break;
		case 'R': record_path = optarg; break;
		case 'P': replay_path = optarg; break;
		default:  Usage(NULL);
		}
	}
	if (optind >= argc) Usage("Missing CAN bus name");

	if (!controls_mode && stat(DATA_DIR, &dirstat) == -1) {
		printf("ERROR: DATA_DIR not found\n");
		exit(34);
	}
	if (controls_mode && play_traffic && stat(traffic_log, &dirstat) == -1) {
		fprintf(stderr, "WARNING: CAN traffic file not found: %s\n", traffic_log);
		play_traffic = 0;
	}

	printf("Using CAN interface %s", argv[optind]);
	if (controls_mode) printf("  [controls mode]");
	printf("\n");

	if (can_bus_open(&can, argv[optind]) < 0) {
		fprintf(stderr, "%s\n", can_bus_error());
		return 1;
	}
	can_bus_set_nonblocking(can, 1);

	if (controls_mode) {
		ctrl_can = can;
	} else {
		if (record_path) {
			can_recorder = can_log_open_record(record_path);
			printf("Recording to: %s\n", record_path);
		}
		if (replay_path) {
			can_replayer = can_log_open_replay(replay_path);
			printf("Replaying from: %s\n", replay_path);
		}
	}

	/* Config */
	if (model) {
		if (!strncmp(model, "bmw", 3))
			icsim_config_load(&g_cfg, "models/bmw_x1.toml");
		else if (strchr(model, '.'))
			icsim_config_load(&g_cfg, model);
		else {
			char path[256];
			snprintf(path, sizeof(path), "models/%s.toml", model);
			icsim_config_load(&g_cfg, path);
		}
	} else {
		icsim_config_defaults(&g_cfg);
	}
	printf("Vehicle: %s\n", g_cfg.name);

	g_door_id   = g_cfg.can.door_id;
	g_signal_id = g_cfg.can.signal_id;
	g_speed_id  = g_cfg.can.speed_id;
	g_rpm_id    = g_cfg.rpm.rpm_id;
	g_temp_id   = g_cfg.temp.temp_id;
	g_fuel_id   = g_cfg.fuel.fuel_id;

	ctrl_door_id   = g_door_id;
	ctrl_signal_id = g_signal_id;
	ctrl_speed_id  = g_speed_id;
	ctrl_rpm_id    = g_rpm_id;
	ctrl_temp_id   = g_temp_id;
	ctrl_fuel_id   = g_fuel_id;
	ctrl_door_pos   = g_cfg.can.door_pos;
	ctrl_signal_pos = g_cfg.can.signal_pos;
	ctrl_speed_pos  = g_cfg.can.speed_pos;

	if (randomize || seed) {
		if (randomize) seed = (int)time(NULL);
		srand((unsigned int)seed);
		g_door_id = rand() % 2046 + 1;
		g_signal_id = rand() % 2046 + 1;
		g_speed_id = rand() % 2046 + 1;
		printf("Seed: %d\n", seed);
	}

	/* scan available models, restore last choice */
	scan_models();
	{
		char saved[256] = {0};
		FILE *sf = fopen("icsim_model.txt", "r");
		if (sf) {
			if (fgets(saved, sizeof(saved), sf)) {
				size_t len = strlen(saved);
				while (len > 0 && (saved[len-1] == '\n' || saved[len-1] == '\r'))
					saved[--len] = '\0';
				for (int i = 0; i < (int)model_list.size(); i++) {
					if (model_list[i] == saved) {
						current_model_idx = i;
						break;
					}
				}
			}
			fclose(sf);
		}
	}

	/* SDL + ImGui init */
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("SDL init failed\n");
		exit(40);
	}
	int win_w = controls_mode ? 480 : 800;
	int win_h = controls_mode ? 370 : 400;
	const char *win_title = controls_mode ? "ICSim Controls" : "ICSim Dashboard";
	SDL_Window *window = SDL_CreateWindow(win_title,
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
		SDL_RENDERER_ACCELERATED);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = "icsim.ini";
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui::StyleColorsDark();
	ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
	ImGui_ImplSDLRenderer2_Init(renderer);

	replay_base_tick = SDL_GetTicks();
	Uint32 last_speed_update = 0;

	/* initial send in controls mode so dashboard sees CAN immediately */
	if (controls_mode) controls_send_all();
	if (controls_mode && play_traffic)
		traffic_thread = SDL_CreateThread(play_can_traffic_thread,
			"icsim-bg-can", NULL);

	while (running) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			ImGui_ImplSDL2_ProcessEvent(&event);
			if (event.type == SDL_QUIT) running = 0;
			if (event.type == SDL_KEYDOWN) {
				SDL_Keycode k = event.key.keysym.sym;
				Uint16 mod = SDL_GetModState();
				if (k == SDLK_d && !controls_mode) show_debug = !show_debug;
				if (k == SDLK_SLASH || k == SDLK_F1 || (k == SDLK_h && (mod & KMOD_CTRL)))
					show_help = !show_help;
				if (k == SDLK_F11 || (k == SDLK_RETURN && (mod & KMOD_ALT))) {
					fullscreen = !fullscreen;
					SDL_SetWindowFullscreen(window,
						fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
				}
			}
		}

		if (controls_mode) {
			/* throttle → speed simulation */
			Uint32 now = SDL_GetTicks();
			if (now - last_speed_update > 33) {
				float dt = (now - last_speed_update) / 1000.0f;
				float rate = MAX_SPEED / ACCEL_RATE;
				ctrl_speed_mph += throttle_val * rate * dt;
				if (ctrl_speed_mph < 0.0f) ctrl_speed_mph = 0.0f;
				if (ctrl_speed_mph > MAX_SPEED) ctrl_speed_mph = MAX_SPEED;
				current_speed = (long)(ctrl_speed_mph + 0.5f);
				controls_send_speed();
				last_speed_update = now;
			}
		} else {
			/* CAN receive */
			for (int batch = 0; batch < 64; batch++) {
				if (can_bus_recv(can, &frame, &mtu) < 0) {
					if (can_bus_error_is_would_block()) break;
					fprintf(stderr, "%s\n", can_bus_error());
					running = 0; break;
				}
				if (debug_can) fprintf(stderr, "[CAN RX] id=0x%03X len=%d\n", frame.can_id, frame.len);
				dispatch_frame(&frame, mtu, g_door_id, g_signal_id, g_speed_id,
					g_rpm_id, g_temp_id, g_fuel_id);
			}

			/* replay injection */
			if (can_replayer) {
				double toff; size_t rmtu; struct canfd_frame rf;
				while (can_log_replay_next(can_replayer, &rf, &toff, &rmtu)) {
					double elapsed = (double)(SDL_GetTicks() - replay_base_tick) / 1000.0;
					if (elapsed * replay_speed < toff) break;
					can_bus_send(can, &rf, rmtu);
					if (can_recorder) can_log_record(can_recorder, &rf);
				}
			}
		}

		/* render */
		ImGui_ImplSDLRenderer2_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();

		if (controls_mode)
			render_controls();
		else {
			render_dashboard();
			render_debug_panel();
		}
		render_help_overlay();

		ImGui::Render();
		SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
		SDL_RenderClear(renderer);
		ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
		SDL_RenderPresent(renderer);

		/* FPS limiter: target ~60 FPS (~16.67 ms/frame) */
		{
			static Uint32 last_frame = 0;
			Uint32 now_f = SDL_GetTicks();
			Uint32 elapsed = now_f - last_frame;
			if (elapsed < 16)
				SDL_Delay(16 - elapsed);
			last_frame = SDL_GetTicks();
		}
	}

	/* cleanup */
	traffic_running = 0;
	if (traffic_thread)
		SDL_WaitThread(traffic_thread, NULL);

	ImGui_ImplSDLRenderer2_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();

	if (can_recorder) can_log_close(can_recorder);
	if (can_replayer) can_log_close(can_replayer);
	can_bus_close(can);
	return 0;
}
