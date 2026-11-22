/* ============================================================================================
 * vehicles.c - the vehicle Sarge drives: routes with reversing, a steady turret, getting unstuck
 *
 * User 2026-11-20:
 *   1. "vehicle navigation needs to be reworked to allow for reversing to reorient the vehicle to fit through
 *      gaps"  ([controls] vehicle_reverse=1)
 *   2. "the tank turret is jittery when driving"  ([controls] turret_smooth=1)
 *   3. "vehicles can get stuck on objects - make them able to get unstuck" (Desert 2: a captured jeep wedged
 *      against a curved sandbag wall next to a rock)  ([controls] vehicle_unstick=1)
 *
 * Game facts (binary, checked against the exe 2026-11-20):
 *   VEHICLE: +0x04/+0x06 position, +0x08 hull facing (0..255), +0x9c pAnimation (turret ANIM at +0xe4),
 *   +0xb8 full_velocity (wanted; FUN_00415af0 accelerates / brakes it from the FORWARD / BACK input),
 *   +0xba velocity, +0xbc spin (pending turn: FUN_00415a80 adds +8 for TURN_LEFT (input 0x10), -8 for
 *   TURN_RIGHT (0x20), at most once per 60 ms), +0xbe max / +0xc0 min velocity, +0xc2 gun_facing, +0xc9 bTurret,
 *   +0xe8 npc, +0x10c framesstuck, +0x14a prevBadTiles. Input 0x100 halves the wanted speed.
 *   VehicleUpdate 0x4176f0 (__fastcall ecx vehicle; called only from 0x419c52, the per-army loop 0x419c30):
 *     - while full_velocity < 0 the spin is NEGATED (car-like steering: backing up with TURN_LEFT turns the hull
 *       the other way); jeep and convoy scale the spin by speed (no turning on the spot when Sarge drives).
 *     - player vehicles (npc 0): a move is allowed when (recoil <= 1 and fewer than 4 untrafficable tiles under
 *       the new mask) or fewer bad tiles than before or cheat_move (0x4f73d0). recoil = tiles NEWLY covered that
 *       hold an object with movement > 0 (damageItemsInMask 0x417ef0). Refused moves count framesstuck; every
 *       20 frames one more object tile is tolerated (recoil <= framesstuck / 20) as long as the mask has at most
 *       4 bad tiles. So a vehicle pressed against sandbags creeps into them a tile at a time, and once it is
 *       wedged with 5+ bad tiles under it nothing it can do reduces the count: stuck for good.
 *     - when the hull turns, VehicleLockTurret 0x417eb0 (__fastcall ecx vehicle, edx spin; call at 0x417ce8)
 *       turns the turret by the same amount ("add vehicle turn to immediate turret turn").
 *   AnimFigureWouldMoveTo 0x414f50 (__fastcall ecx ANIM, edx facing, stack velocity; returns SPOINT x | y << 16):
 *   where this frame's move would put the vehicle (no side effects).
 *
 * 1. Routes with reversing: pathfind.c path_vehicle_plan_* (hybrid A* over x, y, heading with forward, backward
 *    and on-the-spot motions and the vehicle's footprint at every heading). It runs in slices of PLAN_SLICE_MS per
 *    frame; the vehicle waits meanwhile. The driver here follows the poses segment by segment (a segment = poses
 *    of one direction): pure pursuit on a point LOOKAHEAD px ahead, forward or backward (rear toward the point),
 *    turning on the spot for on-the-spot poses; at the end of a segment it stops and changes direction. It plans
 *    again when it makes no progress for REPLAN_STUCK_MS or is more than OFF_ROUTE px off the route (at most
 *    MAX_REPLANS times), and when a partial route ends. Steering (vehicles_steer) knows that the game inverts the
 *    turn while rolling backward. If no plan is found: the old forward route (path_find_vehicle).
 * 2. Steady turret: Sarge's turret is aimed in world terms only by us - VehicleLockTurret is skipped for it (the
 *    hull turning 8 facings at once turned the turret with it and our aim pulled it back over the next frames:
 *    the jitter), and the aim eases toward the pointer's world point (continuous angle, TURRET_EASE_MS, at most
 *    the original speed 8 facings / 60 ms) with a dead band (TURRET_DEAD_START / _STOP facings) so one-pixel
 *    camera / pointer rounding no longer flips the sprite between two facings.
 * 3. Unstick (vehicles_update, around the game's update of the vehicle Sarge drives): this frame's move is
 *    predicted (AnimFigureWouldMoveTo, the hull after the spin) and the blocking tiles under the footprint counted
 *    before and after (pathfind.c path_vehicle_overlap: movement layer >= 15 + blocking objects).
 *      - a move that would put MORE object tiles under the vehicle than now, and more than WEDGE_TOUCH, is
 *        refused (full_velocity and spin 0 for this update): no more creeping into sandbags / rocks / walls.
 *      - when the game has refused every move for UNSTICK_AFTER_MS while Sarge drives, a move that does not
 *        increase the blocking tiles is let through with cheat_move for that one update (backing out, turning
 *        free) when it also leads out: probes UNSTICK_PROBE_PX, 2x, 3x further the same way find fewer blocking
 *        tiles and nowhere more.
 *    Routes keep clear of blocking objects too (pathfind.c vtile_object).
 *
 * Wraps: call 0x419c52 -> VehicleUpdate (vehicles_update), call 0x417ce8 -> VehicleLockTurret
 * (vehicles_turret_lock). Both only change the vehicle Sarge drives, and only with their switch on.
 * Trace: [debug] log=3 writes one line per update of Sarge's vehicle ("veh:").
 * ============================================================================================ */
