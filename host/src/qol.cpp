// Quality-of-life options (Conker tab):
//
// - Autosave Icon: Conker's head, pulsing and rocking, in the bottom-right corner while the game saves
//   (the save file in the settings folder changes; the runtime writes it as the game writes its
//   EEPROM, at checkpoints and on the save menu).
// - Pause When Unfocused: while the window doesn't have the keyboard (alt-tab, another window
//   clicked), the game holds still: the VI callback waits, so game time, sound and input stop.
// - Skip Any Cutscene: holding L skips any cutscene, even the first time (the game only lets watched
//   ones be skipped), including the ones it never lets you skip (the opening, the hangover scene), as
//   CBFD-Recompiled's Skip Any Cutscene mod allows (MIT). Held, not pressed, so it can't happen by
//   accident: "Hold to Skip" shows in the corner with a ring filling as L is held. A hook where
//   func_1501E05C (the game's "skip the playing cutscene?" check) returns, recomp/conker.toml.
// - Reduce Motion Effects: no motion blur (the drunk ghosting at the start of the game), for players
//   who get motion sick.
// - Always Show Health: health (chocolate) stays on screen instead of sliding away.
// - Longer Tail Spin: the spin after a jump (A twice) floats and glides for longer.
// - Toggle R-Look and Toggle Crouch: a press of R or Z holds it until the next press, for players
//   who can't hold a button down.
// - The Free Camera notice (Conker Redux): when L3 switches the Free Camera (free_camera.cpp),
//   FREECAM ON or FREECAM OFF shows in the top-left corner for notice_seconds, then fades out. The
//   words are the user's art (host/art/redux_freecam.png -> tools/redux_menu_art.py).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <thread>

#include <SDL.h>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "recompui/recompui.h"
#include "elements/ui_element.h"
#include "elements/ui_image.h"
#include "elements/ui_label.h"
#include "util/file.h"

#include "conker.hpp"

extern std::atomic_bool exited; // librecomp's recomp.cpp: set by ultramodern::quit

namespace {
    using clock = std::chrono::steady_clock;

    // Pause When Unfocused.
    std::atomic<bool> window_focused{ true };


    // Testing aid, only with a file named capture_enabled in the settings folder: F9 saves a snapshot
    // of the game's memory there (memory_<n>.rdram), to find things only a real moment of play shows
    // (e.g. the cash display's timer for Always Show HUD).
    std::atomic<int> capture_requests{ 0 };

