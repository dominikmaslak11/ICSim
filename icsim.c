/*
 * Instrument cluster simulator
 *
 * (c) 2014 Open Garages - Craig Smith <craig@theialabs.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "getopt_compat.h"
#include "can_platform.h"
#include "can_log.h"
#include "config.h"
#include "lib.h"

#ifndef DATA_DIR
#define DATA_DIR "./data/"  // Needs trailing slash
#endif

#define SCREEN_WIDTH 692
#define SCREEN_HEIGHT 329
#define DOOR_LOCKED 0
#define DOOR_UNLOCKED 1
#define OFF 0
#define ON 1

/* CAN IDs, byte positions, and signal definitions are now loaded from
 * TOML config files via config.h / config.c.  Hardcoded BMW model
 * constants have been moved to models/bmw_x1.toml. */
static icsim_config_t g_cfg;

int debug = 0;
int randomize = 0;
int seed = 0;
long current_speed = 0;
int engine_rpm = 800;
int coolant_temp = 80;
int fuel_level = 75;
int door_status[4];
int turn_status[2];
char *model = NULL;   /* model name for BMW-specific speed formula */
char *record_path = NULL;
char *replay_path = NULL;
can_log_t *can_recorder = NULL;
can_log_t *can_replayer = NULL;
char data_file[256];
SDL_Renderer *renderer = NULL;
SDL_Texture *base_texture = NULL;
SDL_Texture *needle_tex = NULL;
SDL_Texture *sprite_tex = NULL;
SDL_Rect speed_rect;
int screen_dirty = 0;