#include "common.h"
#include <math.h>
#include <stdlib.h>

/* ---- game memory */
#define SARGE               (*(BYTE **)0x697520)
#define SARGE_VEHICLE(u)    (*(BYTE **)((u) + 0x2d0))
#define GAME                (*(BYTE **)0x4b3f70)            /* gpGame */
#define GAME_FRAME_MS(g)    (*(int *)((g) + 0x4c))          /* this frame's game time (ms) */
#define INPUT               (*(volatile DWORD *)0x6a547c)
#define INPUT_FORWARD       0x002
#define INPUT_BACK          0x004
#define INPUT_TURN_LEFT     0x010           /* spin +8 (FUN_00415a80 edx 1) - while rolling forward / standing */
#define INPUT_TURN_RIGHT    0x020           /* spin -8 */
#define INPUT_SLOW          0x100           /* wanted speed halved */
#define CHEAT_MOVE          (*(volatile int *)0x4f73d0)    /* cheat_move: VehicleUpdate lets any move through */

#define V_X(v)              (*(short *)((v) + 0x04))
#define V_Y(v)              (*(short *)((v) + 0x06))
#define V_HULL(v)           (*(short *)((v) + 0x08) & 255)
#define V_ANIM(v)           (*(BYTE **)((v) + 0x9c))
#define V_FULL_VEL(v)       (*(short *)((v) + 0xb8))
#define V_VEL(v)            (*(short *)((v) + 0xba))
#define V_SPIN(v)           (*(short *)((v) + 0xbc))
#define V_MAX_VEL(v)        (*(short *)((v) + 0xbe))
#define V_TURRET(v)         (*(short *)((v) + 0xc2))
#define V_HAS_TURRET(v)     (*(BYTE *)((v) + 0xc9))
#define V_FRAMES_STUCK(v)   (*(int *)((v) + 0x10c))
#define V_TYPE(v)           ((*(int *)(v) >> 16) & 0x3f)    /* 2 jeep, 3 tank, 4 halftrack, 5 convoy */
#define V_TYPE_JEEP         2
#define V_TYPE_FIXED_GUN    4               /* halftrack: gun 0x20 either side of the hull */
#define V_TYPE_CONVOY       5
#define TURRET_ANIM(v)      (V_ANIM(v) + 0xe4)

typedef void (__fastcall *VehicleFn)(BYTE *v);
typedef void (__fastcall *LockTurretFn)(BYTE *v, int spin);
typedef DWORD (__fastcall *WouldMoveFn)(BYTE *anim, int facing, int velocity);
typedef void (__fastcall *TurretSpriteFn)(BYTE *sprite, int direction);
#define VEHICLE_UPDATE      ((VehicleFn)0x4176f0)
#define LOCK_TURRET         ((LockTurretFn)0x417eb0)
#define WOULD_MOVE_TO       ((WouldMoveFn)0x414f50)
#define SET_ANIM_FACING     ((TurretSpriteFn)0x4146c0)

/* ---- tuning */
#define PLAN_SLICE_MS       4.0     /* search time per gameplay frame */
#define LOOKAHEAD           26      /* px: pure pursuit point ahead on the route */
#define POSE_REACHED        14      /* px: a pose counts as reached */
#define SEGMENT_END         12      /* px: the end of a segment (before a change of direction) is reached */
#define ARRIVE              22      /* px: at the clicked spot */
#define OFF_ROUTE           56      /* px: further from the route than this: plan again */
#define REPLAN_STUCK_MS     1500    /* no progress this long: plan again */
#define MAX_REPLANS         4
#define DRIVE_TIMEOUT_MS    45000
#define STEER_DEAD          4       /* facings: no turning closer than this (the hull turns 8 at a time) */
#define STEER_DRIVE         28      /* facings: tank / halftrack drive only while this close to the heading ... */
#define STEER_KEEP          44      /* ... and keep driving up to this */
#define SPIN_DONE           6       /* facings: an on-the-spot turn is done */
#define JEEP_BACK_FROM      96      /* facings: a jeep asked forward with the heading further off backs up while turning */
#define JEEP_BACK_UNTIL     40      /* ... until it is this close */
#define VEH_UTURN_FROM      64      /* facings: a forward U-turn (turning round) steers this far to its side until closer (common.h) */
#define STEP_TANK           16      /* px per planned motion: vehicles that turn on the spot */
#define STEP_JEEP           24      /* ... jeep / convoy (turn only while moving: wider) */
#define TURRET_RATE         (8.0 / 60.0)    /* facings per ms at most (the original mouse turret) */
#define TURRET_EASE_MS      70.0    /* easing time constant toward the aim */
#define TURRET_DEAD_START   1.6     /* facings off the aim before the turret moves */
#define TURRET_DEAD_STOP    0.35    /* ... and it stops this close */
#define TURRET_MIN_DIST     16      /* px: pointer this close to the vehicle: keep the turret */
#define WEDGE_TOUCH         1       /* object tiles a vehicle may touch (more = refused when it would grow) */
#define UNSTICK_AFTER_MS    250     /* every move refused this long: moves that do not go deeper are let through */
#define UNSTICK_PROBES      3       /* ... and lead out: probes this many times ... */
#define UNSTICK_PROBE_PX    8       /* ... this far apart along the move */

