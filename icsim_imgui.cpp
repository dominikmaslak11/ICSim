/* ICSim — Dear ImGui version
 *
 * Replaces SDL sprite rendering with ImGui widgets:
 *   - Circular speedometer gauge
 *   - Door/turn signal indicators
 *   - RPM, temp, fuel progress bars
 *   - Debug CAN frame viewer
 *
 * CAN processing logic is shared with the original icsim.c.
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

extern "C" {
#include "getopt_compat.h"
}

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef DATA_DIR
#define DATA_DIR "./data/"
#endif

/* ---------- global state ---------- */
static icsim_config_t g_cfg;

static int debug = 0;
static int randomize = 0;
static int seed = 0;
static long current_speed = 0;
static int engine_rpm = 800;
static int coolant_temp = 80;
static int fuel_level = 75;
static int door_status[4] = {0, 0, 0, 0};
static int turn_status[2] = {0, 0};
static char *model = NULL;
static char *record_path = NULL;
static char *replay_path = NULL;
static int headless = 0, headless_duration = 0;
static can_log_t *can_recorder = NULL;
static can_log_t *can_replayer = NULL;
static int frames_total = 0;
static Uint32 last_can_activity = 0;
static Uint32 replay_base_tick = 0;

static bool show_debug = false;

#define DOOR_LOCKED 0
#define DOOR_UNLOCKED 1
#define OFF 0
#define ON 1

/* ---------- CAN frame handlers (same logic as icsim.c) ---------- */

static long map_val(long x, long a1, long a2, long b1, long b2) {
	return (x - a1) * (b2 - b1) / (a2 - a1) + b1;
}

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

/* ---------- ImGui rendering ---------- */

static void draw_speedometer(float cx, float cy, float radius) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	float angle = (float)map_val(current_speed, 0, 280, 135, 405);
	float rad = (angle - 90.0f) * 3.14159f / 180.0f;
	float nx = cx + cosf(rad) * radius * 0.7f;
	float ny = cy + sinf(rad) * radius * 0.7f;

	/* Background arc */
	dl->PathArcTo(ImVec2(cx, cy), radius, 2.36f, 7.07f, 32);
	dl->PathStroke(ImColor(60, 60, 60), false, 8.0f);

	/* Active arc */
	for (int i = 0; i < (int)((angle - 135.0f) / 2.7f); i++) {
		float a = (135.0f + (float)i * 2.7f) * 3.14159f / 180.0f;
		float x = cx + cosf(a) * radius * 0.85f;
		float y = cy + sinf(a) * radius * 0.85f;
		dl->AddCircleFilled(ImVec2(x, y), 3.0f, ImColor(0, 255, 255));
	}

	/* Speed text */
	char buf[32];
	snprintf(buf, sizeof(buf), "%ld", current_speed);
	dl->AddText(ImVec2(cx - 30, cy + 10), ImColor(255, 255, 255), buf);

	/* Needle */
	dl->AddLine(ImVec2(cx, cy), ImVec2(nx, ny), ImColor(255, 60, 60), 3.0f);
	dl->AddCircleFilled(ImVec2(cx, cy), 6.0f, ImColor(200, 200, 200));
}

