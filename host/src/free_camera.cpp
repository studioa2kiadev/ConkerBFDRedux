// From CBFD-Recompiled's mouse camera (Copyright (c) 2026 Sean Ciaschi, MIT License; see
// recomp/THIRD_PARTY_LICENSE). Ours adds the right stick (the Conker tab's
// Free Camera option): it turns and tilts the same orbit, at up to stick_degrees_per_second.
//
// Free camera: a free orbit camera, called from hooks in recomp/conker.toml.
//
// RecompFrontend's General tab has a Mouse Sensitivity option. Above 0, the cursor is
// captured while the game is played (and released in the menus), and
// recompinput::get_mouse_deltas() gives the mouse's movement since the game last read
// its controllers, scaled by that sensitivity.
//
// The follow camera (struct108: gObjects[0].camera for player 1, D_800DBFF0 the one
// being played) looks at a point (+0x2BC) above its pivot at Conker's feet (+0x2A4),
// from an eye kept at a horizontal distance (+0x374) and height (+0x344) from the
// pivot. It doesn't keep its angle: every frame func_15125330 works it out (+0x37C)
// from where the eye is, and Conker's movement is relative to it. So once the mouse
// moves, the eye the camera wants (+0x2F8) is placed each frame from our own yaw and
// pitch around the look-at point, and the rest of the game follows. The camera stays
// where the mouse leaves it.
//
// Walls: the eye is placed as the game's camera collision (func_1512BB10) starts, the
// way the C-buttons' turning places it earlier in the same update (func_15122C5C). The
// collision moves the camera from where it was drawn last frame (+0x304) toward that eye
// and stops it at walls, sliding along them, and leaves the result in +0x2F8. The view
// (func_151284C4, in func_1512C490) then draws from there (+0x2EC). So the orbit stops at
// walls the way the game's own camera does.
//
// The orbit only runs where the C-buttons turn the camera (func_1512D390 ran this
// frame) and not in the look mode (func_15120158: hold R, aiming), so cutscenes, special
// cameras and aiming are the game's. Pressing C-left or C-right
// hands the camera back to the game until the mouse moves again.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL.h>

#include "recomp.h"
#include "recompinput/input_state.h"
#include "recompinput/input_types.h"

#include "conker.hpp"

namespace {
    // Degrees per pixel of mouse movement at 100% sensitivity.
    constexpr float degrees_per_pixel = 0.2f;
    // The right stick (Free Camera): degrees a second at full tilt, and how far it must
    // be pushed before it turns the camera.
    constexpr float stick_degrees_per_second = 150.0f;
    constexpr float stick_dead_zone = 0.2f;
    // The stick's turning eases toward the speed it asks for, over about this many seconds,
    // so it starts and stops smoothly instead of all at once. Past the dead zone, half of
    // the speed grows with the push and half with its square: small pushes turn finely.
    constexpr float stick_ease_seconds = 0.1f;
    // The ground: the eye is kept at least this many units above Conker's feet (+0x2A8), where
    // flat ground is; slopes the game's collision pushes it up from are handled after it.
    constexpr float eye_above_feet = 12.0f;
    // The collision pushing the eye up by more than this counts as the ground holding it.
    constexpr float ground_push = 2.0f;
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    // How far the camera may look up or down: pitch is the eye's angle above the
    // look-at point.
    constexpr float min_pitch = -25.0f * degrees_to_radians;
    constexpr float max_pitch = 75.0f * degrees_to_radians;
    constexpr uint32_t current_camera = 0x800DBFF0; // D_800DBFF0
    // Scroll wheel zoom: each notch scales the distance by this, between the nearest and
    // farthest of the game's own camera distances (D_800A34B0: the controller's four,
    // each a horizontal distance and a height from the pivot, 530 x 400 the farthest).
    constexpr float zoom_step = 1.12f;
    constexpr uint32_t camera_distances = 0x800A34B0; // D_800A34B0, 4 x { horizontal, height }
    constexpr int camera_distance_count = 4;
    // The orbit's distance (the wheel's, the game's closer one in tight spots, the floor's) glides
    // to a new value over about this many seconds instead of jumping there.
    constexpr float reach_ease_seconds = 0.2f;
    // Tilted toward the floor, the eye comes in along the orbit (looking up at Conker) rather
    // than stopping at once, but no nearer the look-at point than this; there it stops tilting.
    constexpr float closest = 170.0f;
    // Walls and tight spots: when the collision holds the eye nearer than it was, the view
    // follows at once (it mustn't go through walls); when it lets it go further again, the view
    // eases back out over about this many seconds, so a camera bumping along walls doesn't jump.
    constexpr float ease_out_seconds = 0.35f;
    // Line of sight (clear_reach): the eye stays sight_margin in front of anything in the way. If
    // that leaves it nearer Conker than sight_closest, it rises instead (looking down past what's in
    // the way, up to sight_lift_max more), by sight_lift_step tries; it comes back down over
    // lift_ease_seconds once there's room.
    constexpr float sight_margin = 14.0f;
    constexpr float sight_closest = 150.0f;
    constexpr float sight_nearest = 60.0f;
    constexpr float sight_lift_step = 15.0f * 3.14159265358979f / 180.0f;
    constexpr int sight_lift_tries = 4;
    constexpr float lift_ease_seconds = 0.6f;
    // R3 (Free Camera on) cycles the distance: Standard (the game's own, C-Down and the wheel still
    // change it), Far, then Over the Shoulder, and back. Far is this much past the controller's
    // farthest; Over the Shoulder is this near, with the view shifted this far to Conker's right
    // (both the eye and the point it looks at, only while the view is built). The shift glides in
    // and out over shoulder_ease_seconds, and shrinks where a wall is closer on that side.
    constexpr float far_scale = 1.3f;
    constexpr float shoulder_distance = 150.0f;
    constexpr float shoulder_offset = 40.0f;
    constexpr float shoulder_margin = 12.0f;
    constexpr float shoulder_ease_seconds = 0.2f;
    // The ground under the eye: probed straight down from ground_probe_up above it. Where the floor
    // there is within ground_clearance of the eye (or above it), the eye is raised to clear it,
    // rising over ground_rise_seconds and settling back over ground_settle_seconds, instead of
    // going into the ground for the game's collision to shove it out in one frame.
    constexpr float ground_probe_up = 80.0f;
    constexpr float ground_clearance = 20.0f;
    constexpr float ground_rise_seconds = 0.08f;
    constexpr float ground_settle_seconds = 0.4f;
    // The hover floor: the ground Conker stands on (found straight down from the look-at point, so
    // it stays put while he jumps), plus this much. The eye never goes lower, wherever it is.
    constexpr float hover_above_conker_floor = 25.0f;
    constexpr float floor_probe_below = 160.0f;
    // Standard (the "mid" distance): the game's own, brought in by standard_scale, then this far of
    // the way out toward Far (Far's own distance; 0.5: halfway between the two).
    constexpr float standard_scale = 0.85f;
    constexpr float standard_toward_far = 0.5f;
    enum class Distance { Standard, Far, Shoulder };
    // Smoothing (Conker Redux): the camera's own moves glide instead of jumping. Its own turns (round
    // a post, back to where it could see Conker) settle over yaw_settle_seconds (the stick's turns
    // still go at once); rising to look past something over lift_rise_seconds; the line of sight's
    // distance in over sight_in_seconds (never past what's in the way) and out over sight_out_seconds.
    constexpr float yaw_settle_seconds = 0.12f;
    constexpr float lift_rise_seconds = 0.1f;
    constexpr float sight_in_seconds = 0.06f, sight_out_seconds = 0.3f;
    // Camera Follow (Conker Redux): with the stick and mouse left alone for follow_delay seconds, the
    // camera drifts behind the way Conker is going, over follow_seconds at a run (follow_run_speed
    // units a second or more; slower, more gently), not below follow_min_speed, and not when he's
    // going more than follow_max_angle round from where it looks (running at the camera).
    constexpr float follow_delay = 0.9f;
    constexpr float follow_seconds = 1.6f;
    constexpr float follow_run_speed = 250.0f, follow_min_speed = 25.0f;
    constexpr float follow_max_angle = 2.3f;