static int wrap_dir(int d) { d &= 255; return d >= 128 ? d - 256 : d; }
static double wrap_dirf(double d) { d = fmod(d, 256.0); if (d < 0) d += 256.0; return d >= 128.0 ? d - 256.0 : d; }
static int is_jeepish(BYTE *v) { int t = V_TYPE(v); return t == V_TYPE_JEEP || t == V_TYPE_CONVOY; }
static BYTE *sarge_vehicle(void) { BYTE *u = SARGE; return u ? SARGE_VEHICLE(u) : NULL; }

/* facing (continuous, 0..256) of the world vector (dx, dy), from the game's facing tables (controls.c).
 * 2026-11-20 (user: jeep shots miss the pointer near the window edge): the game's 256 facings are NOT evenly
 * spaced in angle (Wine log: f32 = (-0.760, -0.652) = 40.6 deg, not 45). This used to assume even spacing from
 * facings 0 and 64, so between the four main directions the turret was up to ~4 deg off: ~30 px beside the
 * pointer at 400-600 px (the window edge), a few px near the vehicle. Now: the angle of every table facing,
 * the two facings around the wanted angle, linear in between - the facing the game's own shot table
 * (0x4ea778 / 0x4e9f78, used by the projectile) really points along. */
static double angle_diff(double a, double b) { double d = a - b; while (d > M_PI) d -= 2 * M_PI; while (d < -M_PI) d += 2 * M_PI; return d; }
static double facing_of(double dx, double dy)
{
    static double ang[256];
    static int ready;
    if (!ready) {
        for (int f = 0; f < 256; f++) {
            double x, y; controls_facing_vector(f, &x, &y);
            if (x == 0 && y == 0) return 0;
            ang[f] = atan2(y, x);
        }
        ready = 1;
    }
    double a = atan2(dy, dx);
    int best = 0; double bd = 1e9;
    for (int f = 0; f < 256; f++) { double d = fabs(angle_diff(a, ang[f])); if (d < bd) { bd = d; best = f; } }
    double d = angle_diff(a, ang[best]);
    int next = (best + 1) & 255, prev = (best - 1) & 255;
    double step_up = angle_diff(ang[next], ang[best]), step_dn = angle_diff(ang[prev], ang[best]);
    double f = best;
    if (step_up != 0 && (d > 0) == (step_up > 0)) f += d / step_up;          /* between best and best + 1 */
    else if (step_dn != 0) f -= d / step_dn;                                  /* between best and best - 1 */
    f = fmod(f, 256.0);
    return f < 0 ? f + 256.0 : f;
}
static int facing_to(double dx, double dy) { return (int)lround(facing_of(dx, dy)) & 255; }

/* ---------------------------------------------------------------- steering */
/* Hull toward `heading` (0..255, the facing the hull should have); drive 1 forward, -1 backward, 0 stand.
 * Jeep / convoy turn only while moving (VehicleUpdate scales their spin by speed): they always roll while turning,
 * and asked to go forward with the heading far behind they back up while turning (a K-turn) until it is within
 * JEEP_BACK_UNTIL - before 2026-11-20 W A S D / hold toward a direction behind a jeep only pressed TURN with no
 * throttle, and a jeep cannot turn standing: nothing happened (Wine, Desert 1 jeep). Returns the remaining turn. */
int vehicles_steer(BYTE *v, int heading, int drive, int slow)
{
    int hull = V_HULL(v), d = wrap_dir(heading - hull), turn = 0;
    static int driving, jeep_back;
    if (is_jeepish(v) && drive > 0) {
        if (!jeep_back && abs(d) > JEEP_BACK_FROM) jeep_back = 1;
        else if (jeep_back && abs(d) < JEEP_BACK_UNTIL) jeep_back = 0;
        if (jeep_back) drive = -1;
    } else jeep_back = 0;
    if (d > STEER_DEAD) turn = 1;                  /* hull facing up */
    else if (d < -STEER_DEAD) turn = -1;
    if (V_FULL_VEL(v) < 0) turn = -turn;           /* the game inverts the turn while rolling backward */
    INPUT &= ~(INPUT_FORWARD | INPUT_BACK | INPUT_TURN_LEFT | INPUT_TURN_RIGHT | INPUT_SLOW);
    if (turn > 0) INPUT |= INPUT_TURN_LEFT;
    if (turn < 0) INPUT |= INPUT_TURN_RIGHT;
    int go;
    if (!drive) go = 0;
    else if (is_jeepish(v)) go = 1;                /* turns only while moving */
    else { go = abs(d) <= (driving ? STEER_KEEP : STEER_DRIVE); }
    driving = go;
    if (go) INPUT |= drive > 0 ? INPUT_FORWARD : INPUT_BACK;
    if (go && (slow || abs(d) > 20)) INPUT |= INPUT_SLOW;
    return d;
}