static void render_dashboard() {
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
	ImGui::Begin("ICSim Dashboard", NULL,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoBringToFrontOnFocus);

	/* Top bar: title + CAN activity */
	ImGui::TextColored(ImVec4(0, 0.8f, 1, 1), "ICSim ImGui");
	ImGui::SameLine();
	int active = (SDL_GetTicks() - last_can_activity) < 1000;
	ImGui::TextColored(active ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0.2f, 0.2f, 1),
		active ? " CAN ACTIVE" : " NO CAN");
	ImGui::SameLine();
	ImGui::Text("  frames: %d", frames_total);
	if (can_replayer) {
		ImGui::SameLine();
		ImGui::ProgressBar((float)(SDL_GetTicks() - replay_base_tick) / 10000.0f,
			ImVec2(100, 12), "replay");
	}
	ImGui::Separator();

	/* Speedometer */
	draw_speedometer(170, 160, 110);

	/* Right side: door indicators */
	ImGui::SetCursorPos(ImVec2(340, 30));
	ImGui::Text("Doors:");
	for (int d = 0; d < 4; d++) {
		ImGui::SetCursorPos(ImVec2(340, 55.0f + d * 25.0f));
		if (door_status[d] == DOOR_UNLOCKED) {
			ImGui::TextColored(ImVec4(1, 0, 0, 1), "  Door %d: UNLOCKED", d + 1);
		} else {
			ImGui::Text("  Door %d: locked", d + 1);
		}
	}

	/* Turn signals */
	ImGui::SetCursorPos(ImVec2(340, 165));
	ImGui::Text("Left:  %s", turn_status[0] == ON ? "<-- ON" : "off");
	ImGui::SetCursorPos(ImVec2(340, 185));
	ImGui::Text("Right: %s", turn_status[1] == ON ? "ON -->" : "off");

	/* RPM gauge */
	ImGui::SetCursorPos(ImVec2(20, 220));
	ImGui::Text("RPM");
	ImGui::SetCursorPos(ImVec2(60, 220));
	ImGui::PushItemWidth(200);
	float rpm_pct = engine_rpm / 7000.0f;
	if (rpm_pct > 1.0f) rpm_pct = 1.0f;
	ImGui::ProgressBar(rpm_pct, ImVec2(0, 14), "");
	ImGui::SameLine();
	ImGui::Text(" %d", engine_rpm);

	/* Temp gauge */
	ImGui::SetCursorPos(ImVec2(20, 242));
	ImGui::Text("TEMP");
	ImGui::SetCursorPos(ImVec2(60, 242));
	float temp_pct = (coolant_temp - 60.0f) / 60.0f;
	if (temp_pct < 0) temp_pct = 0;
	if (temp_pct > 1.0f) temp_pct = 1.0f;
	ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
		temp_pct > 0.75f ? ImVec4(1, 0.2f, 0.2f, 1) : ImVec4(0, 0.8f, 0, 1));
	ImGui::ProgressBar(temp_pct, ImVec2(0, 14), "");
	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::Text(" %dC", coolant_temp);

	/* Fuel gauge */
	ImGui::SetCursorPos(ImVec2(20, 264));
	ImGui::Text("FUEL");
	ImGui::SetCursorPos(ImVec2(60, 264));
	float fuel_pct = fuel_level / 100.0f;
	ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
		fuel_pct < 0.15f ? ImVec4(1, 0.2f, 0.2f, 1) : ImVec4(0, 0.7f, 0, 1));
	ImGui::ProgressBar(fuel_pct, ImVec2(0, 14), "");
	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::Text(" %d%%", fuel_level);

	ImGui::PopItemWidth();

	/* FPS + status bar */
	ImGui::SetCursorPos(ImVec2(20, 300));
	ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
	ImGui::SameLine();
	ImGui::Text(" | Press D for debug panel");

	ImGui::End();
}

static void render_debug_panel() {
	if (!show_debug) return;
	ImGui::Begin("CAN Debug", &show_debug);
	ImGui::Text("Speed:   0x%03X (%d)", g_cfg.can.speed_id, g_cfg.can.speed_id);
	ImGui::Text("Doors:   0x%03X (%d)", g_cfg.can.door_id, g_cfg.can.door_id);
	ImGui::Text("Signals: 0x%03X (%d)", g_cfg.can.signal_id, g_cfg.can.signal_id);
	ImGui::Text("RPM:     0x%03X (%d)", g_cfg.rpm.rpm_id, g_cfg.rpm.rpm_id);
	ImGui::Text("Temp:    0x%03X (%d)", g_cfg.temp.temp_id, g_cfg.temp.temp_id);
	ImGui::Text("Fuel:    0x%03X (%d)", g_cfg.fuel.fuel_id, g_cfg.fuel.fuel_id);
	ImGui::Separator();
	ImGui::Text("Vehicle: %s", g_cfg.name);
	ImGui::Text("Recording: %s", record_path ? record_path : "none");
	ImGui::Text("Replaying: %s", replay_path ? replay_path : "none");
	ImGui::End();
}

/* ---------- frame dispatch ---------- */
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
	printf("  -r        randomize IDs\n");
	printf("  -s SEED   seed value\n");
	printf("  -d        debug mode\n");
	printf("  -m MODEL  vehicle model (bmw, default.toml, etc.)\n");
	printf("  -R FILE   record to ASC file\n");
	printf("  -P FILE   replay from ASC file\n");
	printf("  --headless  run without GUI\n");
	printf("  --duration N  run for N seconds\n");
	exit(1);
}