    int SDLCALL watch_focus(void*, SDL_Event* event) {
        if (event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_SCANCODE_F9 && !event->key.repeat) {
            capture_requests++;
        }
        if (event->type == SDL_WINDOWEVENT) {
            if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                window_focused = true;
            } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                window_focused = false;
            }
        }
        return 1;
    }

    // Test runs play in the background, without the keyboard.
    bool test_run() {
        static const bool testing = std::getenv("CONKER_SNAP_AT") != nullptr || std::getenv("CONKER_INPUT_SCRIPT") != nullptr;
        return testing;
    }

    // Autosave Icon.
    constexpr auto icon_time = std::chrono::milliseconds(2000);
    constexpr auto check_every = std::chrono::milliseconds(250);
    std::atomic<bool> ui_ready = false;
    bool icon_created = false;
    recompui::ContextId icon_context = recompui::ContextId::null();
    clock::time_point last_check{}, shown_until{};
    std::filesystem::file_time_type last_save{};
    bool have_last_save = false;

    // The newest save file's time (the runtime keeps them in the settings folder's saves).
    bool newest_save(std::filesystem::file_time_type& time) {
        std::error_code ec;
        const std::filesystem::path folder = recomp::get_config_path() / "saves";
        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".bin") {
                continue;
            }
            const auto t = entry.last_write_time(ec);
            if (!ec && (!found || t > time)) {
                time = t;
                found = true;
            }
        }
        return found;
    }

    recompui::Image* icon_image = nullptr;

    void create_icon() {
        std::ifstream file(recompui::file::get_asset_path("save_icon.png"), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        icon_context = recompui::create_context();
        icon_context.open();
        icon_context.set_captures_input(false);
        icon_context.set_captures_mouse(false);
        if (!bytes.empty()) {
            recompui::queue_image_from_bytes_file("?/conker/save_icon", bytes);
            icon_image = icon_context.create_element<recompui::Image>(icon_context.get_root_element(), "?/conker/save_icon");
            icon_image->set_position(recompui::Position::Absolute);
            icon_image->set_right(30.0f);
            icon_image->set_bottom(30.0f);
            icon_image->set_width(64.0f);
            icon_image->set_height(64.0f);
        }
        icon_context.close();
        icon_created = true;
    }

    // Skip Any Cutscene's hold.
    constexpr uint16_t button_l = 0x0020, button_start = 0x1000;
    constexpr double hold_seconds = 1.2;
    std::atomic<uint16_t> player_buttons{ 0 };     // player 1's own buttons (before the toggles)
    std::atomic<int64_t> cutscene_seen{ 0 };       // the check last ran (a cutscene is playing)
    std::atomic<int64_t> hold_started{ 0 };        // 0: not holding
    std::atomic<bool> wait_for_release{ false };   // after a skip, until L is let go
    std::atomic<float> hold_progress{ 0.0f };

    int64_t ticks() { return clock::now().time_since_epoch().count(); }
    double seconds_since(int64_t t) { return std::chrono::duration<double>(clock::now() - clock::time_point(clock::duration(t))).count(); }

    constexpr int ring_dots = 24;
    constexpr float ring_radius = 28.0f, ring_right = 66.0f, ring_bottom = 66.0f, dot_size = 8.0f;
    bool skip_created = false;
    recompui::ContextId skip_context = recompui::ContextId::null();
    recompui::Element* dots[ring_dots] = {};
    int lit_shown = -1;
    const recompui::Color dot_off{ 0xFF, 0xF4, 0xD6, 0x50 }, dot_on{ 0xF0, 0x7A, 0x1E, 0xFF };

    void create_skip() {
        skip_context = recompui::create_context();
        skip_context.open();
        skip_context.set_captures_input(false);
        skip_context.set_captures_mouse(false);
        recompui::Element* root = skip_context.get_root_element();
        for (int k = 0; k < ring_dots; k++) {
            const float a = (-90.0f + k * 360.0f / ring_dots) * 3.14159265f / 180.0f; // from the top, clockwise
            recompui::Element* d = skip_context.create_element<recompui::Element>(root);
            d->set_position(recompui::Position::Absolute);
            d->set_right(ring_right - ring_radius * std::cos(a) - dot_size / 2);
            d->set_bottom(ring_bottom - ring_radius * std::sin(a) - dot_size / 2);
            d->set_width(dot_size);
            d->set_height(dot_size);
            d->set_border_radius(dot_size / 2);
            d->set_background_color(dot_off);
            dots[k] = d;
        }
        // Conker Redux: Conker's head (the autosave icon's) inside the ring, in place of an "L".
        std::ifstream file(recompui::file::get_asset_path("save_icon.png"), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (!bytes.empty()) {
            constexpr float head = 2.0f * ring_radius - dot_size - 4.0f;
            recompui::queue_image_from_bytes_file("?/conker/skip_icon", bytes);
            recompui::Image* face = skip_context.create_element<recompui::Image>(root, "?/conker/skip_icon");
            face->set_position(recompui::Position::Absolute);
            face->set_right(ring_right - head / 2);
            face->set_bottom(ring_bottom - head / 2);
            face->set_width(head);
            face->set_height(head);
        }
        recompui::Label* text = skip_context.create_element<recompui::Label>(root, "Hold L to Skip", recompui::LabelStyle::Large);
        text->set_position(recompui::Position::Absolute);
        text->set_right(ring_right + ring_radius + 18.0f);
        text->set_bottom(ring_bottom - 16.0f);
        text->set_color({ 0xFF, 0xE9, 0xC2, 0xFF });
        skip_context.close();
        skip_created = true;
    }

    void update_skip() {
        const bool cutscene = seconds_since(cutscene_seen.load()) < 0.25;
        const bool holding = (player_buttons.load() & button_l) != 0 && !wait_for_release.load();
        const bool wanted = ui_ready && conker::qol::skip_any_cutscene() && cutscene && holding;
        if (!wanted) {
            if (skip_created && recompui::is_context_shown(skip_context)) {
                recompui::hide_context(skip_context);
            }
            return;
        }
        if (!skip_created) {
            create_skip();
        }
        if (!recompui::is_context_shown(skip_context)) {
            recompui::show_context(skip_context, "");
            lit_shown = -1;
        }
        const int lit = std::clamp((int)std::lround(hold_progress.load() * ring_dots), 0, ring_dots);
        if (lit != lit_shown) {
            skip_context.open();
            for (int k = 0; k < ring_dots; k++) {
                dots[k]->set_background_color(k < lit ? dot_on : dot_off);
            }
            skip_context.close();
            lit_shown = lit;
        }
    }

    // The Free Camera notice.
    constexpr double notice_seconds = 2.0, notice_fade_seconds = 0.4;
    constexpr float notice_left = 30.0f, notice_top = 26.0f, notice_gap = 10.0f;
    constexpr float notice_scale = 34.0f / 120.0f; // the pictures' pixels to the UI's (FREECAM 34 high)
    std::atomic<int64_t> notice_at{ 0 };  // ticks when it was asked for (0: never)
    std::atomic<bool> notice_on{ false };
    bool notice_created = false;
    recompui::ContextId notice_context = recompui::ContextId::null();
    recompui::Element* notice_box = nullptr;
    recompui::Image* notice_words[2] = {}; // OFF, ON
    int notice_shown = -1;

    // A PNG's width and height (from its header), 0 if it isn't one.
    void png_size(const std::vector<char>& bytes, float* w, float* h) {
        auto be32 = [&](size_t at) {
            return ((uint32_t)(uint8_t)bytes[at] << 24) | ((uint32_t)(uint8_t)bytes[at + 1] << 16) |
                ((uint32_t)(uint8_t)bytes[at + 2] << 8) | (uint32_t)(uint8_t)bytes[at + 3];
        };
        *w = *h = 0.0f;
        if (bytes.size() >= 24) {
            *w = (float)be32(16);
            *h = (float)be32(20);
        }
    }

    // Loads assets/redux_menu/<name>.png as an image at (left, top) in the notice, its own pixels
    // scaled by notice_scale; out: its size. Null if it's missing.
    recompui::Image* notice_image(const char* name, float left, float top, float* w, float* h) {
        std::ifstream file(recompui::file::get_asset_path((std::string("redux_menu/") + name + ".png").c_str()), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        png_size(bytes, w, h);
        *w *= notice_scale;
        *h *= notice_scale;
        if (bytes.empty() || *w <= 0.0f) {
            return nullptr;
        }
        const std::string src = std::string("?/redux/notice_") + name;
        recompui::queue_image_from_bytes_file(src, bytes);
        recompui::Image* image = notice_context.create_element<recompui::Image>(notice_box, src);
        image->set_position(recompui::Position::Absolute);
        image->set_left(left);
        image->set_top(top);
        image->set_width(*w);
        image->set_height(*h);
        return image;
    }

    void create_notice() {
        notice_context = recompui::create_context();
        notice_context.open();
        notice_context.set_captures_input(false);
        notice_context.set_captures_mouse(false);
        notice_box = notice_context.create_element<recompui::Element>(notice_context.get_root_element());
        notice_box->set_position(recompui::Position::Absolute);
        notice_box->set_left(0.0f);
        notice_box->set_top(0.0f);
        float fw = 0.0f, fh = 0.0f;
        notice_image("freecam", notice_left, notice_top, &fw, &fh);
        const char* names[2] = { "freecam_off", "freecam_on" };
        for (int k = 0; k < 2; k++) {
            // Beside FREECAM, level with its middle (its letters dip and rise).
            float w = 0.0f, h = 0.0f;
            notice_words[k] = notice_image(names[k], notice_left + fw + notice_gap, notice_top, &w, &h);
            if (notice_words[k] != nullptr) {
                notice_words[k]->set_top(notice_top + (fh - h) * 0.5f);
            }
        }
        notice_context.close();
        notice_created = true;
    }

    void update_notice() {
        const int64_t at = notice_at.load();
        const double since = at == 0 ? 1e9 : std::chrono::duration<double>(clock::now() - clock::time_point(clock::duration(at))).count();
        const bool wanted = ui_ready && since < notice_seconds + notice_fade_seconds;
        if (!wanted) {
            if (notice_created && recompui::is_context_shown(notice_context)) {
                recompui::hide_context(notice_context);
            }
            return;
        }
        if (!notice_created) {
            create_notice();
        }
        if (!recompui::is_context_shown(notice_context)) {
            recompui::show_context(notice_context, "");
            notice_shown = -1;
        }
        const int on = notice_on.load() ? 1 : 0;
        const float opacity = since < notice_seconds ? 1.0f : (float)std::clamp(1.0 - (since - notice_seconds) / notice_fade_seconds, 0.0, 1.0);
        notice_context.open();
        if (on != notice_shown) {
            for (int k = 0; k < 2; k++) {
                if (notice_words[k] != nullptr) {
                    notice_words[k]->set_opacity(k == on ? 1.0f : 0.0f);
                }
            }
            notice_shown = on;
        }
        notice_box->set_opacity(opacity);
        notice_context.close();
    }

    // Toggle R-Look / Toggle Crouch.
    struct Toggle {
        bool was_down = false;
        bool held = false;
        uint16_t apply(uint16_t buttons, uint16_t button, bool enabled) {
            const bool down = (buttons & button) != 0;
            if (!enabled) {
                held = false;
                was_down = down;
                return buttons;
            }
            if (down && !was_down) {
                held = !held;
            }
            was_down = down;
            return held ? (buttons | button) : (buttons & (uint16_t)~button);
        }
    };
    Toggle toggle_r, toggle_z;
}

void conker::qol::init() {
    SDL_AddEventWatch(watch_focus, nullptr);
}

void conker::qol::on_ui_ready() {
    ui_ready = true;
}

void conker::qol::on_vi() {
    if (!conker::qol::pause_unfocused() || test_run()) {
        return;
    }
    // Hold the game still until the window has the keyboard again, or the game is closing (else
    // quitting while unfocused, e.g. from the taskbar, would wait here forever).
    while (!window_focused.load() && !exited.load() && ultramodern::is_game_started() && conker::qol::pause_unfocused()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void conker::qol::update() {
    update_skip();
    update_notice();
    const clock::time_point now = clock::now();
    if (now - last_check >= check_every) {
        last_check = now;
        std::filesystem::file_time_type t{};
        if (newest_save(t)) {
            if (have_last_save && t != last_save && ultramodern::is_game_started()) {
                shown_until = now + icon_time;
            }
            last_save = t;
            have_last_save = true;
        }
    }
    const bool wanted = ui_ready && conker::qol::saving_icon() && now < shown_until;
    if (!wanted) {
        if (icon_created && recompui::is_context_shown(icon_context)) {
            recompui::hide_context(icon_context);
        }
        return;
    }
    if (!icon_created) {
        create_icon();
    }
    if (!recompui::is_context_shown(icon_context)) {
        recompui::show_context(icon_context, "");
    }
    // Pulsing and rocking a little.
    if (icon_image != nullptr) {
        const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
        icon_context.open();
        icon_image->set_opacity((float)(0.85 + 0.15 * std::sin(seconds * 2.0 * 3.14159265 * 1.5)));
        icon_image->set_rotation((float)(14.0 * std::sin(seconds * 2.0 * 3.14159265 * 0.9)));
        icon_context.close();
    }
}

void conker::qol::show_free_camera_notice(bool on) {
    notice_on = on;
    notice_at = clock::now().time_since_epoch().count();
}

void conker::qol::set_player_buttons(uint16_t buttons) {
    player_buttons = buttons;
}

uint16_t conker::qol::apply_toggles(uint16_t buttons) {
    constexpr uint16_t button_r = 0x0010, button_z = 0x2000;
    buttons = toggle_r.apply(buttons, button_r, conker::qol::toggle_r_look());
    buttons = toggle_z.apply(buttons, button_z, conker::qol::toggle_crouch());
    return buttons;
}

// Skip Any Cutscene: where func_1501E05C returns ($v0: skip the playing cutscene now). It runs each
// frame while a cutscene plays. Holding L for hold_seconds skips; a skip the game decided by itself
// (not from a button: the bar's walk-in after Skip Intro) is kept; a press alone does nothing.
extern "C" void conker_skip_cutscene_result(uint8_t* rdram, recomp_context* ctx) {
    cutscene_seen = ticks(); // (the cash counter hides during cutscenes too, cash_hud.cpp)
    if (!conker::qol::skip_any_cutscene()) {
        return;
    }
    const uint16_t pressed = (uint16_t)MEM_HU(0, (gpr)(int32_t)0x800BE710); // this frame's new presses
    const bool by_itself = ctx->r2 != 0 && (pressed & (button_l | button_start)) == 0;
    const bool held = (player_buttons.load() & button_l) != 0;
    if (!held) {
        hold_started = 0;
        wait_for_release = false;
        hold_progress = 0.0f;
    } else if (!wait_for_release && hold_started.load() == 0) {
        hold_started = ticks();
    }
    const int64_t started = hold_started.load();
    const float progress = started != 0 ? (float)std::min(1.0, seconds_since(started) / hold_seconds) : 0.0f;
    hold_progress = progress;
    if (progress >= 1.0f) {
        hold_started = 0;
        wait_for_release = true; // not the next cutscene too
        hold_progress = 0.0f;
        ctx->r2 = 1;
        return;
    }
    ctx->r2 = by_itself ? 1 : 0;
}

bool conker::qol::cutscene_playing() {
    return seconds_since(cutscene_seen.load()) < 0.25;
}

// Reduce Motion Effects: func_151D6778 draws the motion blur (Conker drunk at the start of the game:
// the last frames left as ghosts over the picture) when its strength, D_800BE574 ($v1 at 0x151D6798),
// isn't 0. Taken as 0 there, it draws none; the game's own value is left as it is.
extern "C" void conker_motion_blur(uint8_t* rdram, recomp_context* ctx) {
    if (conker::qol::reduce_motion()) {
        ctx->r3 = 0;
    }
}

// Always Show Health: the chocolate (health) display slides in when health changes and away again when
// its timer, D_800D2444 (frames, set to 240 by func_1508F0D4 on a change), runs out. Kept topped up,
// it stays on screen.
void conker::qol::on_vi_memory(uint8_t* rdram) {
    if (rdram == nullptr) {
        return;
    }
    if (capture_requests.load() > 0) {
        capture_requests--;
        static const bool enabled = std::filesystem::exists(recomp::get_config_path() / "capture_enabled");
        static int count = 0;
        if (enabled) {
            const std::filesystem::path path = recomp::get_config_path() / ("memory_" + std::to_string(++count) + ".rdram");
            if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
                std::fwrite(rdram, 1, 8 * 1024 * 1024, f);
                std::fclose(f);
            }
        }
    }
    conker::qol::longer_spin_on_vi(rdram);
    if (!conker::qol::always_show_hud()) {
        return;
    }
    MEM_W(0, (gpr)(int32_t)0x800D2444) = 240;
}


// Longer Tail Spin: pressing A again in a jump makes Conker spin his tail: his upward speed (his
// object's + 0x20, D_800CC2D0 is player 1's) is set to 7 and his gravity (+ 0x24) to 1.0, which stays
// until he lands (a jump's is 6.2, standing 5.0). He floats up and glides down. With the option on,
// while he spins (gravity exactly 1.0 or ours, in the air) gravity is lower and he falls more slowly,
// so the spin lasts longer. Set once per VI: the game keeps the value until he lands.
namespace {
    constexpr uint32_t player_object = 0x800CC2D0;
    constexpr float spin_gravity = 1.0f, longer_gravity = 0.55f, longer_fall_speed = -6.0f;
}

void conker::qol::longer_spin_on_vi(uint8_t* rdram) {
    if (!conker::qol::longer_spin()) {
        return;
    }
    const gpr object = (gpr)(int32_t)player_object;
    auto read = [&](int offset) { uint32_t w = (uint32_t)MEM_W(offset, object); float f; std::memcpy(&f, &w, 4); return f; };
    auto write = [&](int offset, float f) { uint32_t w; std::memcpy(&w, &f, 4); MEM_W(offset, object) = (int32_t)w; };
    const float gravity = read(0x24), height = read(0x28);
    if (height <= 0.0f || (gravity != spin_gravity && gravity != longer_gravity)) {
        return;
    }
    write(0x24, longer_gravity);
    if (read(0x20) < longer_fall_speed) {
        write(0x20, longer_fall_speed);
    }
}
