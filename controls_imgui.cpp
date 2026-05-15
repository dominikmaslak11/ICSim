/* ICSim Controls — ImGui version
 *
 * Control panel for the IC Simulator using ImGui sliders, buttons,
 * and checkboxes instead of a gamepad image + SDL key handlers.
 */
#include "can_platform.h"
#include "config.h"

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_sdl2.h"
#include "imgui/backends/imgui_impl_sdlrenderer2.h"

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#endif

/* ---------- state ---------- */
static icsim_config_t g_cfg;

static can_bus_t *s;
static struct canfd_frame cf;

static int door_id, signal_id, speed_id;
static int rpm_id, temp_id, fuel_id;
static int door_pos, signal_pos, speed_pos;

static float throttle_val = 0.0f;
static float current_speed = 0.0f;
static int engine_rpm = 800;
static int coolant_temp = 80;
static int fuel_level = 75;
static int door_state = 0x0F;     /* all locked */
static int turn_left = 0, turn_right = 0;

#define MAX_SPEED 90.0f
#define ACCEL_RATE 8.0f

/* ---------- CAN send helpers ---------- */
static void send_pkt(int mtu) {
	if (can_bus_send(s, &cf, (size_t)mtu) < 0)
		fprintf(stderr, "%s\n", can_bus_error());
}

static void send_speed() {
	float kph = current_speed / 0.6213751f * 100.0f;
	memset(&cf, 0, sizeof(cf));
	cf.can_id = speed_id;
	cf.len = (uint8_t)(speed_pos + 2);
	cf.data[speed_pos + 1] = (uint8_t)((int)kph & 0xFF);
	cf.data[speed_pos]     = (uint8_t)(((int)kph >> 8) & 0xFF);
	send_pkt(CAN_MTU);
}

static void send_doors() {
	memset(&cf, 0, sizeof(cf));
	cf.can_id = door_id;
	cf.len = (uint8_t)(door_pos + 1);
	cf.data[door_pos] = (uint8_t)door_state;
	send_pkt(CAN_MTU);
}

static void send_signals() {
	uint8_t sig = 0;
	if (turn_left)  sig |= ICSIM_TURN_LEFT;
	if (turn_right) sig |= ICSIM_TURN_RIGHT;
	memset(&cf, 0, sizeof(cf));
	cf.can_id = signal_id;
	cf.len = (uint8_t)(signal_pos + 1);
	cf.data[signal_pos] = sig;
	send_pkt(CAN_MTU);
}

static void send_rpm() {
	int raw = engine_rpm * g_cfg.rpm.divisor;
	memset(&cf, 0, sizeof(cf));
	cf.can_id = rpm_id;
	cf.len = (uint8_t)(g_cfg.rpm.rpm_pos + 2);
	cf.data[g_cfg.rpm.rpm_pos + 1] = raw & 0xFF;
	cf.data[g_cfg.rpm.rpm_pos]     = (raw >> 8) & 0xFF;
	send_pkt(CAN_MTU);
}

static void send_temp() {
	memset(&cf, 0, sizeof(cf));
	cf.can_id = temp_id;
	cf.len = (uint8_t)(g_cfg.temp.temp_pos + 1);
	cf.data[g_cfg.temp.temp_pos] = (uint8_t)coolant_temp;
	send_pkt(CAN_MTU);
}

static void send_fuel() {
	memset(&cf, 0, sizeof(cf));
	cf.can_id = fuel_id;
	cf.len = (uint8_t)(g_cfg.fuel.fuel_pos + 1);
	cf.data[g_cfg.fuel.fuel_pos] = (uint8_t)fuel_level;
	send_pkt(CAN_MTU);
}

