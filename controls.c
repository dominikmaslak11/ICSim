/*
 * Control panel for IC Simulation
 *
 * OpenGarages 
 *
 * craig@theialabs.com
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

#include "getopt_compat.h"
#include "can_platform.h"
#include "config.h"
#include "lib.h"

#ifndef DATA_DIR
#define DATA_DIR "./data/"
#endif
#define DEFAULT_CAN_TRAFFIC DATA_DIR "sample-can.log"

#define DEFAULT_DIFFICULTY 1
// 0 = No randomization added to the packets other than location and ID
// 1 = Add NULL padding
// 2 = Randomize unused bytes
/* CAN IDs, byte positions, and signal bitmasks are now loaded from
 * TOML config files via config.h / config.c.  The defaults match
 * the original hardcoded values. */
#define ON 1
#define OFF 0
#define DOOR_LOCKED 0
#define DOOR_UNLOCKED 1
#define SCREEN_WIDTH 835
#define SCREEN_HEIGHT 608
#define JOY_UNKNOWN -1
#define BUTTON_LOCK 4
#define PS3_BUTTON_LOCK 10
#define BUTTON_UNLOCK 5
#define PS3_BUTTON_UNLOCK 11
#define BUTTON_A 0
#define PS3_BUTTON_A 14
#define BUTTON_B 1
#define PS3_BUTTON_B 13
#define BUTTON_X 2
#define PS3_BUTTON_X 15 
#define BUTTON_Y 3
#define PS3_BUTTON_Y 12
#define BUTTON_START 7
#define PS3_BUTTON_START 3
#define AXIS_LEFT_V 0
#define PS3_AXIS_LEFT_V 0
#define AXIS_LEFT_H 1
#define PS3_AXIS_LEFT_H 1
#define AXIS_L2 2
#define PS3_AXIS_L2 12
#define AXIS_RIGHT_H 3
#define PS3_AXIS_RIGHT_H 3
#define AXIS_RIGHT_V 4
#define PS3_AXIS_RIGHT_V 2
#define AXIS_R2 5
#define PS3_AXIS_R2 13
#define PS3_X_ROT 4
#define PS3_Y_ROT 5
#define PS3_Z_ROT 6 // The rotations are just guessed
#define MAX_SPEED 90.0 // Limiter 260.0 is full guage speed
#define ACCEL_RATE 8.0 // 0-MAX_SPEED in seconds
#define USB_CONTROLLER 0
#define PS3_CONTROLLER 1

/* Model constants are now in TOML config files (models/ *.toml) */
static icsim_config_t g_cfg;

int gButtonY = BUTTON_Y;
int gButtonX = BUTTON_X;
int gButtonA = BUTTON_A;
int gButtonB = BUTTON_B;
int gButtonStart = BUTTON_START;
int gButtonLock = BUTTON_LOCK;
int gButtonUnlock = BUTTON_UNLOCK;
int gAxisL2 = AXIS_L2;
int gAxisR2 = AXIS_R2;
int gAxisRightH = AXIS_RIGHT_H;
int gAxisRightV = AXIS_RIGHT_V;
int gAxisLeftH = AXIS_LEFT_H;
int gAxisLeftV = AXIS_LEFT_V;
// Acelleromoter axis info
int gJoyX = JOY_UNKNOWN;
int gJoyY = JOY_UNKNOWN;
int gJoyZ = JOY_UNKNOWN;

//Analog joystick dead zone
const int JOYSTICK_DEAD_ZONE = 8000;
int gLastAccelValue = 0; // Non analog R2

can_bus_t *s;
struct canfd_frame cf;
char *traffic_log = DEFAULT_CAN_TRAFFIC;
char can_name[64];
int door_len;
int signal_len;
int speed_len;
int difficulty = DEFAULT_DIFFICULTY;
char *model = NULL;