    // Scroll wheel notches since the view last read them (SDL event watch: the
    // frontend's own event loop consumes the events).
    std::atomic<int> wheel_notches = 0;
    int SDLCALL watch_wheel(void*, SDL_Event* event) {
        if (event->type == SDL_MOUSEWHEEL) {
            wheel_notches.fetch_add(event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event->wheel.y : event->wheel.y);
        }
        return 1;
    }

    struct Orbit {
        bool engaged = false;
        bool follow_camera_ran = false; // func_1512D390 ran since the last view
        bool look_mode_ran = false;     // func_15120158 (hold R, aiming) ran since the last view
        bool turned = false;            // the mouse and wheel were read since the last view
        bool has_target = false;        // target and next_target hold eyes the orbit wanted
        float target[3] = {};           // the eye the orbit wanted last frame
        float next_target[3] = {};      // this frame's, kept as target once the frame ends
        float yaw = 0.0f;               // radians, the eye's direction from the look-at point
        float pitch = 0.0f;
        float wanted = 0.0f;            // the scroll wheel's distance from the look-at point
        double last_frame = 0.0;        // game seconds at the last orbit update
        float stick_speed[2] = {};      // the stick's eased turning, in mouse pixels a second
        bool placed_directly = false;   // this frame's eye was placed at the orbit's target
        float placed[3] = {};           // the eye placed for the collision this frame
        float shown_distance = 0.0f;    // the eye's eased distance from the look-at point (0: none yet)
        float reach = 0.0f;             // the orbit's eased distance (0: none yet)
        float lift = 0.0f;              // radians the eye is raised to see past something
        float shown_yaw = 0.0f;         // the yaw the eye was placed at last frame (has_shown_yaw)
        bool has_shown_yaw = false;
        double last_view = 0.0;         // game seconds at the last view
        Distance distance = Distance::Standard; // R3's choice
        bool r3_was_down = false;
        bool l3_was_down = false;
        bool l3_off = false; // L3 switched the Free Camera off: the mouse leaves the camera alone too
        float shoulder = 0.0f;          // 0 to 1: how far the over-the-shoulder shift has glided in
        float shoulder_room = shoulder_offset; // how far right of Conker is clear this frame
        float ground_lift = 0.0f;       // units the eye is raised off the ground under it
        float view_yaw = 0.0f;          // the yaw drawn: eases toward yaw (the camera's own turns)
        bool has_view_yaw = false;
        float sight_distance = 0.0f;    // the line of sight's distance, eased (0: none yet)
        double last_input = 0.0;        // game seconds when the stick or mouse last turned it
        float last_pivot[2] = {};       // Conker's feet (x, z) last update, for Camera Follow
        bool has_last_pivot = false;
    } orbit;
    // The over-the-shoulder shift applied while the view is built (conker_mouse_camera), undone
    // once it's built (conker_mouse_camera_view_built).
    struct ViewShift {
        bool applied = false;
        gpr camera = 0;
        float offset[3] = {};
        float look[3] = {}, eye[3] = {}; // the unshifted look-at point (+0x2BC) and eye (+0x2F8)
    } view_shift;
    bool was_normal_camera = false; // conker::normal_camera (Camera: Field of View)

    float read_float(uint8_t* rdram, gpr base, int32_t offset) {
        uint32_t word = (uint32_t)MEM_W(offset, base);
        float value;
        std::memcpy(&value, &word, sizeof(value));
        return value;
    }

    void write_float(uint8_t* rdram, gpr base, int32_t offset, float value) {
        uint32_t word;
        std::memcpy(&word, &value, sizeof(word));
        MEM_W(offset, base) = (int32_t)word;
    }
}