/* ---------------------------------------------------------------- turning round instead of backing up
 * 2026-11-20 (user: "backing up a vehicle will continue backing up - make it flip around if there is enough space
 * after backing up for too long. this is direct mouse control"): controls.c backs the vehicle up toward a pointer
 * held behind it; the camera follows, so the pointer stays behind and it backed up for ever. After
 * REVERSE_FLIP_MS of backing up controls.c asks this: is there room to turn the hull round to `heading`?
 * The turn is simulated the way vehicles_steer drives it, every pose tested with the vehicle's footprint
 * (pathfind.c path_vehicle_overlap: movement layer >= 15 + blocking objects; never more blocking tiles than the
 * vehicle has under it now):
 *   - tank / halftrack: on the spot, 8 facings a step, the shorter way round (the way vehicles_steer turns);
 *   - jeep / convoy (turn only while moving, 8 facings per STEP_JEEP px - the planner's model): first a forward
 *     U-turn, either side (Wine Desert 1: the K-turn alone backed further into the barrels - never room), else
 *     the K-turn vehicles_steer drives (backward while the heading is more than JEEP_BACK_UNTIL off, then forward
 *     until it is within STEER_DEAD); ROOM_STEP px per test. */
#define ROOM_STEP 6
/* one jeep turn simulated: uturn 0 = the K-turn vehicles_steer drives (backward while the heading is more than
 * JEEP_BACK_FROM off, until JEEP_BACK_UNTIL), else a forward U-turn to side uturn (+1 facings up, -1 down) */
static int jeep_turn_fits(BYTE *v, int heading, int uturn, int now)
{
    int type = V_TYPE(v) - 1, h = V_HULL(v);
    double x = V_X(v), y = V_Y(v), f = h;
    int back = !uturn && abs(wrap_dir(heading - h)) > JEEP_BACK_FROM;
    for (int i = 0; i < 200; i++) {                          /* at most 1200 px of motion */
        double d = wrap_dirf(heading - f);
        if (fabs(d) <= STEER_DEAD) return 1;
        if (fabs(d) < JEEP_BACK_UNTIL) back = 0;
        double turn = 8.0 * ROOM_STEP / STEP_JEEP;
        if (turn > fabs(d)) turn = fabs(d);
        int s = uturn && fabs(d) > VEH_UTURN_FROM ? uturn : d > 0 ? 1 : -1;
        f += s * turn;
        double dx, dy; controls_facing_vector((int)lround(f) & 255, &dx, &dy);
        x += (back ? -dx : dx) * ROOM_STEP; y += (back ? -dy : dy) * ROOM_STEP;
        if (path_vehicle_overlap(type, x, y, (int)lround(f) & 255, NULL) > now) return 0;
    }
    return 0;
}
/* Returns 0 = no room, 1 = turn as vehicles_steer does (tank: on the spot; jeep: K-turn), +2 / -2 = jeep: forward
 * U-turn turning to that side (controls.c steers the hull VEH_UTURN_FROM facings to that side until the heading
 * is closer than that). Jeep: the U-turn is preferred (it does not back up any further), the side with room first
 * toward the shorter way round. */
int vehicles_turn_room(BYTE *v, int heading)
{
    int type = V_TYPE(v) - 1, h = V_HULL(v);
    int now = path_vehicle_overlap(type, V_X(v), V_Y(v), h, NULL);
    int sign = wrap_dir(heading - h) >= 0 ? 1 : -1;
    if (!is_jeepish(v)) {
        for (int k = 8; k < abs(wrap_dir(heading - h)) + 8; k += 8)
            if (path_vehicle_overlap(type, V_X(v), V_Y(v), (h + sign * k) & 255, NULL) > now) return 0;
        return 1;
    }
    if (jeep_turn_fits(v, heading, sign, now)) return 2 * sign;
    if (jeep_turn_fits(v, heading, -sign, now)) return -2 * sign;
    return jeep_turn_fits(v, heading, 0, now);
}

/* ---------------------------------------------------------------- click to drive with reversing */
#define MAX_POSES 400
static struct {
    BYTE *v;
    int tx, ty;
    int planning;                   /* search running (pathfind.c) */
    int partial;                    /* the route ends short of the target */
    VehPose p[MAX_POSES]; int n, i;
    DWORD since, progress_at;
    int prog_x, prog_y, prog_h, prog_i;
    int replans;
} D;

static int plan_begin(BYTE *v)
{
    int spin = !is_jeepish(v);
    D.planning = path_vehicle_plan_begin(V_TYPE(v) - 1, V_X(v), V_Y(v), V_HULL(v), D.tx, D.ty, spin, spin ? STEP_TANK : STEP_JEEP);
    D.n = D.i = 0;
    if (!D.planning) mlog("vehicles: plan could not start (%d,%d -> %d,%d)", V_X(v), V_Y(v), D.tx, D.ty);
    return D.planning;
}