int lock_enabled = 0;
int unlock_enabled = 0;
char door_state = 0xf;
char signal_state = 0;
int throttle = 0;
float current_speed = 0;
int engine_rpm = 800;
int coolant_temp = 80;
int fuel_level = 75;
int turning = 0;
int door_id, signal_id, speed_id;
int rpm_id, temp_id, fuel_id;
int currentTime;
int lastAccel = 0;
int lastTurnSignal = 0;

int seed = 0;
int debug = 0;

int play_id;
char data_file[256];
SDL_GameController *gGameController = NULL;
SDL_Joystick *gJoystick = NULL;
SDL_Haptic *gHaptic = NULL;
SDL_Renderer *renderer = NULL;
SDL_Texture *base_texture = NULL;
int gControllerType = USB_CONTROLLER;
volatile int traffic_running = 1;

// Adds data dir to file name
// Uses a single pointer so not to have a memory leak
// returns point to data_files or NULL if append is too large
char *get_data(char *fname) {
  if(strlen(DATA_DIR) + strlen(fname) > 255) return NULL;
  strncpy(data_file, DATA_DIR, 255);
  strncat(data_file, fname, 255-strlen(data_file));
  return data_file;
}


void send_pkt(int mtu) {
  if(can_bus_send(s, &cf, mtu) < 0) {
	fprintf(stderr, "%s\n", can_bus_error());
  }
}

// Randomizes bytes in CAN packet if difficulty is hard enough
void randomize_pkt(int start, int stop) {
	if (difficulty < 2) return;
	int i = start;
	for(;i < stop;i++) {
		if(rand() % 3 < 1) cf.data[i] = rand() % 255;
	}
}

void send_lock(char door) {
	door_state |= door;
	memset(&cf, 0, sizeof(cf));
	cf.can_id = door_id;
	cf.len = door_len;
	cf.data[g_cfg.can.door_pos] = door_state;
	if (g_cfg.can.door_pos) randomize_pkt(0, g_cfg.can.door_pos);
	if (door_len != g_cfg.can.door_pos + 1) randomize_pkt(g_cfg.can.door_pos + 1, door_len);
	send_pkt(CAN_MTU);
}

void send_unlock(char door) {
	door_state &= ~door;
	memset(&cf, 0, sizeof(cf));
	cf.can_id = door_id;
	cf.len = door_len;
	cf.data[g_cfg.can.door_pos] = door_state;
	if (g_cfg.can.door_pos) randomize_pkt(0, g_cfg.can.door_pos);
	if (door_len != g_cfg.can.door_pos + 1) randomize_pkt(g_cfg.can.door_pos + 1, door_len);
	send_pkt(CAN_MTU);
}

void toggle_door(char door) {
	if (door_state & door) {
		send_unlock(door);
	} else {
		send_lock(door);
	}
}

void send_speed() {
	int sp = g_cfg.can.speed_pos;
	if (model) {
		if (!strncmp(model, "bmw", 3)) {
		        int b = ((16 * current_speed)/256) + 208;
			int a = 16 * current_speed - ((b-208) * 256);
		        memset(&cf, 0, sizeof(cf));
		        cf.can_id = speed_id;
		        cf.len = speed_len;
		        cf.data[sp+1] = (char)b & 0xff;
		        cf.data[sp] = (char)a & 0xff;
		        if(current_speed == 0) { // IDLE
		                cf.data[sp] = rand() % 80;
		                cf.data[sp+1] = 208;
		        }
		        if (sp) randomize_pkt(0, sp);
		        if (speed_len != sp + 2) randomize_pkt(sp+2, speed_len);
		        send_pkt(CAN_MTU);
		}
	} else {
		int kph = (current_speed / g_cfg.speed.scaling) * g_cfg.speed.divisor;
		memset(&cf, 0, sizeof(cf));
		cf.can_id = speed_id;
		cf.len = speed_len;
		cf.data[sp+1] = (char)kph & 0xff;
		cf.data[sp] = (char)(kph >> 8) & 0xff;
		if(kph == 0) { // IDLE
			cf.data[sp] = 1;
			cf.data[sp+1] = rand() % 255+100;
		}
		if (sp) randomize_pkt(0, sp);
		if (speed_len != sp + 2) randomize_pkt(sp+2, speed_len);
		send_pkt(CAN_MTU);
	}
}

