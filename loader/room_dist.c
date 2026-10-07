/* room_dist.c -- optional: rooms beyond a distance from the camera are not
 * drawn.
 *
 * The engine renders every room that passes its VIS list and the camera planes
 * (CollectActiveRooms), however far it is. With
 *     ux0:data/kotor2/room_distance.txt   containing a distance in metres
 * a room whose box is farther than that from the camera is dropped from the
 * list: it adds no meshes and no objects to the frame. The current room and
 * the rooms the engine forces visible are always kept. A dropped room comes
 * back once it is 2 m inside the distance, so a room on the edge does not
 * flicker with the camera. Without the file (or with 0) nothing is hooked.
 *
 * Distant geometry disappears, hence opt-in. Hardware, Peragus, same views
 * (PERFORMANCE.md 6.7): with 17 rooms beyond 35 m, 66.7 -> 51.0 ms per frame
 * (489 -> 318 draws); with 4, 42.5 -> 34.9 ms; nothing changes where no room
 * is that far.
 *
 * Test builds (ROOM_AB): the distance is ROOM_AB_RADIUS_M in alternate windows
 * of ROOM_AB_WINDOW_S seconds, all rooms in the others, and one "[roomab]"
 * line per window: frame time, draws, active rooms, rooms beyond the distance
 * and their names. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "room_dist.h"

#define HYSTERESIS_M 2.0f
#define MAX_DROPPED 128

/* CAurRoom: +0x1c box min, +0x28 box max, +0x78 name (64 bytes), +0xb8 forced
 * visible. Scene+0xe0: the current room. List<T>: data pointer, count. Camera
 * position at +0xa8 (main.c's room filters). */
static void (*o_collect)(void *scene, void *list);
static void **s_camera;
static float s_radius;                      /* metres; 0: keep every room */
static const void *s_dropped[MAX_DROPPED];  /* rooms dropped last time, for the hysteresis */
static unsigned s_ndropped;

static int was_dropped(const void *room) {
  for (unsigned i = 0; i < s_ndropped; i++)
    if (s_dropped[i] == room) return 1;
  return 0;
}

#if ROOM_AB
#define MAX_FRAMES 1024
static uint64_t s_t0, s_prev_swap, s_prev_draws;
static int s_window = -1;
static unsigned s_frames, s_scene_frames, s_n;
static uint32_t s_frame_us[MAX_FRAMES];
static uint64_t s_sum_us, s_draws, s_rooms, s_far, s_dropcount;
static unsigned s_frame_rooms, s_frame_far, s_frame_dropped, s_frame_scene;
static char s_far_names[384];
static int s_far_len;

static void note_far(const char *name, float d) {
  /* Each far room once per window, with the first distance seen. */
  char tag[32];
  int n = snprintf(tag, sizeof tag, "%.24s ", name);
  if (n <= 0 || strstr(s_far_names, tag)) return;
  if (s_far_len + n + 8 >= (int)sizeof s_far_names) return;
  s_far_len += snprintf(s_far_names + s_far_len, sizeof s_far_names - s_far_len, "%.24s %.0fm, ", name, d);
}
#endif

static void h_collect(void *scene, void *list) {
  o_collect(scene, list);
  uintptr_t current = 0, data = 0;
  int count = 0;
  memcpy(&current, (char *)scene + 0xe0, sizeof current);
  memcpy(&data, list, sizeof data);
  memcpy(&count, (char *)list + 4, sizeof count);
  void *camera = s_camera ? *s_camera : NULL;
  if (!current || !camera || count <= 0 || count > 1024 || !data) return;
  float pos[3];
  memcpy(pos, (char *)camera + 0xa8, sizeof pos);
  for (int a = 0; a < 3; a++)
    if (!(pos[a] > -1e6f && pos[a] < 1e6f)) return;

  float radius = s_radius;
#if ROOM_AB
  radius = (s_window >= 0 && (s_window & 1)) ? (float)ROOM_AB_RADIUS_M : 0.0f;
  s_frame_scene = 1;
  s_frame_rooms += (unsigned)count;
#endif
  const void *dropped[MAX_DROPPED];
  unsigned ndropped = 0;
  void **items = (void **)data;
  int w = 0;
  for (int i = 0; i < count; i++) {
    char *room = (char *)items[i];
    int keep = 1;
    if (room && room != (char *)current && !room[0xb8]) {
      float lo[3], hi[3], d2 = 0.0f;
      int valid = 1;
      memcpy(lo, room + 0x1c, sizeof lo);
      memcpy(hi, room + 0x28, sizeof hi);
      for (int a = 0; a < 3; a++) {
        if (!(lo[a] > -1e6f && hi[a] < 1e6f && lo[a] <= hi[a])) { valid = 0; break; }
        float d = pos[a] < lo[a] ? lo[a] - pos[a] : pos[a] > hi[a] ? pos[a] - hi[a] : 0.0f;
        d2 += d * d;
      }
#if ROOM_AB
      if (valid && d2 > (float)ROOM_AB_RADIUS_M * (float)ROOM_AB_RADIUS_M) {
        s_frame_far++;
        note_far(room + 0x78, __builtin_sqrtf(d2));
      }
#endif
      if (valid && radius > 0.0f) {
        float limit = was_dropped(room) ? radius - HYSTERESIS_M : radius;
        if (d2 > limit * limit) {
          static int s_logged;
          keep = 0;
          if (ndropped < MAX_DROPPED) dropped[ndropped++] = room;
          if (!s_logged) {
            s_logged = 1;
            log_printf("[roomdist] first room left out: %.63s at %.0f m", room + 0x78, __builtin_sqrtf(d2));
          }
        }
      }
    }
    if (keep) items[w++] = room;
  }
  memcpy(s_dropped, dropped, ndropped * sizeof dropped[0]);
  s_ndropped = ndropped;
#if ROOM_AB
  s_frame_dropped += ndropped;
#endif
  if (w != count) {
    for (int i = w; i < count; i++) items[i] = NULL;
    memcpy((char *)list + 4, &w, sizeof w);
  }
}