/* the old forward route as poses (fallback) */
static void forward_route(BYTE *v)
{
    POINT wp[32];
    int n = path_find_vehicle(V_TYPE(v) - 1, V_X(v), V_Y(v), D.tx, D.ty, wp, 32);
    D.n = 0; D.i = 0; D.partial = 0;
    int x = V_X(v), y = V_Y(v);
    for (int k = 0; k < n && D.n < MAX_POSES; k++) {
        double dx = wp[k].x - x, dy = wp[k].y - y, len = sqrt(dx * dx + dy * dy);
        int steps = (int)(len / 24) + 1, f = facing_to(dx, dy);
        for (int s = 1; s <= steps && D.n < MAX_POSES; s++)
            D.p[D.n++] = (VehPose){ (short)(x + dx * s / steps), (short)(y + dy * s / steps), (short)f, 1 };
        x = wp[k].x; y = wp[k].y;
    }
    mlog("vehicles: fallback forward route, %d waypoints, %d poses", n, D.n);
}

void vehicles_drive_start(BYTE *v, int tx, int ty, DWORD now)
{
    memset(&D, 0, sizeof D);
    D.v = v; D.tx = tx; D.ty = ty;
    D.since = D.progress_at = now;                         /* controls.c clock */
    D.prog_x = V_X(v); D.prog_y = V_Y(v); D.prog_h = V_HULL(v);
    if (!plan_begin(v)) forward_route(v);
}

static double dist2(BYTE *v, int x, int y) { double dx = x - V_X(v), dy = y - V_Y(v); return dx * dx + dy * dy; }

/* Per frame while a click-to-drive is on. Returns 0 when it is over (arrived / gave up). */
int vehicles_drive(BYTE *v, DWORD now)
{
    if (v != D.v) return 0;
    if (now - D.since > DRIVE_TIMEOUT_MS) { mlog("vehicles: drive timed out"); return 0; }
    if (D.planning) {
        vehicles_steer(v, V_HULL(v), 0, 0);                  /* wait (brake) while planning */
        int r = path_vehicle_plan_step(PLAN_SLICE_MS);
        if (r == 0) return 1;
        D.planning = 0;
        if (r > 0) { D.n = path_vehicle_plan_result(D.p, MAX_POSES); D.i = 0; D.partial = r == 2; }
        if (r < 0 || D.n == 0) {
            if (D.replans) { mlog("vehicles: no route from here"); return 0; }
            forward_route(v);
            if (!D.n) return 0;
        }
        D.progress_at = now; D.prog_x = V_X(v); D.prog_y = V_Y(v); D.prog_h = V_HULL(v); D.prog_i = D.i;
    }
    if (dist2(v, D.tx, D.ty) <= ARRIVE * ARRIVE) { vehicles_steer(v, V_HULL(v), 0, 0); return 0; }
    if (D.i >= D.n) {
        if (D.partial && D.replans < MAX_REPLANS) { D.replans++; plan_begin(v); return 1; }
        vehicles_steer(v, V_HULL(v), 0, 0);
        return 0;
    }
    /* progress: moved / turned, or a later pose; otherwise plan again after a while */
    if (abs(V_X(v) - D.prog_x) + abs(V_Y(v) - D.prog_y) >= 6 || abs(wrap_dir(V_HULL(v) - D.prog_h)) >= 8 || D.i != D.prog_i) {
        D.progress_at = now; D.prog_x = V_X(v); D.prog_y = V_Y(v); D.prog_h = V_HULL(v); D.prog_i = D.i;
    }
    int off = dist2(v, D.p[D.i].x, D.p[D.i].y) > OFF_ROUTE * OFF_ROUTE && D.p[D.i].dir != 0;
    if (now - D.progress_at > REPLAN_STUCK_MS || off) {
        if (D.replans >= MAX_REPLANS) { mlog("vehicles: gave up (stuck at %d,%d)", V_X(v), V_Y(v)); return 0; }
        D.replans++;
        mlog("vehicles: %s at %d,%d hull %d (pose %d/%d) - planning again", off ? "off the route" : "no progress", V_X(v), V_Y(v), V_HULL(v), D.i, D.n);
        plan_begin(v);
        return 1;
    }
    /* the current segment: poses i..e of the same direction */
    int dir = D.p[D.i].dir, e = D.i;
    while (e + 1 < D.n && D.p[e + 1].dir == dir) e++;
    if (dir == 0) {                                           /* turn on the spot to pose e's heading */
        int want = D.p[e].facing;
        if (abs(wrap_dir(want - V_HULL(v))) <= SPIN_DONE) { D.i = e + 1; return 1; }
        vehicles_steer(v, want, 0, 0);
        return 1;
    }
    /* poses passed or reached along the segment */
    double hx, hy;
    while (D.i < e) {
        controls_facing_vector(D.p[D.i].facing, &hx, &hy);
        double ax = D.p[D.i].x - V_X(v), ay = D.p[D.i].y - V_Y(v);
        if (ax * ax + ay * ay <= POSE_REACHED * POSE_REACHED || (ax * hx + ay * hy) * dir < 0) D.i++;
        else break;
    }
    /* end of the segment reached: next segment (direction change) */
    double ex = D.p[e].x - V_X(v), ey = D.p[e].y - V_Y(v);
    controls_facing_vector(D.p[e].facing, &hx, &hy);
    int last = e == D.n - 1;
    if (!last && (ex * ex + ey * ey <= SEGMENT_END * SEGMENT_END || (ex * hx + ey * hy) * dir < -2)) {
        D.i = e + 1;
        if (g_cfg.log >= 2) mlog("vehicles: segment end at %d,%d hull %d -> %s", V_X(v), V_Y(v), V_HULL(v),
                                 D.p[D.i].dir > 0 ? "forward" : D.p[D.i].dir < 0 ? "backward" : "turn on the spot");
        vehicles_steer(v, V_HULL(v), 0, 0);
        return 1;
    }
    /* pure pursuit: the first pose of the segment at least LOOKAHEAD away, else the segment's end */
    int j = D.i;
    while (j < e && dist2(v, D.p[j].x, D.p[j].y) < LOOKAHEAD * LOOKAHEAD) j++;
    double gx = D.p[j].x, gy = D.p[j].y;
    if (last && j == e) { gx = D.tx; gy = D.ty; }             /* the end: the clicked spot itself */
    double dx = gx - V_X(v), dy = gy - V_Y(v);
    int heading;
    if (dx * dx + dy * dy < 4 * 4) heading = D.p[j].facing;  /* on the point: its heading */
    else heading = facing_to(dx, dy);
    if (dir < 0) heading = (heading + 128) & 255;             /* backward: the rear toward the point */
    double rem = sqrt(ex * ex + ey * ey);
    vehicles_steer(v, heading, dir, !last && rem < 40);
    return 1;
}