void send_turn_signal() {
	memset(&cf, 0, sizeof(cf));
	cf.can_id = signal_id;
	cf.len = signal_len;
	cf.data[g_cfg.can.signal_pos] = signal_state;
	if(g_cfg.can.signal_pos) randomize_pkt(0, g_cfg.can.signal_pos);
	if(signal_len != g_cfg.can.signal_pos + 1) randomize_pkt(g_cfg.can.signal_pos + 1, signal_len);
	send_pkt(CAN_MTU);
}

void send_rpm() {
	int raw = (int)((double)engine_rpm * (double)g_cfg.rpm.divisor / g_cfg.rpm.scaling);
	memset(&cf, 0, sizeof(cf));
	cf.can_id = rpm_id;
	cf.len = g_cfg.rpm.length + g_cfg.rpm.rpm_pos;
	cf.data[g_cfg.rpm.rpm_pos + 1] = (char)raw & 0xff;
	cf.data[g_cfg.rpm.rpm_pos] = (char)(raw >> 8) & 0xff;
	send_pkt(CAN_MTU);
}

void send_temp() {
	int raw = (int)((double)coolant_temp * (double)g_cfg.temp.divisor / g_cfg.temp.scaling);
	memset(&cf, 0, sizeof(cf));
	cf.can_id = temp_id;
	cf.len = g_cfg.temp.length + g_cfg.temp.temp_pos;
	cf.data[g_cfg.temp.temp_pos] = (char)raw & 0xff;
	send_pkt(CAN_MTU);
}

void send_fuel() {
	int raw = (int)((double)fuel_level * (double)g_cfg.fuel.divisor / g_cfg.fuel.scaling);
	memset(&cf, 0, sizeof(cf));
	cf.can_id = fuel_id;
	cf.len = g_cfg.fuel.length + g_cfg.fuel.fuel_pos;
	cf.data[g_cfg.fuel.fuel_pos] = (char)raw & 0xff;
	send_pkt(CAN_MTU);
}

// Checks throttle to see if we should accelerate or decelerate the vehicle
void checkAccel() {
	float rate = MAX_SPEED / (ACCEL_RATE * 100);
	// Updated around 30 Hz to keep the UI responsive while preserving smooth motion.
	if(currentTime > lastAccel + 33) {
		if(throttle < 0) {
			current_speed -= rate;
			if(current_speed < 1) current_speed = 0;
		} else if(throttle > 0) {
			current_speed += rate;
			if(current_speed > MAX_SPEED) { // Limiter
				current_speed = MAX_SPEED;
				if(gHaptic != NULL) {SDL_HapticRumblePlay( gHaptic, 0.5, 1000); printf("DEBUG HAPTIC\n"); }
			}
		}
		send_speed();
		lastAccel = currentTime;
	}
}

// Checks if turning and activates the turn signal
void checkTurn() {
	if(currentTime > lastTurnSignal + 500) {
		if(turning < 0) {
			signal_state ^= ICSIM_TURN_LEFT;
		} else if(turning > 0) {
			signal_state ^= ICSIM_TURN_RIGHT;
		} else {
			signal_state = 0;
		}
		send_turn_signal();
		lastTurnSignal = currentTime;
	}
}

// Takes R2 joystick value and converts it to throttle speed
void accelerate(int value) {
	// Check dead zones
	if(gControllerType == PS3_CONTROLLER) {
		// PS3 works different.  the value range is 0-32k
		if (value < gLastAccelValue) {
			throttle = -1;
		} else if (value > gLastAccelValue) {
			throttle = 1;
		} else {
			throttle = 0;
		}
		gLastAccelValue = value;
	} else {
		if(value < -JOYSTICK_DEAD_ZONE) {
			throttle = -1;
		} else if(value > JOYSTICK_DEAD_ZONE) {
			throttle = 1;
		} else {
			throttle = 0;
		}
	}
}