int main(int argc, char *argv[]) {
	int opt;
	can_bus_t *can;
	struct canfd_frame frame;
	struct stat dirstat;
	int running = 1;
	size_t mtu;
	canid_t door_id, signal_id, speed_id;
	canid_t rpm_id, temp_id, fuel_id;

	while ((opt = getopt(argc, argv, "rs:dm:h?R:P:")) != -1) {
		switch (opt) {
		case 'r': randomize = 1; break;
		case 's': seed = atoi(optarg); break;
		case 'd': debug = 1; break;
		case 'm': model = optarg; break;
		case 'R': record_path = optarg; break;
		case 'P': replay_path = optarg; break;
		default:  Usage(NULL);
		}
	}
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--headless") == 0) headless = 1;
		else if (strcmp(argv[i], "--duration") == 0 && i+1 < argc)
			headless_duration = atoi(argv[++i]);
	}
	if (optind >= argc) Usage("Missing CAN bus name");

	if (stat(DATA_DIR, &dirstat) == -1) {
		printf("ERROR: DATA_DIR not found\n");
		exit(34);
	}

	printf("Using CAN interface %s\n", argv[optind]);
	if (can_bus_open(&can, argv[optind]) < 0) {
		fprintf(stderr, "%s\n", can_bus_error());
		exit(1);
	}
	can_bus_set_nonblocking(can, 1);

	if (record_path) {
		can_recorder = can_log_open_record(record_path);
		printf("Recording to: %s\n", record_path);
	}
	if (replay_path) {
		can_replayer = can_log_open_replay(replay_path);
		printf("Replaying from: %s\n", replay_path);
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

	door_id = g_cfg.can.door_id; signal_id = g_cfg.can.signal_id;
	speed_id = g_cfg.can.speed_id;
	rpm_id = g_cfg.rpm.rpm_id; temp_id = g_cfg.temp.temp_id;
	fuel_id = g_cfg.fuel.fuel_id;

	if (randomize || seed) {
		if (randomize) seed = (int)time(NULL);
		srand((unsigned int)seed);
		door_id = rand() % 2046 + 1;
		signal_id = rand() % 2046 + 1;
		speed_id = rand() % 2046 + 1;
		printf("Seed: %d\n", seed);
	}

	/* SDL init */
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("SDL init failed\n");
		exit(40);
	}
	SDL_Window *window = SDL_CreateWindow("ICSim ImGui",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		500, 340, SDL_WINDOW_SHOWN);
	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
		SDL_RENDERER_ACCELERATED);

	/* ImGui init */
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui::StyleColorsDark();
	ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
	ImGui_ImplSDLRenderer2_Init(renderer);

	replay_base_tick = SDL_GetTicks();

	while (running) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			ImGui_ImplSDL2_ProcessEvent(&event);
			if (event.type == SDL_QUIT) running = 0;
			if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_d)
				show_debug = !show_debug;
		}

		/* CAN polling */
		for (int batch = 0; batch < 64; batch++) {
			if (can_bus_recv(can, &frame, &mtu) < 0) {
				if (can_bus_error_is_would_block()) break;
				fprintf(stderr, "%s\n", can_bus_error());
				running = 0; break;
			}
			dispatch_frame(&frame, mtu, door_id, signal_id, speed_id,
				rpm_id, temp_id, fuel_id);
		}

		/* Replay injection */
		if (can_replayer) {
			double toff; size_t rmtu; struct canfd_frame rf;
			while (can_log_replay_next(can_replayer, &rf, &toff, &rmtu)) {
				if ((double)(SDL_GetTicks() - replay_base_tick) / 1000.0 < toff)
					break;
				can_bus_send(can, &rf, rmtu);
				if (can_recorder) can_log_record(can_recorder, &rf);
			}
		}

		/* Render ImGui */
		ImGui_ImplSDLRenderer2_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();

		render_dashboard();
		render_debug_panel();

		ImGui::Render();
		SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
		SDL_RenderClear(renderer);
		ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
		SDL_RenderPresent(renderer);

		SDL_Delay(5);
	}

	/* Cleanup */
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