/* modern.c route line: the vehicle, then the rest of the route (thinned) */
int vehicles_route(POINT *pts, int max)
{
    BYTE *v = sarge_vehicle();
    if (!v || v != D.v || max < 2 || D.planning) return 0;
    int n = 0;
    pts[n++] = (POINT){ V_X(v), V_Y(v) };
    for (int k = D.i; k < D.n && n < max - 1; k += 2) pts[n++] = (POINT){ D.p[k].x, D.p[k].y };
    if (n < max) pts[n++] = (POINT){ D.tx, D.ty };
    return n;
}

/* ---------------------------------------------------------------- steady turret
 * snap: 0 = ease (dead band), 1 = fire press: exactly at the aim, 2 = fire held: exactly at the aim once within
 * TURRET_FIRE_SNAP facings (larger turns still ease at the turret's speed).
 * 2026-11-20 (user: jeep shots not lining up with the pointer at the window edge) - the aim is the line the game's
 * shot really takes: VehicleFireWeapon 0x416c60 starts it at the MUZZLE (FUN_00416c00: turret ANIM position + its
 * frame's hotspot; vehicles_muzzle), not at the vehicle's position (for the jeep its rear), and flies along the
 * turret facing; while firing the dead band (TURRET_DEAD_START, up to 1.6 facings = 2.2 deg = ~20 px at the window
 * edge) no longer applies. Together with the table-exact facing_of above: Wine, Desert 1 jeep, miss at the
 * pointer 19-32 px -> <= ~4 px (one facing step at that distance). */
#define TURRET_FIRE_SNAP    3.0     /* facings: firing and the aim this close: the turret is put exactly on it */
#define TURRET_FIRE_KEEP    0.65    /* facings: ... but kept where it is this close (the muzzle moves ~3 px with the
                                       facing's frame: the aim then flipped between two facings every frame) */
static struct { BYTE *v; double cur; int written, moving; DWORD last; } T;

void vehicles_aim_turret(BYTE *v, double wx, double wy, DWORD now, int snap)
{
    if (!V_HAS_TURRET(v) || !V_ANIM(v)) return;
    int tur = V_TURRET(v) & 255;
    if (T.v != v || tur != T.written) { T.v = v; T.cur = tur; T.written = tur; T.moving = 0; T.last = now; }
    double dx = wx - V_X(v), dy = wy - V_Y(v);
    double dt = (double)(now - T.last);
    T.last = now;
    if (dt > 100) dt = 100;
    if (dx * dx + dy * dy < TURRET_MIN_DIST * TURRET_MIN_DIST) return;
    int mx, my;
    /* aim along the shot's own line - only while firing (2026-11-20, user: "the turret is jittery again"): the muzzle
     * moves with the turret's current frame, so aiming from it while turning changed the wanted facing every step */
    if (snap && vehicles_muzzle(v, &mx, &my)) {
        double ex = wx - mx, ey = wy - my;
        if (ex * ex + ey * ey >= TURRET_MIN_DIST * TURRET_MIN_DIST) { dx = ex; dy = ey; }
    }
    double want = facing_of(dx, dy);
    if (V_TYPE(v) == V_TYPE_FIXED_GUN) {                       /* gun 0x20 either side of the hull */
        double rel = wrap_dirf(want - V_HULL(v));
        if (rel > 0x20) rel = 0x20;
        if (rel < -0x20) rel = -0x20;
        want = V_HULL(v) + rel;
    }
    double err = wrap_dirf(want - T.cur);
    if (snap == 2 && fabs(wrap_dirf(want - tur)) <= TURRET_FIRE_KEEP) { T.cur = tur; T.moving = 0; }   /* already on it */
    else if (snap == 1 || (snap == 2 && fabs(err) <= TURRET_FIRE_SNAP)) { T.cur = want; T.moving = 0; }
    else {
        if (!T.moving && fabs(err) > TURRET_DEAD_START) T.moving = 1;
        if (T.moving) {
            double step = fabs(err) * (1.0 - exp(-dt / TURRET_EASE_MS)), cap = TURRET_RATE * dt, floor_ = 0.02 * dt;
            if (step < floor_) step = floor_;                  /* no endless crawl at the end */
            if (step > cap) step = cap;
            if (step >= fabs(err)) { T.cur = want; T.moving = 0; }
            else T.cur += err > 0 ? step : -step;
            if (fabs(wrap_dirf(want - T.cur)) < TURRET_DEAD_STOP) T.moving = 0;
        }
    }
    T.cur = fmod(T.cur + 256.0, 256.0);
    int dir = (int)lround(T.cur) & 255;
    if (dir != tur) { V_TURRET(v) = (short)dir; SET_ANIM_FACING(TURRET_ANIM(v), dir); }
    T.written = dir;
}