// Check LEFT_V axis to see if we are turning
void turn(int value) {
	if(value < -JOYSTICK_DEAD_ZONE) {
		turning = -1;
	} else if(value > JOYSTICK_DEAD_ZONE) {
		turning = 1;
	} else {
		turning = 0;
	}
}

void ud(int value) {
	if(value < -JOYSTICK_DEAD_ZONE) {
	} else if(value > JOYSTICK_DEAD_ZONE) {
	}
}

// Plays background can traffic
void play_can_traffic() {
#ifndef _WIN32
	char can2can[80];
	snprintf(can2can, sizeof(can2can), "%s=can0", can_name);
	if(execlp("canplayer", "canplayer", "-I", traffic_log, "-l", "i", can2can, NULL) == -1) printf("WARNING: Could not execute canplayer. No bg data\n");
#else
	printf("WARNING: Background canplayer traffic is not available on Windows\n");
#endif
}

int play_can_traffic_thread(void *unused) {
  (void)unused;

  while(traffic_running) {
	FILE *traffic = fopen(traffic_log, "r");
	char line[256];

	if(!traffic) {
		fprintf(stderr, "WARNING: Could not open CAN traffic file: %s\n", traffic_log);
		return 1;
	}

	while(traffic_running && fgets(line, sizeof(line), traffic)) {
		char *hash = strchr(line, '#');
		char *frame_text;
		struct canfd_frame traffic_frame;
		int mtu;

		if(!hash) continue;
		frame_text = hash;
		while(frame_text > line && frame_text[-1] != ' ' && frame_text[-1] != '\t')
			frame_text--;

		mtu = parse_canframe(frame_text, &traffic_frame);
		if(mtu)
			can_bus_send(s, &traffic_frame, (size_t)mtu);
		SDL_Delay(2);
	}
	fclose(traffic);
  }

  return 0;
}

void kill_child() {
#ifndef _WIN32
	kill(play_id, SIGINT);
#endif
}

void redraw_screen() {
  SDL_RenderCopy(renderer, base_texture, NULL, NULL);
  SDL_RenderPresent(renderer);
}

// Maps the controllers buttons
void map_joy() {
	switch(gControllerType) {
	case USB_CONTROLLER:
		gButtonA = BUTTON_A;
		gButtonB = BUTTON_B;
		gButtonX = BUTTON_X;
		gButtonY = BUTTON_Y;
		gButtonStart = BUTTON_START;
		gButtonLock = BUTTON_LOCK;
		gButtonUnlock = BUTTON_UNLOCK;
		gAxisL2 = AXIS_L2;
		gAxisR2 = AXIS_R2;
		gAxisRightH = AXIS_RIGHT_H;
		gAxisRightV = AXIS_RIGHT_V;
		gAxisLeftH = AXIS_LEFT_H;
		gAxisLeftV = AXIS_LEFT_V;
		break;
	case PS3_CONTROLLER:
		gButtonA = PS3_BUTTON_A;
		gButtonB = PS3_BUTTON_B;
		gButtonX = PS3_BUTTON_X;
		gButtonY = PS3_BUTTON_Y;
		gButtonStart = PS3_BUTTON_START;
		gButtonLock = PS3_BUTTON_LOCK;
		gButtonUnlock = PS3_BUTTON_UNLOCK;
		gAxisL2 = PS3_AXIS_L2;
		gAxisR2 = PS3_AXIS_R2;
		gAxisRightH = PS3_AXIS_RIGHT_H;
		gAxisRightV = PS3_AXIS_RIGHT_V;
		gAxisLeftH = PS3_AXIS_LEFT_H;
		gAxisLeftV = PS3_AXIS_LEFT_V;
		gJoyX = PS3_X_ROT;
		gJoyY = PS3_Y_ROT;
		gJoyZ = PS3_Z_ROT;
 		break; 
	default:
	printf("Unknown controller type for mapping\n");
  	}
}