// Simple map function
long map(long x, long in_min, long in_max, long out_min, long out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// Adds data dir to file name
// Uses a single pointer so not to have a memory leak
// returns point to data_files or NULL if append is too large
char *get_data(char *fname) {
  if(strlen(DATA_DIR) + strlen(fname) > 255) return NULL;
  strncpy(data_file, DATA_DIR, 255);
  strncat(data_file, fname, 255-strlen(data_file));
  return data_file;
}

/* Default vehicle state */
void init_car_state() {
  door_status[0] = DOOR_LOCKED;
  door_status[1] = DOOR_LOCKED;
  door_status[2] = DOOR_LOCKED;
  door_status[3] = DOOR_LOCKED;
  turn_status[0] = OFF;
  turn_status[1] = OFF;
}

/* Empty IC */
void blank_ic() {
  SDL_RenderCopy(renderer, base_texture, NULL, NULL);
}

/* Updates speedo */
void update_speed() {
  SDL_Rect dial_rect;
  SDL_Point center;
  double angle = 0;
  dial_rect.x = 200;
  dial_rect.y = 80;
  dial_rect.h = 130;
  dial_rect.w = 300;
  SDL_RenderCopy(renderer, base_texture, &dial_rect, &dial_rect);
  /* Because it's a curve we do a smaller rect for the top */
  dial_rect.x = 250;
  dial_rect.y = 30;
  dial_rect.h = 65;
  dial_rect.w = 200;
  SDL_RenderCopy(renderer, base_texture, &dial_rect, &dial_rect);
  // And one more smaller box for the pivot point of the needle
  dial_rect.x = 323;
  dial_rect.y = 171;
  dial_rect.h = 52;
  dial_rect.w = 47;
  SDL_RenderCopy(renderer, base_texture, &dial_rect, &dial_rect);
  center.x = 135;
  center.y = 20;
  angle = map(current_speed, 0, 280, 0, 180);
  if(angle < 0) angle = 0;
  if(angle > 180) angle = 180;
  SDL_RenderCopyEx(renderer, needle_tex, NULL, &speed_rect, angle, &center, SDL_FLIP_NONE);
}

/* Updates door unlocks simulated by door open icons */
void update_doors() {
  SDL_Rect door_area, update, pos;
  door_area.x = 390;
  door_area.y = 215;
  door_area.w = 110;
  door_area.h = 85;
  SDL_RenderCopy(renderer, base_texture, &door_area, &door_area);
  // No update if all doors are locked
  if(door_status[0] == DOOR_LOCKED && door_status[1] == DOOR_LOCKED &&
     door_status[2] == DOOR_LOCKED && door_status[3] == DOOR_LOCKED) return;
  // Make the base body red if even one door is unlocked
  update.x = 440;
  update.y = 239;
  update.w = 45;
  update.h = 83;
  memcpy(&pos, &update, sizeof(SDL_Rect));
  pos.x -= 22;
  pos.y -= 22;
  SDL_RenderCopy(renderer, sprite_tex, &update, &pos);
  if(door_status[0] == DOOR_UNLOCKED) {
    update.x = 420;
    update.y = 263;
    update.w = 21;
    update.h = 22;
    memcpy(&pos, &update, sizeof(SDL_Rect));
    pos.x -= 22;
    pos.y -= 22;
    SDL_RenderCopy(renderer, sprite_tex, &update, &pos);
  }
  if(door_status[1] == DOOR_UNLOCKED) {
    update.x = 484;
    update.y = 261;
    update.w = 21;
    update.h = 22;
    memcpy(&pos, &update, sizeof(SDL_Rect));
    pos.x -= 22;
    pos.y -= 22;
    SDL_RenderCopy(renderer, sprite_tex, &update, &pos);
  }
  if(door_status[2] == DOOR_UNLOCKED) {
    update.x = 420;
    update.y = 284;
    update.w = 21;
    update.h = 22;
    memcpy(&pos, &update, sizeof(SDL_Rect));
    pos.x -= 22;
    pos.y -= 22;
    SDL_RenderCopy(renderer, sprite_tex, &update, &pos);
  }
  if(door_status[3] == DOOR_UNLOCKED) {
    update.x = 484;
    update.y = 287;
    update.w = 21;
    update.h = 22;
    memcpy(&pos, &update, sizeof(SDL_Rect));
    pos.x -= 22;
    pos.y -= 22;
    SDL_RenderCopy(renderer, sprite_tex, &update, &pos);
  }
}

/* Updates turn signals */
void update_turn_signals() {
  SDL_Rect left, right, lpos, rpos;
  left.x = 213;
  left.y = 51;
  left.w = 45;
  left.h = 45;
  memcpy(&right, &left, sizeof(SDL_Rect));
  right.x = 482;
  memcpy(&lpos, &left, sizeof(SDL_Rect));
  memcpy(&rpos, &right, sizeof(SDL_Rect));
  lpos.x -= 22;
  lpos.y -= 22;
  rpos.x -= 22;
  rpos.y -= 22;
  if(turn_status[0] == OFF) {
	SDL_RenderCopy(renderer, base_texture, &lpos, &lpos);
  } else {
	SDL_RenderCopy(renderer, sprite_tex, &left, &lpos);
  }
  if(turn_status[1] == OFF) {
	SDL_RenderCopy(renderer, base_texture, &rpos, &rpos);
  } else {
	SDL_RenderCopy(renderer, sprite_tex, &right, &rpos);
  }
}

/* Updates engine RPM bar */
void update_rpm_bar() {
  SDL_Rect bg = {150, 250, 120, 12};
  SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
  SDL_RenderFillRect(renderer, &bg);

  int rpm_clamped = engine_rpm;
  if (rpm_clamped < 0) rpm_clamped = 0;
  if (rpm_clamped > 7000) rpm_clamped = 7000;
  int w = (rpm_clamped * 120) / 7000;
  SDL_Rect bar = {150, 250, w, 12};
  SDL_SetRenderDrawColor(renderer, 0, 200, 0, 255);
  SDL_RenderFillRect(renderer, &bar);
}

/* Updates coolant temp bar */
void update_temp_bar() {
  SDL_Rect bg = {150, 268, 120, 12};
  SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
  SDL_RenderFillRect(renderer, &bg);

  int t = coolant_temp;
  if (t < 60) t = 60;
  if (t > 120) t = 120;
  int w = ((t - 60) * 120) / 60;
  SDL_Rect bar = {150, 268, w, 12};
  SDL_SetRenderDrawColor(renderer, t > 105 ? 200 : 0,
                         t > 105 ? 50 : 150, 0, 255);
  SDL_RenderFillRect(renderer, &bar);
}

/* Updates fuel level bar */
void update_fuel_bar() {
  SDL_Rect bg = {150, 286, 120, 12};
  SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
  SDL_RenderFillRect(renderer, &bg);

  int f = fuel_level;
  if (f < 0) f = 0;
  if (f > 100) f = 100;
  int w = (f * 120) / 100;
  SDL_Rect bar = {150, 286, w, 12};
  SDL_SetRenderDrawColor(renderer, f < 15 ? 200 : 0,
                         f < 15 ? 100 : 150, 0, 255);
  SDL_RenderFillRect(renderer, &bar);
}

/* Redraws the IC updating everything 
 * Slowest way to go.  Should only use on init
 */
void redraw_ic() {
  blank_ic();
  update_speed();
  update_doors();
  update_turn_signals();
  update_rpm_bar();
  update_temp_bar();
  update_fuel_bar();
  SDL_RenderPresent(renderer);
  screen_dirty = 0;
}

/* Parses CAN fram and updates current_speed */
void update_speed_status(struct canfd_frame *cf, int maxdlen) {
  int len = (cf->len > maxdlen) ? maxdlen : cf->len;
  if(len <= g_cfg.can.speed_pos + 1) return;
  if (model) {
	if (!strncmp(model, "bmw", 3)) {
		current_speed = (((cf->data[g_cfg.can.speed_pos + 1] - 208) * 256) + cf->data[g_cfg.can.speed_pos]) / 16;
	}
  } else {
	  int speed = cf->data[g_cfg.can.speed_pos] << 8;
	  speed += cf->data[g_cfg.can.speed_pos + 1];
	  speed = speed / g_cfg.speed.divisor;
	  current_speed = speed * g_cfg.speed.scaling;
  }
  update_speed();
  screen_dirty = 1;
}

/* Parses CAN frame and updates RPM */
void update_rpm_status(struct canfd_frame *cf, int maxdlen) {
  int len = (cf->len > maxdlen) ? maxdlen : cf->len;
  int pos = g_cfg.rpm.rpm_pos;
  if (len <= pos + 1) return;
  int raw = (cf->data[pos] << 8) | cf->data[pos + 1];
  engine_rpm = (int)((double)raw * g_cfg.rpm.scaling / (double)g_cfg.rpm.divisor);
  update_rpm_bar();
  screen_dirty = 1;
}

/* Parses CAN frame and updates coolant temp */
void update_temp_status(struct canfd_frame *cf, int maxdlen) {
  int len = (cf->len > maxdlen) ? maxdlen : cf->len;
  int pos = g_cfg.temp.temp_pos;
  if (len <= pos) return;
  coolant_temp = (int)((double)cf->data[pos] * g_cfg.temp.scaling / (double)g_cfg.temp.divisor);
  update_temp_bar();
  screen_dirty = 1;
}

/* Parses CAN frame and updates fuel level */
void update_fuel_status(struct canfd_frame *cf, int maxdlen) {
  int len = (cf->len > maxdlen) ? maxdlen : cf->len;
  int pos = g_cfg.fuel.fuel_pos;
  if (len <= pos) return;
  fuel_level = (int)((double)cf->data[pos] * g_cfg.fuel.scaling / (double)g_cfg.fuel.divisor);
  update_fuel_bar();
  screen_dirty = 1;
}

/* Parses CAN frame and updates turn signal status */
void update_signal_status(struct canfd_frame *cf, int maxdlen) {
  int len = (cf->len > maxdlen) ? maxdlen : cf->len;
  if(len <= g_cfg.can.signal_pos) return;
  if(cf->data[g_cfg.can.signal_pos] & ICSIM_TURN_LEFT) {
    turn_status[0] = ON;
  } else {
    turn_status[0] = OFF;
  }
  if(cf->data[g_cfg.can.signal_pos] & ICSIM_TURN_RIGHT) {
    turn_status[1] = ON;
  } else {
    turn_status[1] = OFF;
  }
  update_turn_signals();
  screen_dirty = 1;
}

/* Parses CAN frame and updates door status */
void update_door_status(struct canfd_frame *cf, int maxdlen) {
  int len = (cf->len > maxdlen) ? maxdlen : cf->len;
  if(len <= g_cfg.can.door_pos) return;
  if(cf->data[g_cfg.can.door_pos] & ICSIM_DOOR1) {
	door_status[0] = DOOR_LOCKED;
  } else {
	door_status[0] = DOOR_UNLOCKED;
  }
  if(cf->data[g_cfg.can.door_pos] & ICSIM_DOOR2) {
	door_status[1] = DOOR_LOCKED;
  } else {
	door_status[1] = DOOR_UNLOCKED;
  }
  if(cf->data[g_cfg.can.door_pos] & ICSIM_DOOR3) {
	door_status[2] = DOOR_LOCKED;
  } else {
	door_status[2] = DOOR_UNLOCKED;
  }
  if(cf->data[g_cfg.can.door_pos] & ICSIM_DOOR4) {
	door_status[3] = DOOR_LOCKED;
  } else {
	door_status[3] = DOOR_UNLOCKED;
  }
  update_doors();
  SDL_RenderPresent(renderer);
  screen_dirty = 0;
}

void Usage(char *msg) {
  if(msg) printf("%s\n", msg);
  printf("Usage: icsim [options] <can>\n");
  printf("\t-r\trandomize IDs\n");
  printf("\t-s\tseed value\n");
  printf("\t-d\tdebug mode\n");
  printf("\t-m\tmodel FILE or name  (Ex: -m bmw, -m models/default.toml)\n");
  printf("\t-R FILE\trecord CAN frames to ASC file\n");
  printf("\t-P FILE\treplay CAN frames from ASC file\n");
  exit(1);
}

int main(int argc, char *argv[]) {
  int opt;
  can_bus_t *can;
  struct canfd_frame frame;
  struct stat dirstat;
  int running = 1;
  int maxdlen;
  size_t mtu;
  int seed = 0;
  canid_t door_id, signal_id, speed_id;
  canid_t rpm_id, temp_id, fuel_id;
  SDL_Event event;

  while ((opt = getopt(argc, argv, "rs:dm:h?R:P:")) != -1) {
    switch(opt) {
	case 'r':
		randomize = 1;
		break;
	case 's':
		seed = atoi(optarg);
		break;
	case 'd':
		debug = 1;
		break;
	case 'm':
		model = optarg;
		break;
	case 'R':
		record_path = optarg;
		break;
	case 'P':
		replay_path = optarg;
		break;
	case 'h':
	case '?':
	default:
		Usage(NULL);
		break;
    }
  }

  if (optind >= argc) Usage("You must specify at least one can device");

  if (seed && randomize) Usage("You can not specify a seed value AND randomize the seed");

  // Verify data directory exists
  if(stat(DATA_DIR, &dirstat) == -1) {
  	printf("ERROR: DATA_DIR not found.  Define in make file or run in src dir\n");
	exit(34);
  }
  
  printf("Using CAN interface %s\n", argv[optind]);
  if (can_bus_open(&can, argv[optind]) < 0) {
    fprintf(stderr, "%s\n", can_bus_error());
    exit(1);
  }
  if (can_bus_set_nonblocking(can, 1) < 0) {
    fprintf(stderr, "%s\n", can_bus_error());
    exit(1);
  }

  /* Recording / replay */
  if (record_path) {
	can_recorder = can_log_open_record(record_path);
	if (!can_recorder)
		fprintf(stderr, "WARNING: recording disabled\n");
	else
		printf("Recording CAN frames to: %s\n", record_path);
  }
  if (replay_path) {
	can_replayer = can_log_open_replay(replay_path);
	if (!can_replayer)
		fprintf(stderr, "WARNING: replay disabled\n");
	else
		printf("Replaying CAN frames from: %s\n", replay_path);
  }

  init_car_state();

  /* Load vehicle configuration from TOML file if -m was given */
  if (model) {
	if (!strncmp(model, "bmw", 3)) {
		/* BMW X1: try TOML first, model string still triggers formula */
		icsim_config_load(&g_cfg, "models/bmw_x1.toml");
	} else if (strchr(model, '.')) {
		/* Looks like a file path (e.g. models/default.toml) */
		icsim_config_load(&g_cfg, model);
	} else {
		/* Short name: try models/<name>.toml */
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

  if (randomize || seed) {
	if(randomize) seed = time(NULL);
	srand(seed);
	door_id = (rand() % 2046) + 1;
	signal_id = (rand() % 2046) + 1;
	speed_id = (rand() % 2046) + 1;
	g_cfg.can.door_pos = rand() % 9;
	g_cfg.can.signal_pos = rand() % 9;
	g_cfg.can.speed_pos = rand() % 8;
	printf("Seed: %d\n", seed);
	const char *seed_path =
#ifdef _WIN32
		"icsim_seed.txt";
#else
		"/tmp/icsim_seed.txt";
#endif
	FILE *fdseed = fopen(seed_path, "w");
	if (fdseed) {
		fprintf(fdseed, "%d\n", seed);
		fclose(fdseed);
	}
  }

  SDL_Window *window = NULL;
  if(SDL_Init ( SDL_INIT_VIDEO ) < 0 ) {
	printf("SDL Could not initializes\n");
	exit(40);
  }
  window = SDL_CreateWindow("IC Simulator", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT,
                            SDL_WINDOW_SHOWN); // | SDL_WINDOW_RESIZABLE);
  if(window == NULL) {
	printf("Window could not be shown\n");
  }
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  SDL_Surface *image = IMG_Load(get_data("ic.png"));
  SDL_Surface *needle = IMG_Load(get_data("needle.png"));
  SDL_Surface *sprites = IMG_Load(get_data("spritesheet.png"));
  base_texture = SDL_CreateTextureFromSurface(renderer, image);
  needle_tex = SDL_CreateTextureFromSurface(renderer, needle);
  sprite_tex = SDL_CreateTextureFromSurface(renderer, sprites);

  speed_rect.x = 212;
  speed_rect.y = 175;
  speed_rect.h = needle->h;
  speed_rect.w = needle->w;

  // Draw the IC
  redraw_ic();

  Uint32 last_present = SDL_GetTicks();
  Uint32 replay_base_tick = SDL_GetTicks();

  /* For now we will just operate on one CAN interface */
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
			redraw_ic();
		break;
	    }
   	}
    }

    int processed = 0;
    while (processed < 512) {
      if (can_bus_recv(can, &frame, &mtu) < 0) {
        if (can_bus_error_is_would_block())
          break;
        fprintf(stderr, "%s\n", can_bus_error());
        return 1;
      }
      if (mtu == CAN_MTU)
        maxdlen = CAN_MAX_DLEN;
      else if (mtu == CANFD_MTU)
        maxdlen = CANFD_MAX_DLEN;
      else {
        fprintf(stderr, "read: incomplete CAN frame\n");
        return 1;
      }
//      if(debug) fprint_canframe(stdout, &frame, "\n", 0, maxdlen);
      if(frame.can_id == door_id) update_door_status(&frame, maxdlen);
      if(frame.can_id == signal_id) update_signal_status(&frame, maxdlen);
      if(frame.can_id == speed_id) update_speed_status(&frame, maxdlen);
      if(frame.can_id == rpm_id) update_rpm_status(&frame, maxdlen);
      if(frame.can_id == temp_id) update_temp_status(&frame, maxdlen);
      if(frame.can_id == fuel_id) update_fuel_status(&frame, maxdlen);
      if (can_recorder) can_log_record(can_recorder, &frame);
      processed++;
    }

    /* Replay: inject frames whose timestamp has elapsed */
    if (can_replayer) {
      double t_offset;
      size_t replay_mtu;
      struct canfd_frame replay_frame;
      while (can_log_replay_next(can_replayer, &replay_frame,
                                 &t_offset, &replay_mtu)) {
        Uint32 elapsed = SDL_GetTicks() - replay_base_tick;
        if ((double)elapsed / 1000.0 < t_offset)
          break; /* not yet time for this frame */
        if (can_bus_send(can, &replay_frame, replay_mtu) < 0)
          fprintf(stderr, "replay send: %s\n", can_bus_error());
        if (can_recorder) can_log_record(can_recorder, &replay_frame);
      }
    }

    if (screen_dirty && SDL_GetTicks() - last_present >= 16) {
      SDL_RenderPresent(renderer);
      screen_dirty = 0;
      last_present = SDL_GetTicks();
    }

    SDL_Delay(processed ? 0 : 1);
  }

  SDL_DestroyTexture(base_texture);
  SDL_DestroyTexture(needle_tex);
  SDL_DestroyTexture(sprite_tex);
  SDL_FreeSurface(image);
  SDL_FreeSurface(needle);
  SDL_FreeSurface(sprites);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  IMG_Quit();
  SDL_Quit();
  can_bus_close(can);

  if (can_recorder) can_log_close(can_recorder);
  if (can_replayer) can_log_close(can_replayer);

  return 0;
}