/* call 0x417ce8 in VehicleUpdate: VehicleLockTurret(vehicle, spin) - the turret turns with the hull. Not for
 * Sarge's turret while turret_smooth (its aim is a world direction, kept by vehicles_aim_turret). */
void __fastcall vehicles_turret_lock(BYTE *v, int spin)
{
    if (g_cfg.turret_smooth && g_cfg.controls_modern && v && v == sarge_vehicle() && V_TYPE(v) != V_TYPE_FIXED_GUN) return;
    LOCK_TURRET(v, spin);
}

/* ---------------------------------------------------------------- update of Sarge's vehicle: unstick */
static struct { BYTE *v; int stuck_ms; DWORD last_log; } U;

void __fastcall vehicles_update(BYTE *v)
{
    if (!v || v != sarge_vehicle() || !V_ANIM(v)) { VEHICLE_UPDATE(v); return; }
    if (U.v != v) { memset(&U, 0, sizeof U); U.v = v; }
    BYTE *g = GAME;
    int frame_ms = g ? GAME_FRAME_MS(g) : 16;
    int x0 = V_X(v), y0 = V_Y(v), h0 = V_HULL(v), fv = V_FULL_VEL(v), spin = V_SPIN(v);
    int intent = fv != 0 || spin != 0;
    int cheat = 0, refused = 0, ov_now = -1, ov_new = -1, obj_now = 0, obj_new = 0;
    if (g_cfg.vehicle_unstick && intent) {
        /* this update's move as VehicleUpdate will make it */
        int vel = fv, sp = spin;
        if (vel) { int a = abs(vel); if (a < 30) a = 30; if (a > 800) a = 800; vel = vel < 0 ? -a : a; }
        if (vel < 0) sp = -sp;
        if (is_jeepish(v) && V_MAX_VEL(v) > 0) {
            sp = sp * abs(vel) * 2 / V_MAX_VEL(v);
            if (sp < -8) sp = -8;
            if (sp > 8) sp = 8;
        }
        int nf = (h0 + sp) & 255;
        DWORD to = WOULD_MOVE_TO(V_ANIM(v), nf, vel);
        int nx = (short)(to & 0xffff), ny = (short)(to >> 16);
        int type = V_TYPE(v) - 1;
        ov_now = path_vehicle_overlap(type, x0, y0, h0, &obj_now);
        ov_new = path_vehicle_overlap(type, nx, ny, nf, &obj_new);
        if (obj_new > obj_now && obj_new > WEDGE_TOUCH) {     /* would go deeper into an object: refuse */
            V_FULL_VEL(v) = 0; V_SPIN(v) = 0; refused = 1;
        } else if (U.stuck_ms >= UNSTICK_AFTER_MS && ov_now > 0 && ov_new <= ov_now) {
            /* only a move that leads OUT: a few px further the same way there are fewer blocking tiles and nowhere
             * more (one frame's step of 1-2 px rarely changes the tile count; equal counts all the way = sliding
             * through, not out - Wine 2026-11-20: "<=" alone let a tank back into rocks 3 -> 9 tiles) */
            double mx = nx - x0, my = ny - y0, ml = sqrt(mx * mx + my * my);
            int better = 0, worse = 0;
            for (int k = 1; k <= UNSTICK_PROBES; k++) {
                double px, py; int pf;
                if (ml >= 0.5) { px = x0 + mx / ml * UNSTICK_PROBE_PX * k; py = y0 + my / ml * UNSTICK_PROBE_PX * k; pf = nf; }
                else { px = x0; py = y0; pf = (h0 + (sp > 0 ? 8 : -8) * k) & 255; }      /* turning on the spot */
                int o = path_vehicle_overlap(type, px, py, pf, NULL);
                if (o < ov_now) better = 1;
                if (o > ov_now) worse = 1;
            }
            cheat = better && !worse;
        }
    }
    int cheat_was = CHEAT_MOVE;
    if (cheat) CHEAT_MOVE = 1;
    VEHICLE_UPDATE(v);
    if (cheat) CHEAT_MOVE = cheat_was;
    int moved = V_X(v) != x0 || V_Y(v) != y0 || V_HULL(v) != h0;
    if (intent && !moved) U.stuck_ms += frame_ms > 0 && frame_ms < 200 ? frame_ms : 16;
    else if (moved) U.stuck_ms = 0;
    if (cheat && moved && g_cfg.log >= 2) mlog("vehicles: unstick move %d,%d h%d -> %d,%d h%d (blocking tiles %d -> %d)",
                                               x0, y0, h0, V_X(v), V_Y(v), V_HULL(v), ov_now, ov_new);
    if (refused && g_cfg.log >= 2) {
        DWORD t = GetTickCount();
        if (t - U.last_log > 500) { U.last_log = t; mlog("vehicles: move refused at %d,%d h%d: object tiles %d -> %d", x0, y0, h0, obj_now, obj_new); }
    }
    if (g_cfg.log >= 3)
        mlog("veh: %d,%d hull %d turret %d | full %d vel %d spin %d | in %#lx | stuck %d ms fs %d | ov %d->%d obj %d->%d%s%s",
             V_X(v), V_Y(v), V_HULL(v), V_TURRET(v) & 255, fv, V_VEL(v), spin, INPUT, U.stuck_ms, V_FRAMES_STUCK(v),
             ov_now, ov_new, obj_now, obj_new, cheat ? " CHEAT" : "", refused ? " REFUSED" : "");
}