void print_joy_info() {
	printf("Name: %s\n", SDL_JoystickNameForIndex(0));
	printf("Number of Axes: %d\n", SDL_JoystickNumAxes(gJoystick));
	printf("Number of Buttons: %d\n", SDL_JoystickNumButtons(gJoystick));
	if(SDL_JoystickNumBalls(gJoystick) > 0) printf("Number of Balls: %d\n", SDL_JoystickNumBalls(gJoystick));
        if(strncmp(SDL_JoystickNameForIndex(0), "PLAYSTATION(R)3 Controller", 25) == 0) {
		// PS3 Rumble controller via BT
		gControllerType = PS3_CONTROLLER;
	}
        if(strncmp(SDL_JoystickNameForIndex(0), "Sony PLAYSTATION(R)3 Controller", 30) == 0) {
		// PS3 directly connected
		gControllerType = PS3_CONTROLLER;
	}
	map_joy();
}

void usage(char *msg) {
  if(msg) printf("%s\n", msg);
  printf("Usage: controls [options] <can>\n");
  printf("\t-s\tseed value from IC\n");
  printf("\t-l\tdifficulty level. 0-2 (default: %d)\n", DEFAULT_DIFFICULTY);
  printf("\t-t\ttraffic file to use for bg CAN traffic\n");
  printf("\t-m\tModel (Ex: -m bmw)\n");
  printf("\t-X\tDisable background CAN traffic.  Cheating if doing RE but needed if playing on a real CANbus\n");
  printf("\t--demo\tautomatic demo mode (random inputs every 2s)\n");
  printf("\t-d\tdebug mode\n");
  exit(1);
}