/* ---------- ImGui UI ---------- */
static void render_controls() {
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
	ImGui::Begin("ICSim Controls", NULL,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove);

	ImGui::TextColored(ImVec4(0, 0.8f, 1, 1), "CANBus Control Panel (ImGui)");
	ImGui::Separator();

	/* Throttle / speed */
	ImGui::Text("Throttle / Speed");
	if (ImGui::SliderFloat("##throttle", &throttle_val, -1.0f, 1.0f, "%.0f%%")) {
		/* no immediate send — speed is sent by timer below */
	}
	ImGui::Text("Speed: %.1f mph", current_speed);

	/* Turn signals */
	ImGui::Spacing();
	ImGui::Text("Turn Signals");
	if (ImGui::Checkbox("Left", (bool*)&turn_left))  send_signals();
	ImGui::SameLine();
	if (ImGui::Checkbox("Right", (bool*)&turn_right)) send_signals();

	/* Doors */
	ImGui::Spacing();
	ImGui::Text("Doors");
	for (int d = 0; d < 4; d++) {
		bool locked = (door_state >> d) & 1;
		char label[32];
		snprintf(label, sizeof(label), "Door %d %s", d + 1, locked ? "(locked)" : "(open)");
		if (ImGui::Checkbox(label, &locked)) {
			if (locked) door_state |=  (1 << d);
			else        door_state &= ~(1 << d);
			send_doors();
		}
	}

	/* RPM */
	ImGui::Spacing();
	ImGui::Text("Engine RPM");
	if (ImGui::SliderInt("##rpm", &engine_rpm, 0, 7000, "%d")) send_rpm();

	/* Coolant */
	ImGui::Text("Coolant Temp");
	if (ImGui::SliderInt("##temp", &coolant_temp, 60, 120, "%d C")) send_temp();

	/* Fuel */
	ImGui::Text("Fuel Level");
	if (ImGui::SliderInt("##fuel", &fuel_level, 0, 100, "%d %%")) send_fuel();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

	ImGui::End();
}

/* ---------- main ---------- */
int main(int argc, char *argv[]) {
	const char *bus_name = "vcan0";
	char *model = NULL;

	for (int i = 1; i < argc; i++) {
		if (argv[i][0] != '-') bus_name = argv[i];
		else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) model = argv[++i];
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
	} else icsim_config_defaults(&g_cfg);

	door_id   = g_cfg.can.door_id;
	signal_id = g_cfg.can.signal_id;
	speed_id  = g_cfg.can.speed_id;
	rpm_id    = g_cfg.rpm.rpm_id;
	temp_id   = g_cfg.temp.temp_id;
	fuel_id   = g_cfg.fuel.fuel_id;
	door_pos   = g_cfg.can.door_pos;
	signal_pos = g_cfg.can.signal_pos;
	speed_pos  = g_cfg.can.speed_pos;

	if (can_bus_open(&s, bus_name) < 0) {
		fprintf(stderr, "%s\n", can_bus_error());
		return 1;
	}

	/* SDL + ImGui */
	SDL_Init(SDL_INIT_VIDEO);
	SDL_Window *window = SDL_CreateWindow("ICSim Controls ImGui",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		350, 450, SDL_WINDOW_SHOWN);
	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
		SDL_RENDERER_ACCELERATED);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
	ImGui_ImplSDLRenderer2_Init(renderer);

	int running = 1;
	Uint32 last_speed_update = 0;

	while (running) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			ImGui_ImplSDL2_ProcessEvent(&event);
			if (event.type == SDL_QUIT) running = 0;
		}

		/* Update speed based on throttle */
		Uint32 now = SDL_GetTicks();
		if (now - last_speed_update > 33) {
			float rate = MAX_SPEED / (ACCEL_RATE * 100.0f);
			if (throttle_val > 0.01f)
				current_speed += rate;
			else if (throttle_val < -0.01f) {
				current_speed -= rate;
				if (current_speed < 0) current_speed = 0;
			}
			if (current_speed > MAX_SPEED) current_speed = MAX_SPEED;
			send_speed();
			last_speed_update = now;
		}

		/* Render */
		ImGui_ImplSDLRenderer2_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();
		render_controls();
		ImGui::Render();
		SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
		SDL_RenderClear(renderer);
		ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
		SDL_RenderPresent(renderer);
		SDL_Delay(5);
	}

	ImGui_ImplSDLRenderer2_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	can_bus_close(s);
	return 0;
}