/* ---------------------------------------------------------------- aim report (diagnostic, log >= 2) */
/* the shot's start as VehicleFireWeapon 0x416c60 takes it (FUN_00416c00): the turret ANIM (or the hull ANIM
 * without a turret) position + the current frame's hotspot (ANIM +0x88 frame table, +0x96 frame, 12 bytes each) */
int vehicles_muzzle(BYTE *v, int *x, int *y)
{
    BYTE *a = V_ANIM(v);
    if (!a) return 0;
    if (V_HAS_TURRET(v)) a += 0xe4;
    BYTE *frames = *(BYTE **)(a + 0x88);
    if (!frames) return 0;
    BYTE *f = frames + *(short *)(a + 0x96) * 0xc;
    *x = *(short *)(f + 8) + *(short *)(a + 0x10);
    *y = *(short *)(f + 10) + *(short *)(a + 0x12);
    return 1;
}

void vehicles_aim_report(BYTE *v, int wx, int wy, int firing, const char *tag)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (g_cfg.log < 2 || now - last < 120) return;
    last = now;
    int mx = V_X(v), my = V_Y(v);
    vehicles_muzzle(v, &mx, &my);
    int tur = V_TURRET(v) & 255;
    double ux, uy;
    controls_facing_vector(tur, &ux, &uy);
    double l = sqrt(ux * ux + uy * uy); if (l > 0) { ux /= l; uy /= l; }
    double dx = wx - mx, dy = wy - my;
    double along = dx * ux + dy * uy, miss = dx * uy - dy * ux;
    BYTE *w = *(BYTE **)(v + 0x20);
    int wt = w ? *(int *)w : -1, range = w && wt >= 0 && wt < 32 ? *(short *)(intptr_t)(0x50504e + wt * 0x18) * w[0x19] / 100 : -1;
    mlog("aim%s: veh type %d at %d,%d hull %d muzzle %d,%d | pointer world %d,%d cam %d,%d | turret %d want(veh) %.1f want(muzzle) %.1f | along %.0f miss %.1f px | weapon %d range %d (table %d x %d%%)%s",
         tag, V_TYPE(v), V_X(v), V_Y(v), V_HULL(v), mx, my, wx, wy, *(short *)0x4e2de8, *(short *)0x4e2dea, tur,
         facing_of(wx - V_X(v), wy - V_Y(v)), facing_of(dx, dy), along, miss, wt, range, w && wt >= 0 && wt < 32 ? *(short *)(intptr_t)(0x50504e + wt * 0x18) : -1, w ? w[0x19] : -1, firing ? " FIRE" : "");
}

/* modern.c reticle: can the vehicle's gun point at the world point at all? The halftrack's gun (type 4) only turns
 * 0x20 facings either side of the hull - beyond that its shots go along the arc's edge, not at the pointer
 * (the reticle's range ticks turn grey, as beyond the weapon's range). */
int vehicles_aim_reachable(BYTE *v, int wx, int wy)
{
    if (!v || V_TYPE(v) != V_TYPE_FIXED_GUN) return 1;
    double dx = wx - V_X(v), dy = wy - V_Y(v);
    int mx, my;
    if (vehicles_muzzle(v, &mx, &my)) { dx = wx - mx; dy = wy - my; }
    return fabs(wrap_dirf(facing_of(dx, dy) - V_HULL(v))) <= 0x20 + 0.5;
}

/* savestate.c: the turret / unstick state is keyed by vehicle address - saved with the vehicles */
void vehicles_savestate(void) { SAVESTATE(T); SAVESTATE(U); }