int main(int argc, char *argv[]) {
  int opt;
  int running = 1;
  int play_traffic = 1;
  int demo_mode = 0;
  struct stat st;
  SDL_Event event;

  while ((opt = getopt(argc, argv, "Xdl:s:t:m:h?")) != -1) {
    switch(opt) {
	case 'l':
		difficulty = atoi(optarg);
		break;
	case 's':
		seed = atoi(optarg);
		break;
	case 't':
		traffic_log = optarg;
		break;
	case 'd':
		debug = 1;
		break;
	case 'm':
		model = optarg;
		break;
	case 'X':
		play_traffic = 0;
		break;
	case 'h':
	case '?':
	default:
		usage(NULL);
		break;
    }
  }

  /* Parse long options */
  for (int i = 1; i < argc; i++) {
	if (strcmp(argv[i], "--demo") == 0)
		demo_mode = 1;
  }

  if (optind >= argc) usage("You must specify at least one can device");

  if(stat(traffic_log, &st) == -1) {
	char msg[256];
	snprintf(msg, 255, "CAN Traffic file not found: %s\n", traffic_log);
	usage(msg);
  }

  snprintf(can_name, sizeof(can_name), "%s", argv[optind]);
  if (can_bus_open(&s, can_name) < 0) {
       fprintf(stderr, "%s\n", can_bus_error());
       return 1;
  }

  /* Load vehicle configuration from TOML file */
  if (model) {
	if (!strncmp(model, "bmw", 3)) {
		icsim_config_load(&g_cfg, "models/bmw_x1.toml");
	} else if (strchr(model, '.')) {
		icsim_config_load(&g_cfg, model);
	} else {
		char path[256];
		snprintf(path, sizeof(path), "models/%s.toml", model);
		if (icsim_config_load(&g_cfg, path) != 0)
			icsim_config_defaults(&g_cfg);
	}
  } else {
	icsim_config_defaults(&g_cfg);
  }
  printf("Vehicle: %s (%s)\n", g_cfg.name, g_cfg.description);

  door_id   = g_cfg.can.door_id;
  signal_id = g_cfg.can.signal_id;
  speed_id  = g_cfg.can.speed_id;
  rpm_id    = g_cfg.rpm.rpm_id;
  temp_id   = g_cfg.temp.temp_id;
  fuel_id   = g_cfg.fuel.fuel_id;
  door_len  = g_cfg.can.door_pos + 1;
  signal_len = g_cfg.can.signal_pos + 1;
  speed_len  = g_cfg.can.speed_pos + 2;

  if (seed) {
	srand(seed);
        door_id = (rand() % 2046) + 1;
        signal_id = (rand() % 2046) + 1;
        speed_id = (rand() % 2046) + 1;
        g_cfg.can.door_pos = rand() % 9;
        g_cfg.can.signal_pos = rand() % 9;
        g_cfg.can.speed_pos = rand() % 8;
        printf("Seed: %d\n", seed);
	door_len = g_cfg.can.door_pos + 1;
	signal_len = g_cfg.can.signal_pos + 1;
	speed_len = g_cfg.can.speed_pos + 2;
  }

  if(difficulty > 0) {
	if (door_len < 8) {
		door_len += rand() % (8 - door_len);
	} else {
		door_len = 0;
	}
	if (signal_len < 8) {
		signal_len += rand() % (8 - signal_len);
	} else {
		signal_len = 0;
	}
	if (speed_len < 8) {
		speed_len += rand() % (8 - speed_len);
	} else {
		speed_len = 0;
	}
  }

  if(play_traffic) {
#ifndef _WIN32
	if(can_bus_is_virtual(s)) {
		SDL_CreateThread(play_can_traffic_thread, "can-traffic", NULL);
	} else {
		play_id = fork();
		if((int)play_id == -1) {
			printf("Error: Couldn't fork bg player\n");
			exit(-1);
		} else if (play_id == 0) {
			play_can_traffic();
			// Shouldn't return
			exit(0);
		}
		atexit(kill_child);
	}
#else
	SDL_CreateThread(play_can_traffic_thread, "can-traffic", NULL);
#endif
  }

  // GUI Setup
  SDL_Window *window = NULL;
  if(SDL_Init ( SDL_INIT_VIDEO | SDL_INIT_JOYSTICK ) < 0 ) {
        printf("SDL Could not initializes\n");
        exit(40);
  }
  if( SDL_NumJoysticks() < 1) {
	printf(" Warning: No joysticks connected\n");
  } else {
	if(SDL_IsGameController(0)) {
	  gGameController = SDL_GameControllerOpen(0);
	  if(gGameController == NULL) {
		printf(" Warning: Unable to open game controller. %s\n", SDL_GetError() );
	  } else {
		gJoystick = SDL_GameControllerGetJoystick(gGameController);
		gHaptic = SDL_HapticOpenFromJoystick(gJoystick);
		print_joy_info();
	  }
        } else {
		gJoystick = SDL_JoystickOpen(0);
		if(gJoystick == NULL) {
			printf(" Warning: Could not open joystick\n");
		} else {
			gHaptic = SDL_HapticOpenFromJoystick(gJoystick);
			if (gHaptic == NULL) printf("No Haptic support\n");
			print_joy_info();
		}
	}
  }
  window = SDL_CreateWindow("CANBus Control Panel", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if(window == NULL) {
        printf("Window could not be shown\n");
  }
  renderer = SDL_CreateRenderer(window, -1, 0);
  SDL_Surface *image = IMG_Load(get_data("joypad.png"));
  base_texture = SDL_CreateTextureFromSurface(renderer, image);
  SDL_RenderCopy(renderer, base_texture, NULL, NULL);
  SDL_RenderPresent(renderer);
  int button, axis; // Used for checking dynamic joystick mappings

  while(running) {
    while( SDL_PollEvent(&event) != 0 ) {
        switch(event.type) {
            case SDL_QUIT:
                running = 0;
                break;
	    case SDL_WINDOWEVENT:
		switch(event.window.event) {
		case SDL_WINDOWEVENT_ENTER:
		case SDL_WINDOWEVENT_RESIZED:
			redraw_screen();
			break;
		}
                break;
	    case SDL_KEYDOWN:
		switch(event.key.keysym.sym) {
		    case SDLK_UP:
			throttle = 1;
			break;
		    case SDLK_LEFT:
			turning = -1;
			break;
		    case SDLK_RIGHT:
			turning = 1;
			break;
		    case SDLK_LSHIFT:
			lock_enabled = 1;
			if(!event.key.repeat) send_lock(ICSIM_DOOR1 | ICSIM_DOOR2 | ICSIM_DOOR3 | ICSIM_DOOR4);
			break;
		    case SDLK_RSHIFT:
			unlock_enabled = 1;
			if(!event.key.repeat) send_unlock(ICSIM_DOOR1 | ICSIM_DOOR2 | ICSIM_DOOR3 | ICSIM_DOOR4);
			break;
		    case SDLK_a:
			if(lock_enabled) {
				send_lock(ICSIM_DOOR1);
			} else if(unlock_enabled) {
				send_unlock(ICSIM_DOOR1);
			} else if(!event.key.repeat) {
				toggle_door(ICSIM_DOOR1);
			}
			break;
		    case SDLK_b:
			if(lock_enabled) {
				send_lock(ICSIM_DOOR2);
			} else if(unlock_enabled) {
				send_unlock(ICSIM_DOOR2);
			} else if(!event.key.repeat) {
				toggle_door(ICSIM_DOOR2);
			}
			break;
		    case SDLK_x:
			if(lock_enabled) {
				send_lock(ICSIM_DOOR3);
			} else if(unlock_enabled) {
				send_unlock(ICSIM_DOOR3);
			} else if(!event.key.repeat) {
				toggle_door(ICSIM_DOOR3);
			}
			break;
		    case SDLK_y:
			if(lock_enabled) {
				send_lock(ICSIM_DOOR4);
			} else if(unlock_enabled) {
				send_unlock(ICSIM_DOOR4);
			} else if(!event.key.repeat) {
				toggle_door(ICSIM_DOOR4);
			}
			break;
		    case SDLK_1:
			engine_rpm += 500;
			if (engine_rpm > 7000) engine_rpm = 7000;
			send_rpm();
			break;
		    case SDLK_2:
			engine_rpm -= 500;
			if (engine_rpm < 0) engine_rpm = 0;
			send_rpm();
			break;
		    case SDLK_3:
			coolant_temp += 5;
			if (coolant_temp > 120) coolant_temp = 120;
			send_temp();
			break;
		    case SDLK_4:
			coolant_temp -= 5;
			if (coolant_temp < 60) coolant_temp = 60;
			send_temp();
			break;
		    case SDLK_5:
			fuel_level += 5;
			if (fuel_level > 100) fuel_level = 100;
			send_fuel();
			break;
		    case SDLK_6:
			fuel_level -= 5;
			if (fuel_level < 0) fuel_level = 0;
			send_fuel();
			break;
		}
	   	break;
	    case SDL_KEYUP:
		switch(event.key.keysym.sym) {
		    case SDLK_UP:
			throttle = -1;
			break;
		    case SDLK_LEFT:
		    case SDLK_RIGHT:
			turning = 0;
			break;
		    case SDLK_LSHIFT:
			lock_enabled = 0;
			break;
		    case SDLK_RSHIFT:
			unlock_enabled = 0;
			break;
		}
		break;
	    case SDL_JOYAXISMOTION:
		axis = event.jaxis.axis;
		if(axis == gAxisLeftH) {
			ud(event.jaxis.value);
		} else if(axis == gAxisLeftV) {
			turn(event.jaxis.value);
		} else if(axis == gAxisR2) {
			accelerate(event.jaxis.value);
		} else if(axis == gAxisRightH ||
			  axis == gAxisRightV ||
			  axis == gAxisL2 ||
			  axis == gJoyX ||
			  axis == gJoyY ||
			  axis == gJoyZ) {
			// Do nothing, the axis is known just not connected
		} else {
			if (debug) printf("Unkown axis: %d\n", event.jaxis.axis);
		}
		break;
	    case SDL_JOYBUTTONDOWN:
                button = event.jbutton.button;
		if(button == gButtonLock) {
			lock_enabled = 1;
			if(unlock_enabled) send_lock(ICSIM_DOOR1 | ICSIM_DOOR2 | ICSIM_DOOR3 | ICSIM_DOOR4);
		} else if(button == gButtonUnlock) {
			unlock_enabled = 1;
			if(lock_enabled) send_unlock(ICSIM_DOOR1 | ICSIM_DOOR2 | ICSIM_DOOR3 | ICSIM_DOOR4);
		} else if(button == gButtonA) {
			if(lock_enabled) {
				send_lock(ICSIM_DOOR1);
			} else if(unlock_enabled) {
				send_unlock(ICSIM_DOOR1);
			}
		} else if (button == gButtonB) {
			if(lock_enabled) {
				send_lock(ICSIM_DOOR2);
			} else if(unlock_enabled) {
				send_unlock(ICSIM_DOOR2);
			}
		} else if (button == gButtonX) {
			if(lock_enabled) {
				send_lock(ICSIM_DOOR3);
			} else if(unlock_enabled) {
				send_unlock(ICSIM_DOOR3);
			}
		} else if (button == gButtonY) {
			if(lock_enabled) {
				send_lock(ICSIM_DOOR4);
			} else if(unlock_enabled) {
				send_unlock(ICSIM_DOOR4);
			}
		} else if (button == gButtonStart) {
		} else {
			if(debug) printf("Unassigned button: %d\n", event.jbutton.button);
		}
		break;
	    case SDL_JOYBUTTONUP:
		button = event.jbutton.button;
		if(button == gButtonLock) {
			lock_enabled = 0;
		} else if(button == gButtonUnlock) {
			unlock_enabled = 0;
		} else {
			//if(debug) printf("Unassigned button: %d\n", event.jbutton.button);
		}
		break;
	    case SDL_JOYDEVICEADDED:
		// Only use the first controller
		if(event.cdevice.which == 0) {
			gJoystick = SDL_JoystickOpen(0);
			if(gJoystick) {
				gHaptic = SDL_HapticOpenFromJoystick(gJoystick);
				print_joy_info();
			}
		}
		break;
	    case SDL_JOYDEVICEREMOVED:
		if(event.cdevice.which == 0) {
			SDL_JoystickClose(gJoystick);
			gJoystick = NULL;
		}
		break;
	    case SDL_CONTROLLERDEVICEADDED:
		// Only use the first controller
		if(gGameController == NULL) {
			gGameController = SDL_GameControllerOpen(0);
			gJoystick = SDL_GameControllerGetJoystick(gGameController);
			gHaptic = SDL_HapticOpenFromJoystick(gJoystick);
			print_joy_info();
		}
		break;
	    case SDL_CONTROLLERDEVICEREMOVED:
		if(event.cdevice.which == 0) {
			SDL_GameControllerClose(gGameController);
			gGameController = NULL;
		}
		break;
        }
    }
    currentTime = SDL_GetTicks();
    checkAccel();
    checkTurn();

    /* Demo mode: random inputs every 2 seconds */
    if (demo_mode) {
	static Uint32 last_demo = 0;
	if (currentTime - last_demo > 2000) {
		last_demo = currentTime;
		int action = rand() % 7;
		switch (action) {
		case 0: throttle = (rand() % 2) ? 1 : -1; break;
		case 1: throttle = 0; break;
		case 2: turning = (rand() % 3) - 1; break;
		case 3: engine_rpm = 800 + (rand() % 6000);
			if (engine_rpm > 7000) engine_rpm = 7000;
			send_rpm(); break;
		case 4: coolant_temp = 60 + (rand() % 50);
			if (coolant_temp > 120) coolant_temp = 120;
			send_temp(); break;
		case 5: toggle_door(ICSIM_DOOR1 << (rand() % 4)); break;
		case 6: signal_state ^= (rand() % 2) ? ICSIM_TURN_LEFT : ICSIM_TURN_RIGHT;
			send_turn_signal(); break;
		}
	}
    }

    SDL_Delay(5);
  }

  traffic_running = 0;
  SDL_Delay(10);
  can_bus_close(s);
  SDL_DestroyTexture(base_texture);
  SDL_FreeSurface(image);
  SDL_GameControllerClose(gGameController);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