// func_1512D390 (the C-buttons' turning), before its last restore: $s0 is the camera.
// Marks that the follow camera is running this frame, and hands the camera back to the
// game while C-left or C-right is held (+0x36C points at the buttons held).
extern "C" void conker_mouse_camera_follow(uint8_t* rdram, recomp_context* ctx) {
    const gpr camera = ctx->r16;
    static const bool probe = std::getenv("CONKER_PROBE") != nullptr;
    static int calls = 0;
    if (probe && calls++ % 60 == 0) {
        std::printf("[freecam] follow camera ran (camera %08X, current %08X)\n", (uint32_t)camera, (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera));
    }
    if ((uint32_t)camera != (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        return;
    }
    orbit.follow_camera_ran = true;
    const gpr buttons = (gpr)(int32_t)MEM_W(0x36C, camera);
    if (((uint32_t)MEM_HU(0, buttons) & 0x3) != 0) {
        orbit.engaged = false;
    }
}

// func_15120158 (the look mode: hold R, and aiming such as the slingshot on a B pad), after
// its first instruction. The mouse aims there (look_aim.cpp), so the orbit leaves the
// camera to it: otherwise both turned with the mouse, and the view ran ahead of the aim.
extern "C" void conker_mouse_camera_look_mode(uint8_t* rdram, recomp_context* ctx) {
    orbit.look_mode_ran = true;
}

extern "C" void func_15044380(uint8_t* rdram, recomp_context* ctx);

namespace {
    // Line of sight: is anything between Conker (the look-at
    // point) and the eye? The game's movement step (func_15044380, which the camera's collision uses
    // to slide a stand-in object from last frame's eye to the new one) slides a stand-in from the
    // look-at point toward the eye; where it stops short (or is pushed aside), something is in the
    // way. Unlike the camera's own collision (which marks the slide as the camera's, D_800CBDD2, and
    // lets it through the barn's big posts), this is an ordinary object's slide, which they stop.
    constexpr int32_t collide_scratch = (int32_t)0x800CBDC0, collide_scratch_size = 0x40;
    constexpr int32_t collide_layers = (int32_t)0x80089120; // 4 bytes: which collision layers count

    // How far along dir (a unit vector) from look the eye can be, up to full.
    float clear_reach(uint8_t* rdram, recomp_context* ctx, gpr camera, const float look[3], const float dir[3], float full) {
        float eye[3];
        for (int i = 0; i < 3; i++) eye[i] = look[i] + dir[i] * full;
        uint32_t scratch_copy[collide_scratch_size / 4], layers_copy;
        for (int i = 0; i < collide_scratch_size / 4; i++) scratch_copy[i] = (uint32_t)MEM_W(4 * i, (gpr)collide_scratch);
        layers_copy = (uint32_t)MEM_W(0, (gpr)collide_layers);
        recomp_context saved = *ctx;
        // The stand-in, built on the stack below the hooked function's frame as the camera's
        // collision builds its own: kind 0x2D, position = where it's going, the camera's sizes.
        const gpr sp = ctx->r29 - 0x400;
        const gpr object = sp + 0x40;
        for (int i = 0; i < 0x340 / 4; i++) MEM_W(4 * i, object) = 0;
        MEM_W(0x0, object) = 0x2D;
        for (int i = 0; i < 3; i++) write_float(rdram, object, 0x14 + i * 4, eye[i]);
        write_float(rdram, object, 0x28, eye[1] - read_float(rdram, camera, 0x354));
        MEM_W(0x40, object) = MEM_W(0x37C, camera);
        MEM_W(0x180, object) = MEM_W(0x354, camera);
        MEM_W(0x188, object) = MEM_W(0x644, camera);
        MEM_W(0x318, object) = (int32_t)camera;
        MEM_B(0, (gpr)(collide_scratch + 0x12)) = 0; // D_800CBDD2: not the camera's slide
        MEM_B(0, (gpr)(collide_scratch + 0x13)) = 0; // D_800CBDD3
        MEM_B(0, (gpr)(collide_scratch + 0x14)) = 0; // D_800CBDD4
        MEM_W(0, (gpr)collide_layers) = 0x01010101;
        ctx->f12.fl = look[0];
        ctx->f14.fl = look[1];
        uint32_t z_bits;
        std::memcpy(&z_bits, &look[2], 4);
        ctx->r6 = (gpr)(int32_t)z_bits;
        ctx->r7 = object;
        MEM_W(0x10, sp) = 0;
        MEM_W(0x14, sp) = 0;
        ctx->r29 = sp;
        func_15044380(rdram, ctx);
        float along = 0.0f, off = 0.0f, stop[3];
        for (int i = 0; i < 3; i++) {
            stop[i] = read_float(rdram, object, 0x14 + i * 4);
            along += (stop[i] - look[i]) * dir[i];
        }
        for (int i = 0; i < 3; i++) {
            const float d = stop[i] - (look[i] + dir[i] * along);
            off += d * d;
        }
        *ctx = saved;
        for (int i = 0; i < collide_scratch_size / 4; i++) MEM_W(4 * i, (gpr)collide_scratch) = (int32_t)scratch_copy[i];
        MEM_W(0, (gpr)collide_layers) = (int32_t)layers_copy;
        // Grazing something (the floor under a low eye, a wall it runs along) slides the stand-in a
        // little aside or short: only a real stop counts, or the eye would flicker between the two.
        if (along >= full - 24.0f) {
            return full;
        }
        if (std::sqrt(off) > 24.0f) {
            along = std::min(along, full - 24.0f); // pushed well aside: a wall along the way
        }
        return std::clamp(along, 0.0f, full);
    }

    // Line of sight, seeing past thin things (ladders, posts, rails), which the see-through fade
    // (update_see_through) shows Conker through: when something's in the way, the same slide from
    // the eye back toward Conker finds its far side; if what's in the way is no thicker than
    // thin_obstacle, it doesn't count. Something seen from one side only (a wall with nothing
    // behind it) or an eye inside something always counts, so the eye can't leave the level.
    // With dodge_obstacles off (the default), nothing between the eye and Conker moves the camera at
    // all: the fade shows him through it, and only the camera's own collision stops the eye. The
    // environment variable CONKER_CAM_DODGE=1 brings the dodging back (testing).
    constexpr float thin_obstacle = 64.0f;
    float sight_reach(uint8_t* rdram, recomp_context* ctx, gpr camera, const float look[3], const float dir[3], float full) {
        static const bool dodge_obstacles = std::getenv("CONKER_CAM_DODGE") != nullptr;
        if (!dodge_obstacles) {
            return full;
        }
        const float along = clear_reach(rdram, ctx, camera, look, dir, full);
        if (along >= full) {
            return full;
        }
        float eye[3], back_dir[3];
        for (int i = 0; i < 3; i++) {
            eye[i] = look[i] + dir[i] * full;
            back_dir[i] = -dir[i];
        }
        const float back = clear_reach(rdram, ctx, camera, eye, back_dir, full);
        if (back > 24.0f && back < full && (full - back) - along <= thin_obstacle) {
            return full;
        }
        return along;
    }
}

// func_1512BB10 (the camera's collision), after its first instruction: $a0 is the camera.
// Places the eye the orbit wants, for the collision to move the camera toward. The game
// calls it a second time in some frames (camera +0x23C set): the mouse and wheel are read
// only the first time, and the second places the same eye.
extern "C" void conker_mouse_camera_collide(uint8_t* rdram, recomp_context* ctx) {
    const gpr camera = ctx->r4;
    if ((uint32_t)camera != (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        return;
    }
    static const bool probe = std::getenv("CONKER_PROBE") != nullptr;
    if (probe) {
        float sx = 0.0f, sy = 0.0f;
        conker::free_camera_stick(&sx, &sy);
        static int logged = 0;
        if ((sx != 0.0f || sy != 0.0f) && logged++ % 30 == 0) {
            std::printf("[freecam] stick %.2f %.2f follow=%d look=%d engaged=%d\n", sx, sy, (int)orbit.follow_camera_ran, (int)orbit.look_mode_ran, (int)orbit.engaged);
        }
    }
    if (!orbit.follow_camera_ran || orbit.look_mode_ran) {
        orbit.engaged = false;
        return;
    }

    float mouse_x = 0.0f, mouse_y = 0.0f;
    int notches = 0;
    bool r3_pressed = false;
    if (!orbit.turned) {
        // L3: the Free Camera off and on again (for this session; the Conker tab's option is
        // what it starts as). Off hands the camera back to the game, as C-Left or C-Right do.
        const bool l3_down = recompinput::get_input_digital(0, recompinput::InputField::controller_digital(SDL_CONTROLLER_BUTTON_LEFTSTICK));
        if (l3_down && !orbit.l3_was_down) {
            const bool on = conker::toggle_free_camera();
            conker::qol::show_free_camera_notice(on);
            orbit.l3_off = !on;
            if (!on) {
                orbit.engaged = false;
            }
        }
        orbit.l3_was_down = l3_down;
        // R3: the next distance (Standard, Far, Over the Shoulder).
        const bool r3_down = conker::free_camera_enabled() &&
            recompinput::get_input_digital(0, recompinput::InputField::controller_digital(SDL_CONTROLLER_BUTTON_RIGHTSTICK));
        if (r3_down && !orbit.r3_was_down) {
            orbit.distance = orbit.distance == Distance::Standard ? Distance::Far
                : orbit.distance == Distance::Far ? Distance::Shoulder : Distance::Standard;
            r3_pressed = true;
        }
        orbit.r3_was_down = r3_down;
        recompinput::get_mouse_deltas(&mouse_x, &mouse_y);
        notches = wheel_notches.exchange(0);
        if (!conker::mouse_turns_camera()) { // Mouse: Turn the Camera off: the mouse only aims
            mouse_x = mouse_y = 0.0f;
            notches = 0;
        }
        if (orbit.l3_off && !conker::free_camera_enabled()) { // the game's own camera, as on the N64
            mouse_x = mouse_y = 0.0f;
            notches = 0;
        }
        orbit.turned = true;
        // The right stick, turned into mouse-like pixels for this frame (game time, so
        // it turns as fast at any frame rate or test speed).
        const double now = conker::testing::game_seconds();
        static double last = now;
        const float seconds = (float)std::clamp(now - last, 0.0, 0.1);
        last = now;
        float stick_x = 0.0f, stick_y = 0.0f;
        conker::free_camera_stick(&stick_x, &stick_y);
        auto curve = [](float v) { return 0.5f * v + 0.5f * v * std::abs(v); };
        const float pixels_per_second = stick_degrees_per_second * conker::camera_turn_speed() / degrees_per_pixel;
        const float wanted_speed[2] = { curve(stick_x) * pixels_per_second, -curve(stick_y) * pixels_per_second }; // pushed up: the view looks up
        const float ease = 1.0f - std::exp(-seconds / stick_ease_seconds);
        for (int i = 0; i < 2; i++) {
            orbit.stick_speed[i] += (wanted_speed[i] - orbit.stick_speed[i]) * ease;
            if (wanted_speed[i] == 0.0f && std::abs(orbit.stick_speed[i]) < 1.0f) {
                orbit.stick_speed[i] = 0.0f;
            }
        }
        mouse_x += orbit.stick_speed[0] * seconds;
        mouse_y += orbit.stick_speed[1] * seconds;
    }

    const float cx = read_float(rdram, camera, 0x2BC);
    const float cy = read_float(rdram, camera, 0x2C0);
    const float cz = read_float(rdram, camera, 0x2C4);
    // The distance the game keeps: its eye's horizontal distance and height from the
    // pivot, measured from the look-at point.
    const float horizontal = read_float(rdram, camera, 0x374);
    const float height = read_float(rdram, camera, 0x344) - (cy - read_float(rdram, camera, 0x2A8));
    const float wanted_distance = std::sqrt(horizontal * horizontal + height * height);

    if (!orbit.engaged) {
        if (mouse_x == 0.0f && mouse_y == 0.0f && notches == 0 && !r3_pressed) {
            return;
        }
        // Take over from where the game's camera was drawn.
        const float ex = read_float(rdram, camera, 0x2EC) - cx;
        const float ey = read_float(rdram, camera, 0x2F0) - cy;
        const float ez = read_float(rdram, camera, 0x2F4) - cz;
        orbit.yaw = std::atan2(ez, ex);
        orbit.pitch = std::atan2(ey, std::sqrt(ex * ex + ez * ez));
        if (orbit.wanted == 0.0f) {
            orbit.wanted = wanted_distance;
        }
        orbit.engaged = true;
    }

    // The controller's nearest and farthest distances from the look-at point.
    const float look_height = cy - read_float(rdram, camera, 0x2A8);
    float nearest = 0.0f, farthest = 0.0f;
    for (int i = 0; i < camera_distance_count; i++) {
        const gpr preset = (gpr)(int32_t)(camera_distances + i * 8);
        const float h = read_float(rdram, preset, 0), v = read_float(rdram, preset, 4) - look_height;
        const float d = std::sqrt(h * h + v * v);
        nearest = (i == 0) ? d : std::min(nearest, d);
        farthest = (i == 0) ? d : std::max(farthest, d);
    }
    orbit.wanted = std::clamp(orbit.wanted * std::pow(zoom_step, (float)-notches), nearest, farthest);
    const float input_yaw = mouse_x * degrees_per_pixel * degrees_to_radians; // the stick's and mouse's turn
    orbit.yaw += input_yaw;

    orbit.pitch = std::clamp(orbit.pitch + mouse_y * degrees_per_pixel * degrees_to_radians, min_pitch, max_pitch);

    // The distance is the player's (the wheel's). The game pulls its own camera in closer than the
    // controller can in tight spots and on ladders (to keep big posts and beams out of the view);
    // with the see-through fade that's not needed, so the orbit keeps its distance. The environment
    // variable CONKER_CAM_GAMEZOOM=1 follows the game's zoom again (testing).
    // The change glides (orbit.reach) rather than jumps.
    static const bool no_game_zoom = std::getenv("CONKER_CAM_GAMEZOOM") == nullptr;
    float chosen = std::max(orbit.wanted * standard_scale, nearest);
    chosen += (farthest * far_scale - chosen) * standard_toward_far;
    if (orbit.distance == Distance::Far) {
        chosen = farthest * far_scale;
    } else if (orbit.distance == Distance::Shoulder) {
        chosen = shoulder_distance;
    }
    const float zoomed = (wanted_distance < nearest && !no_game_zoom) ? std::min(chosen, wanted_distance) : chosen;
    // Not below the ground at Conker's feet: tilted down that far, the eye comes in along the
    // orbit instead, resting just above the floor and looking up at him (pushed into the ground,
    // the game's collision shoved it back up, and aiming it down again every frame shook the view).
    float reach = zoomed;
    // (orbit.reach eases toward this below)
    const float above_floor = cy - (read_float(rdram, camera, 0x2A8) + eye_above_feet);
    const float nearest_reach = std::min(closest, zoomed);
    const float lowest = -std::asin(std::clamp(above_floor / nearest_reach, 0.0f, 1.0f));
    if (orbit.pitch < lowest) {
        orbit.pitch = lowest;
    }
    if (orbit.pitch < 0.0f && above_floor > 0.0f) {
        reach = std::clamp(above_floor / -std::sin(orbit.pitch), nearest_reach, zoomed);
    }
    // Glide to a new distance rather than jump (a game-time ease, so the same at any frame rate).
    const double now = conker::testing::game_seconds();
    const float seconds = (float)std::clamp(now - orbit.last_frame, 0.0, 0.1);
    orbit.last_frame = now;
    if (orbit.reach <= 0.0f) {
        orbit.reach = reach;
    } else {
        orbit.reach += (reach - orbit.reach) * (1.0f - std::exp(-seconds / reach_ease_seconds));
    }
    const float look[3] = { cx, cy, cz };
    auto direction = [](float yaw, float pitch, float out[3]) {
        out[0] = std::cos(pitch) * std::cos(yaw);
        out[1] = std::sin(pitch);
        out[2] = std::cos(pitch) * std::sin(yaw);
    };
    // Line of sight: the eye comes in front of anything between it and
    // Conker; where that would be too near him, it rises to look past it instead. best_view tries
    // the lifts at a yaw and returns the lift to use and the distance it allows.
    auto best_view = [&](float yaw, float* lift_out) {
        float best_along = -1.0f, best_lift = 0.0f;
        for (int k = 0; k <= sight_lift_tries; k++) {
            const float lift = k * sight_lift_step;
            float dir[3];
            direction(yaw, std::min(orbit.pitch + lift, max_pitch), dir);
            const float along = sight_reach(rdram, ctx, camera, look, dir, orbit.reach);
            if (along >= std::min(sight_closest, orbit.reach - 1.0f)) {
                *lift_out = lift;
                return along;
            }
            if (along > best_along) {
                best_along = along;
                best_lift = lift;
            }
        }
        *lift_out = best_lift;
        return best_along;
    };
    // Camera Follow: drift behind the way Conker is going (his feet, +0x2A4/+0x2AC, since the last
    // update), once the stick and mouse have been left alone a moment.
    {
        if (mouse_x != 0.0f || mouse_y != 0.0f || notches != 0) {
            orbit.last_input = now;
        }
        const float px = read_float(rdram, camera, 0x2A4), pz = read_float(rdram, camera, 0x2AC);
        if (orbit.has_last_pivot && seconds > 0.0f && conker::camera_follow_enabled() && now - orbit.last_input > follow_delay) {
            const float vx = (px - orbit.last_pivot[0]) / seconds, vz = (pz - orbit.last_pivot[1]) / seconds;
            const float speed = std::sqrt(vx * vx + vz * vz);
            if (speed > follow_min_speed) {
                constexpr float two_pi = 2.0f * 3.14159265358979f;
                const float behind = std::atan2(-vz, -vx); // the eye's direction from him, behind his way
                const float turn = std::remainder(behind - orbit.yaw, two_pi);
                if (std::abs(turn) < follow_max_angle) {
                    const float strength = std::min(speed / follow_run_speed, 1.0f);
                    orbit.yaw += turn * strength * (1.0f - std::exp(-seconds / follow_seconds));
                }
            }
        }
        orbit.last_pivot[0] = px;
        orbit.last_pivot[1] = pz;
        orbit.has_last_pivot = true;
    }

    float needed_lift = 0.0f;
    float view = best_view(orbit.yaw, &needed_lift);
    const bool good = view >= std::min(sight_closest, orbit.reach - 1.0f);
    // Where it can't see him from far enough even raised (a post right beside him, turned toward or
    // come to as he moves), the camera turns the least it can to where it can, either way, as it
    // would slide along a wall, rather than squeeze in behind him.
    if (!good) {
        static const float offsets[] = { 4.0f, 8.0f, 14.0f, 22.0f, 32.0f, 45.0f };
        bool found = false;
        for (float offset : offsets) {
            for (float side : { 1.0f, -1.0f }) {
                // The side the player last turned from first: back where it came from.
                const float sign = (mouse_x > 0.0f) ? -side : side;
                const float yaw = orbit.yaw + sign * offset * degrees_to_radians;
                float lift = 0.0f;
                if (best_view(yaw, &lift) >= std::min(sight_closest, orbit.reach - 1.0f)) {
                    orbit.yaw = yaw;
                    needed_lift = lift;
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        // Nowhere near: stay where it was last frame (it could see him there) rather than dive in.
        if (!found && orbit.has_shown_yaw) {
            float lift = 0.0f;
            if (best_view(orbit.shown_yaw, &lift) >= std::min(sight_closest, orbit.reach - 1.0f)) {
                orbit.yaw = orbit.shown_yaw;
                needed_lift = lift;
            }
        }
    }
    orbit.shown_yaw = orbit.yaw;
    orbit.has_shown_yaw = true;
    const float lift_seconds = needed_lift >= orbit.lift ? lift_rise_seconds : lift_ease_seconds;
    orbit.lift += (needed_lift - orbit.lift) * (1.0f - std::exp(-seconds / lift_seconds));
    // The yaw drawn: the stick's turn at once, the camera's own turns gliding.
    if (!orbit.has_view_yaw) {
        orbit.view_yaw = orbit.yaw;
        orbit.has_view_yaw = true;
    } else {
        constexpr float two_pi = 2.0f * 3.14159265358979f;
        orbit.view_yaw += input_yaw;
        orbit.view_yaw += std::remainder(orbit.yaw - orbit.view_yaw, two_pi) * (1.0f - std::exp(-seconds / yaw_settle_seconds));
    }
    float dir[3];
    direction(orbit.view_yaw, std::min(orbit.pitch + orbit.lift, max_pitch), dir);
    float along = sight_reach(rdram, ctx, camera, look, dir, orbit.reach);
    if (along < std::min(sight_closest, orbit.reach - 1.0f) && orbit.lift > needed_lift) {
        // Easing back down, the angle on the way can catch something (a beam above): the one found
        // clear instead.
        orbit.lift = needed_lift;
        direction(orbit.view_yaw, std::min(orbit.pitch + orbit.lift, max_pitch), dir);
        along = sight_reach(rdram, ctx, camera, look, dir, orbit.reach);
    }
    const float sight = (along >= orbit.reach) ? orbit.reach : std::clamp(along - sight_margin, std::min(sight_nearest, orbit.reach), orbit.reach);
    // Glide to it: in quickly, out slowly, and never past what's in the way.
    if (orbit.sight_distance <= 0.0f) {
        orbit.sight_distance = sight;
    } else {
        const float sight_seconds = sight < orbit.sight_distance ? sight_in_seconds : sight_out_seconds;
        orbit.sight_distance += (sight - orbit.sight_distance) * (1.0f - std::exp(-seconds / sight_seconds));
    }
    if (along < orbit.reach) {
        orbit.sight_distance = std::min(orbit.sight_distance, along);
    }
    const float distance = orbit.sight_distance;
    float target[3];
    for (int i = 0; i < 3; i++) target[i] = look[i] + dir[i] * distance;

    // The ground under the eye: raise it to clear the floor there, gliding, rather than leave it in
    // the ground for the collision to shove out. A probe stopped where it starts is inside something
    // (a beam or ceiling just above the eye), so it tells nothing about the floor.
    {
        const float probe_from[3] = { target[0], target[1] + ground_probe_up, target[2] };
        const float down[3] = { 0.0f, -1.0f, 0.0f };
        const float probe_full = ground_probe_up + ground_clearance + 24.0f; // clear_reach ignores the last 24
        const float to_floor = clear_reach(rdram, ctx, camera, probe_from, down, probe_full);
        float lift_needed = 0.0f;
        if (to_floor < probe_full && to_floor > 1.0f) {
            lift_needed = std::max(0.0f, (probe_from[1] - to_floor) + ground_clearance - target[1]);
        }
        // The hover floor: not below the ground Conker is on (over a pit, nothing is found: no floor).
        const float under_full = (cy - read_float(rdram, camera, 0x2A8)) + floor_probe_below;
        const float to_conker_floor = clear_reach(rdram, ctx, camera, look, down, under_full);
        if (to_conker_floor < under_full && to_conker_floor > 1.0f) {
            const float hover = (cy - to_conker_floor) + hover_above_conker_floor;
            lift_needed = std::max(lift_needed, hover - target[1]);
        }
        const float ease = lift_needed > orbit.ground_lift ? ground_rise_seconds : ground_settle_seconds;
        orbit.ground_lift += (lift_needed - orbit.ground_lift) * (1.0f - std::exp(-seconds / ease));
        target[1] += orbit.ground_lift;
    }

    // Over the shoulder: glide the shift in or out, and find how far right of Conker is clear.
    const float shoulder_wanted = orbit.distance == Distance::Shoulder ? 1.0f : 0.0f;
    orbit.shoulder += (shoulder_wanted - orbit.shoulder) * (1.0f - std::exp(-seconds / shoulder_ease_seconds));
    if (orbit.shoulder < 0.001f) {
        orbit.shoulder = 0.0f;
    } else {
        const float right[3] = { std::sin(orbit.yaw), 0.0f, -std::cos(orbit.yaw) };
        const float full = shoulder_offset + shoulder_margin + 24.0f;
        orbit.shoulder_room = std::max(0.0f, clear_reach(rdram, ctx, camera, look, right, full) - shoulder_margin);
    }
    static const bool trace_sight = std::getenv("CONKER_CAM_TRACE") != nullptr;
    if (trace_sight) {
        std::printf("[sight] t=%.3f reach %.1f along %.1f lift %.1f deg distance %.1f\n", now, orbit.reach, along, orbit.lift * 57.2958f, distance);
    }
    // Placed directly: the game's collision (which would slide the camera from last frame's eye,
    // a few units a frame, and lets it through the big posts) is given nothing to move.
    for (int i = 0; i < 3; i++) {
        write_float(rdram, camera, 0x304 + i * 4, target[i]);
        write_float(rdram, camera, 0x2F8 + i * 4, target[i]);
        orbit.next_target[i] = target[i];
        orbit.placed[i] = target[i];
    }
    orbit.placed_directly = true;
}

// func_151284C4 (builds the view), after its first instruction: $a0 is the camera. The
// frame's camera update is done: start over for the next one.
extern "C" void conker_mouse_camera(uint8_t* rdram, recomp_context* ctx) {
    const gpr camera = ctx->r4;
    if ((uint32_t)camera != (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        return;
    }
    was_normal_camera = orbit.follow_camera_ran && !orbit.look_mode_ran;
    if (!orbit.follow_camera_ran || orbit.look_mode_ran) {
        orbit.engaged = false;
    }
    // Sloping ground the collision pushed the eye up from (+0x2F8, where it left the eye):
    // the orbit rests there, at that angle, instead of aiming into the ground again next frame.
    if (orbit.engaged && orbit.placed_directly && read_float(rdram, camera, 0x2F8 + 4) > orbit.placed[1] + ground_push) {
        const float ex = read_float(rdram, camera, 0x2F8) - read_float(rdram, camera, 0x2BC);
        const float ey = read_float(rdram, camera, 0x2FC) - read_float(rdram, camera, 0x2C0);
        const float ez = read_float(rdram, camera, 0x300) - read_float(rdram, camera, 0x2C4);
        orbit.pitch = std::clamp(std::max(orbit.pitch, std::atan2(ey, std::sqrt(ex * ex + ez * ez))), min_pitch, max_pitch);
    }
    // Walls and tight spots: the collision left the eye at +0x2F8. Nearer than the view was: go
    // there at once. Further: ease back out. Along the same line from the look-at point.
    if (orbit.engaged) {
        const double now = conker::testing::game_seconds();
        const float seconds = (float)std::clamp(now - orbit.last_view, 0.0, 0.1);
        orbit.last_view = now;
        float look[3], eye[3], distance = 0.0f;
        for (int i = 0; i < 3; i++) {
            look[i] = read_float(rdram, camera, 0x2BC + i * 4);
            eye[i] = read_float(rdram, camera, 0x2F8 + i * 4) - look[i];
            distance += eye[i] * eye[i];
        }
        distance = std::sqrt(distance);
        if (orbit.shown_distance <= 0.0f || distance <= orbit.shown_distance) {
            orbit.shown_distance = distance;
        } else {
            orbit.shown_distance += (distance - orbit.shown_distance) * (1.0f - std::exp(-seconds / ease_out_seconds));
            if (distance > 0.0f) {
                for (int i = 0; i < 3; i++) {
                    write_float(rdram, camera, 0x2F8 + i * 4, look[i] + eye[i] * (orbit.shown_distance / distance));
                }
            }
        }
    } else {
        orbit.shown_distance = 0.0f;
    }
    // Over the shoulder: the eye and the point it looks at move together to Conker's right (so the
    // view keeps its direction, and Conker's movement with it) while func_1512C490, called just after
    // this hook, builds the view from them. conker_mouse_camera_view_built puts them back.
    view_shift.applied = false;
    if (orbit.engaged && orbit.shoulder > 0.0f) {
        float look[3], eye[3];
        for (int i = 0; i < 3; i++) {
            look[i] = read_float(rdram, camera, 0x2BC + i * 4);
            eye[i] = read_float(rdram, camera, 0x2F8 + i * 4);
        }
        // Right of the view: the eye's direction from the look-at point, turned a quarter to the right.
        const float ex = eye[0] - look[0], ez = eye[2] - look[2];
        const float across = std::sqrt(ex * ex + ez * ez);
        const float amount = std::min(shoulder_offset * orbit.shoulder, orbit.shoulder_room);
        if (across > 1.0f && amount > 0.0f) {
            view_shift.applied = true;
            view_shift.camera = camera;
            view_shift.offset[0] = ez / across * amount;
            view_shift.offset[1] = 0.0f;
            view_shift.offset[2] = -ex / across * amount;
            for (int i = 0; i < 3; i++) {
                view_shift.look[i] = look[i];
                view_shift.eye[i] = eye[i];
                write_float(rdram, camera, 0x2BC + i * 4, look[i] + view_shift.offset[i]);
                write_float(rdram, camera, 0x2F8 + i * 4, eye[i] + view_shift.offset[i]);
            }
        }
    }
    static const bool trace = std::getenv("CONKER_CAM_TRACE") != nullptr;
    if (trace && orbit.engaged) {
        std::printf("[cam] t=%.3f target %.1f %.1f %.1f placed %.1f %.1f %.1f collided %.1f %.1f %.1f look %.1f %.1f %.1f shown %.1f direct %d\n",
            conker::testing::game_seconds(), orbit.next_target[0], orbit.next_target[1], orbit.next_target[2],
            orbit.placed[0], orbit.placed[1], orbit.placed[2],
            read_float(rdram, camera, 0x2F8), read_float(rdram, camera, 0x2FC), read_float(rdram, camera, 0x300),
            read_float(rdram, camera, 0x2BC), read_float(rdram, camera, 0x2C0), read_float(rdram, camera, 0x2C4),
            orbit.shown_distance, (int)orbit.placed_directly);
    }
    orbit.placed_directly = false;
    // The eye wanted this frame, to tell next frame how far the orbit itself moved.
    orbit.has_target = orbit.engaged;
    std::memcpy(orbit.target, orbit.next_target, sizeof(orbit.target));
    orbit.follow_camera_ran = false;
    orbit.look_mode_ran = false;
    orbit.turned = false;
    if (!orbit.engaged) {
        orbit.reach = 0.0f;
        orbit.lift = 0.0f;
        orbit.has_shown_yaw = false;
        orbit.shoulder = 0.0f;
        orbit.ground_lift = 0.0f;
        orbit.has_view_yaw = false;
        orbit.sight_distance = 0.0f;
        orbit.has_last_pivot = false;
    }
}

// See-through: whatever stands between the camera and Conker is faded out with a dither pattern
// (RT64's pixel shader, RasterPS.hlsl, patches/rt64_conker.patch), in a soft-edged circle on the
// screen around him, centred between his feet (+0x2A4) and the look-at point above them and not
// below his feet (the floor he stands on), and only for what's
// nearer the camera than he is by see_through_margin (more faded the nearer, up to
// see_through_strength). Only in the normal camera. Here, as the view is built: where both ends
// land on screen, worked out the way the game draws them, from the eye it drew from (+0x2EC)
// toward the look-at point (+0x2BC) with the world's up as up, through player 1's projection
// (guPerspectiveF's matrix, D_800BE628's camera + 0xBC: x and y scaled by [0][0] and [1][1], w
// the distance along the view times the matrix's scale, -[2][3], times the view matrix's own
// scale: the game's view (D_800D9C10, player 1's, as func_1510B690 copies it) shrinks the world,
// read from the length of its first column). RT64 widens the view for the
// window itself, so the shader squeezes x by 4:3 over the window's shape.
extern "C" volatile float rt64_conker_see_through[8]; // RT64, rt64_framebuffer_renderer.cpp
namespace {
    constexpr uint32_t projection_cameras = 0x800BE628; // D_800BE628, 0x180-byte cameras
    constexpr uint32_t view_matrices = 0x800D9C10;      // D_800D9C10, a float 4x4 per player
    constexpr float see_through_margin = 60.0f;   // units nearer than Conker before fading starts (clears his tail)
    constexpr float see_through_ramp = 50.0f;     // units over which it fades in fully
    constexpr float see_through_radius = 75.0f;   // the circle's radius around him, in units
    constexpr float see_through_strength = 0.85f; // the most of it faded (1: gone)
    // RT64 keeps the last values it was given: when the normal view hasn't been built for this long
    // (cutscenes and other cameras that skip it), see_through_on_vi turns the fade off.
    constexpr double see_through_stale_seconds = 0.1;
    std::atomic<int64_t> see_through_updated{ 0 }; // steady_clock ticks at the last update

    int64_t steady_ticks() {
        return std::chrono::steady_clock::now().time_since_epoch().count();
    }

    void update_see_through(uint8_t* rdram) {
        volatile float* out = rt64_conker_see_through;
        const gpr camera = (gpr)(int32_t)MEM_W(0, (gpr)(int32_t)current_camera);
        const uint32_t projections = (uint32_t)MEM_W(0, (gpr)(int32_t)projection_cameras);
        see_through_updated = steady_ticks();
        // Never in cutscenes, even those that keep the follow camera (conker::qol::cutscene_playing).
        if (!was_normal_camera || conker::qol::cutscene_playing() || (uint32_t)camera == 0 || projections == 0) {
            out[7] = 0.0f;
            return;
        }
        const gpr projection = (gpr)(int32_t)(projections + 0xBC);
        const float sx = read_float(rdram, projection, 0x00);
        const float sy = read_float(rdram, projection, 0x14);
        const gpr view = (gpr)(int32_t)view_matrices;
        const float view_scale = std::sqrt(read_float(rdram, view, 0x00) * read_float(rdram, view, 0x00) +
            read_float(rdram, view, 0x10) * read_float(rdram, view, 0x10) + read_float(rdram, view, 0x20) * read_float(rdram, view, 0x20));
        const float scale = -read_float(rdram, projection, 0x2C);
        const float depth_scale = scale * view_scale; // w per unit along the view
        float eye[3], f[3], ends[2][3];
        for (int i = 0; i < 3; i++) {
            eye[i] = read_float(rdram, camera, 0x2EC + i * 4);
            f[i] = read_float(rdram, camera, 0x2BC + i * 4) - eye[i];
            ends[0][i] = read_float(rdram, camera, 0x2A4 + i * 4);
            // The look-at point Conker's under: without the over-the-shoulder shift.
            ends[1][i] = read_float(rdram, camera, 0x2BC + i * 4) - (view_shift.applied ? view_shift.offset[i] : 0.0f);
        }
        const float f_length = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (!(f_length > 1.0f) || !(scale > 0.0f) || !(view_scale > 0.0f) || !(sx > 0.0f) || !(sy > 0.0f)) {
            out[7] = 0.0f;
            return;
        }
        for (float& v : f) {
            v /= f_length;
        }
        // right = forward x up (0, 1, 0); up = right x forward.
        float r[3] = { -f[2], 0.0f, f[0] };
        const float r_length = std::sqrt(r[0] * r[0] + r[2] * r[2]);
        if (!(r_length > 1e-3f)) {
            out[7] = 0.0f;
            return;
        }
        r[0] /= r_length;
        r[2] /= r_length;
        const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
        float screen[2][2], depth = 1e30f;
        for (int e = 0; e < 2; e++) {
            const float d[3] = { ends[e][0] - eye[0], ends[e][1] - eye[1], ends[e][2] - eye[2] };
            const float along = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
            if (!(along > see_through_margin)) {
                out[7] = 0.0f;
                return;
            }
            const float w = along * scale;
            screen[e][0] = sx * (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / w;
            screen[e][1] = sy * (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) / w;
            depth = std::min(depth, along);
        }
        out[0] = screen[0][0];
        out[1] = screen[0][1];
        out[2] = screen[1][0];
        out[3] = screen[1][1];
        out[4] = (depth - see_through_margin) * depth_scale;   // fading starts nearer than this w
        out[5] = see_through_ramp * depth_scale;                // and is full this much nearer
        out[6] = sy * see_through_radius / (depth * scale);     // radius, in the view's height (-1 to 1)
        out[7] = see_through_strength;
        static const bool debug = std::getenv("CONKER_SEE_THROUGH_DEBUG") != nullptr;
        static int frames = 0;
        if (debug && frames++ % 60 == 0) {
            std::printf("[see-through] feet %.2f,%.2f top %.2f,%.2f depth %.1f scale %.3f view %.4f w %.2f radius %.3f\n",
                out[0], out[1], out[2], out[3], depth, scale, view_scale, out[4], out[6]);
        }
    }
}

// func_151284C4, once func_1512C490 has built the view: undo the over-the-shoulder shift, so the
// game's camera (and next frame's update) carries on from where it really is. The view copied the
// eye into the one it drew from (+0x2EC); that's put back too where it holds the shifted eye.
extern "C" void conker_mouse_camera_view_built(uint8_t* rdram, recomp_context* ctx) {
    update_see_through(rdram);
    if (!view_shift.applied) {
        return;
    }
    view_shift.applied = false;
    const gpr camera = view_shift.camera;
    for (int i = 0; i < 3; i++) {
        if (read_float(rdram, camera, 0x2EC + i * 4) == view_shift.eye[i] + view_shift.offset[i]) {
            write_float(rdram, camera, 0x2EC + i * 4, view_shift.eye[i]);
        }
        write_float(rdram, camera, 0x2BC + i * 4, view_shift.look[i]);
        write_float(rdram, camera, 0x2F8 + i * 4, view_shift.eye[i]);
    }
}

void conker::see_through_on_vi() {
    const auto since = std::chrono::steady_clock::duration(steady_ticks() - see_through_updated.load());
    if (std::chrono::duration<double>(since).count() > see_through_stale_seconds) {
        rt64_conker_see_through[7] = 0.0f;
    }
}

bool conker::normal_camera() {
    return was_normal_camera;
}

// frontend.cpp, once SDL is up: listen for the scroll wheel.
void conker_mouse_camera_init() {
    SDL_AddEventWatch(watch_wheel, nullptr);
}

// The right stick for the Free Camera, x (right) and y (up), -1 to 1 past the dead zone;
// zero when the option is off. Test runs can drive it (testing.cpp).
void conker::free_camera_stick(float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
    float test_x = 0.0f, test_y = 0.0f;
    if (conker::testing::right_stick(&test_x, &test_y)) {
        *x = test_x;
        *y = test_y;
        return;
    }
    if (!conker::free_camera_enabled()) {
        return;
    }
    using recompinput::InputField;
    const float sx = recompinput::get_input_analog(0, InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTX, true)) -
        recompinput::get_input_analog(0, InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTX, false));
    const float sy = recompinput::get_input_analog(0, InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTY, false)) -
        recompinput::get_input_analog(0, InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTY, true));
    auto shape = [](float v) {
        const float a = std::abs(v);
        return a < stick_dead_zone ? 0.0f : std::copysign((a - stick_dead_zone) / (1.0f - stick_dead_zone), v);
    };
    *x = shape(sx);
    *y = shape(sy);
    if (conker::camera_inverted()) {
        *x = -*x;
    }
    if (conker::camera_tilt_inverted()) {
        *y = -*y;
    }
}