#if ROOM_AB
static int cmp_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}

static void window_report(void) {
  if (s_window < 0) return;
  const char *mode = (s_window & 1) ? "B dropped" : "A all";
  if (s_scene_frames < 20) {
    log_printf("[roomab] window %d %s: %u frames, %u with the scene (not enough)", s_window, mode, s_frames,
               s_scene_frames);
  } else {
    unsigned n = s_n < MAX_FRAMES ? s_n : MAX_FRAMES;
    qsort(s_frame_us, n, sizeof s_frame_us[0], cmp_u32);
    double f = s_scene_frames;
    log_printf("[roomab] window %d %s: %u scene frames, frame mean %.1f ms p50 %.1f p90 %.1f | %.1f draws | "
               "rooms %.1f, beyond %d m %.1f (dropped %.1f) | far: %s",
               s_window, mode, s_scene_frames, s_sum_us / 1000.0 / f, s_frame_us[n / 2] / 1000.0,
               s_frame_us[n * 9 / 10] / 1000.0, s_draws / f, s_rooms / f, ROOM_AB_RADIUS_M, s_far / f,
               s_dropcount / f, s_far_len ? s_far_names : "none");
  }
  s_frames = s_scene_frames = s_n = 0;
  s_sum_us = s_draws = s_rooms = s_far = s_dropcount = 0;
  s_far_names[0] = 0;
  s_far_len = 0;
}

void room_dist_frame(uint64_t swap_end_us, uint64_t draws_total) {
  if (!o_collect) return;
  uint64_t frame_us = s_prev_swap ? swap_end_us - s_prev_swap : 0;
  uint64_t draws = draws_total - s_prev_draws;
  s_prev_swap = swap_end_us;
  s_prev_draws = draws_total;
  if (s_frame_scene && frame_us) {
    if (!s_t0) { s_t0 = swap_end_us; s_window = 0; }
    s_scene_frames++;
    if (s_n < MAX_FRAMES) s_frame_us[s_n] = (uint32_t)frame_us;
    s_n++;
    s_sum_us += frame_us;
    s_draws += draws;
    s_rooms += s_frame_rooms;
    s_far += s_frame_far;
    s_dropcount += s_frame_dropped;
  }
  s_frames++;
  s_frame_scene = s_frame_rooms = s_frame_far = s_frame_dropped = 0;
  if (s_t0) {
    int w = (int)((swap_end_us - s_t0) / (ROOM_AB_WINDOW_S * 1000000ull));
    if (w != s_window) { window_report(); s_window = w; }
  }
}
#else
void room_dist_frame(uint64_t swap_end_us, uint64_t draws_total) { (void)swap_end_us; (void)draws_total; }
#endif

/* The distance from room_distance.txt: whole metres, 10 to 1000; anything
 * else (or no file) leaves every room drawn. */
static int read_radius(void) {
  char buf[16] = {0};
  SceUID fd = sceIoOpen(DATA_PATH "/room_distance.txt", SCE_O_RDONLY, 0);
  if (fd < 0) return 0;
  int n = sceIoRead(fd, buf, sizeof buf - 1);
  sceIoClose(fd);
  if (n <= 0) return 0;
  int m = atoi(buf);
  return (m >= 10 && m <= 1000) ? m : 0;
}

void room_dist_install(void) {
  static const char *const k_sym = "_Z18CollectActiveRoomsP5SceneR4ListIP8CAurRoomE";
  int metres = read_radius();
#if !ROOM_AB
  if (!metres) {
    log_printf("[roomdist] off: every room is drawn (ux0:data/kotor2/room_distance.txt sets a distance)");
    return;
  }
#endif
  uintptr_t original = so_symbol(&kotor_mod, k_sym);
  uintptr_t repl = (uintptr_t)&h_collect, callable = 0;
  unsigned n = 0, bad = 0;
  s_camera = (void **)so_symbol(&kotor_mod, "CurrentCamera");
  for (int i = 0; original && s_camera && i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
    Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] : &kotor_mod.relplt[i - kotor_mod.num_reldyn];
    unsigned type = ELF32_R_TYPE(rel->r_info);
    if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) continue;
    Elf32_Sym *sym = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
    if (strcmp(kotor_mod.dynstr + sym->st_name, k_sym) != 0) continue;
    uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
    if ((*slot ^ original) & ~(uintptr_t)1) { bad++; continue; }
    if (!callable) callable = *slot;
    kuKernelCpuUnrestrictedMemcpy(slot, &repl, sizeof repl);
    n++;
  }
  o_collect = (void (*)(void *, void *))callable;
  s_radius = o_collect ? (float)metres : 0.0f;
#if ROOM_AB
  log_printf("[roomab] %s: A = all rooms, B = rooms beyond %d m dropped, windows of %d s (slots %u, other hooks %u)",
             o_collect ? "armed" : "NOT armed", ROOM_AB_RADIUS_M, ROOM_AB_WINDOW_S, n, bad);
#else
  if (o_collect)
    log_printf("[roomdist] on: rooms beyond %d m are not drawn (slots %u)", metres, n);
  else
    log_printf("[roomdist] NOT armed: CollectActiveRooms slots %u, other hooks %u, camera %p", n, bad,
               (void *)s_camera);
#endif
}
