// Conker Redux: three additions to the game's own front end, in the game's own art.
//
// - GAME SETTINGS, a row under CHEATS on the Options page (front-end mode 1): down from CHEATS
//   moves onto it, A opens the VIDEO page.
// - The VIDEO page: RESOLUTION, ASPECT, REFRESH RATE, FULLSCREEN and CAMERA FOLLOW (settings.cpp's
//   conker::video),
//   up/down to pick a row, left/right (or A) to change it, B back to Options.
// - QUIT GAME, a popup on the heading carousel (front-end mode 0): B opens it, left/right picks
//   Y or N (N to start with; a knob beside the one picked), A confirms; Y quits.
//
// The words are Conker Redux's own art (host/assets/redux_menu, made from an atlas by
// tools/redux_menu_art.py: cut out, keyed, crunched to look like N64 art). From the player's ROM, at
// start-up: the joystick knobs that mark the selected row (as the game's own menus do), and the
// cheat-entry tan letter tiles, which spell GAME SETTINGS until it has a picture of its own (with
// glyphs made here for what the tiles lack). Nothing is copied from the ROM into the program.
//
// The game's own menu drawing doesn't take new rows, so ours are drawn over the game in a recompui
// context of its own (like the FPS counter), placed in the game menu's layout: in step with its
// rows, as a part of the window's height. While a row, the page or the popup is in use, the game
// doesn't see player 1's buttons or stick, and the game's sprites that ours replace (the Options
// page's rows on the VIDEO page, the knobs on CHEATS while GAME SETTINGS is picked) are drawn off
// screen (hook on the game's sprite routine, func_151ED430).
//
// What it reads from the game (each VI): gGameState (*D_8008FDD4) +0x2C, the front end's mode (0 the
// carousel or the heading in front, 1 Options, 2-6 the others) and +0x3E (a second mode, none of
// ours while it's set); D_8008FE34, the Options page's cursor row (21 CHEATS); D_8008FEF8, set while cheat entry
// shows; whether the front end ran lately (hook on func_151DE8F0, which runs every frame in it), and
// whether the Options page drew its rows lately (hook on func_151E3344).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <SDL.h>

#include "miniz.h"

#include "recomp.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "recompui/recompui.h"
#include "elements/ui_element.h"
#include "elements/ui_image.h"
#include "util/file.h"

#include "conker.hpp"

namespace {
    // ---- The art, from the ROM -------------------------------------------------------------------

    // Rare's compression: each asset a 4-byte big-endian size, then raw deflate.
    bool unpack(const std::vector<uint8_t>& rom, uint32_t offset, size_t expected, std::vector<uint8_t>& out) {
        if (offset + 4 > rom.size()) {
            return false;
        }
        const size_t size = ((size_t)rom[offset] << 24) | ((size_t)rom[offset + 1] << 16) | ((size_t)rom[offset + 2] << 8) | rom[offset + 3];
        if (size != expected) {
            return false;
        }
        out.assign(size, 0);
        const size_t available = std::min(rom.size() - offset - 4, size * 2 + 1024);
        const size_t written = tinfl_decompress_mem_to_mem(out.data(), out.size(), rom.data() + offset + 4, available, 0);
        return written == size;
    }

    struct Picture {
        uint32_t width = 0, height = 0;
        std::vector<char> rgba; // width * height * 4
    };

    uint8_t five_to_eight(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }

    // Textures stored for a block load: on odd rows, each pair of `unit`-byte groups is swapped.
    void unswap_odd_rows(std::vector<uint8_t>& data, size_t stride, size_t unit) {
        for (size_t row = 1; row * stride + stride <= data.size(); row += 2) {
            uint8_t* r = data.data() + row * stride;
            for (size_t q = 0; q + 2 * unit <= stride; q += 2 * unit) {
                std::swap_ranges(r + q, r + q + unit, r + q + unit);
            }
        }
    }

    // A cheat-entry tile: 16x16 CI8 then a 256-colour RGBA5551 palette, stored upside down.
    bool decode_tile(const std::vector<uint8_t>& rom, uint32_t offset, Picture& out) {
        std::vector<uint8_t> data;
        if (!unpack(rom, offset, 768, data)) {
            return false;
        }
        unswap_odd_rows(data, 16, 4);
        out.width = out.height = 16;
        out.rgba.resize(16 * 16 * 4);
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                const uint8_t index = data[(15 - y) * 16 + x];
                const uint32_t c = ((uint32_t)data[256 + index * 2] << 8) | data[256 + index * 2 + 1];
                char* p = &out.rgba[(y * 16 + x) * 4];
                p[0] = (char)five_to_eight((c >> 11) & 31);
                p[1] = (char)five_to_eight((c >> 6) & 31);
                p[2] = (char)five_to_eight((c >> 1) & 31);
                p[3] = (char)((c & 1) ? 255 : 0);
            }
        }
        return true;
    }

    // A 32x32 RGBA32 sprite (the knobs).
    bool decode_rgba32(const std::vector<uint8_t>& rom, uint32_t offset, Picture& out) {
        std::vector<uint8_t> data;
        if (!unpack(rom, offset, 4096, data)) {
            return false;
        }
        unswap_odd_rows(data, 128, 8);
        out.width = out.height = 32;
        out.rgba.assign(data.begin(), data.end());
        return true;
    }

    // Where they are in the US ROM. A to Z (each its own asset), then the rest.
    constexpr uint32_t letter_offsets[26] = {
        0x1A5527, 0x1A574C, 0x1A5961, 0x1A5B6B, 0x1A5D75, 0x1A5FA5, 0x1A61B3, 0x1A63DE, 0x1A6603,
        0x1A6821, 0x1A6A34, 0x1A6C4C, 0x1A6E66, 0x1A707B, 0x1A727E, 0x1A748B, 0x1A769F, 0x1A78B9,
        0x1A7AE0, 0x1A7D00, 0x1A7F23, 0x1A8141, 0x1A8362, 0x1A8580, 0x1A87A4, 0x1A89AE,
    };
    constexpr uint32_t knob_offsets[6] = { 0x486241, 0x486B23, 0x4873F9, 0x487CE2, 0x4885CD, 0x488EC0 };

    // Our glyphs for what the tiles lack, 8x11, drawn bold on a blank tile.
    struct Glyph { char c; const char* rows[11]; };
    constexpr Glyph glyphs[] = {
        { '0', { "..####..", ".##..##.", "##....##", "##....##", "##....##", "##....##", "##....##", "##....##", "##....##", ".##..##.", "..####.." } },
        { '1', { "...##...", "..###...", ".####...", "...##...", "...##...", "...##...", "...##...", "...##...", "...##...", "...##...", ".######." } },
        { '2', { "..####..", ".##..##.", "##....##", "......##", ".....##.", "....##..", "...##...", "..##....", ".##.....", "##......", "########" } },
        { '3', { ".######.", "##....##", "......##", "......##", "...####.", "......##", "......##", "......##", "......##", "##....##", ".######." } },
        { '4', { ".....##.", "....###.", "...####.", "..##.##.", ".##..##.", "##...##.", "########", ".....##.", ".....##.", ".....##.", ".....##." } },
        { '5', { "########", "##......", "##......", "##......", "#######.", "......##", "......##", "......##", "......##", "##....##", ".######." } },
        { '6', { "..#####.", ".##.....", "##......", "##......", "#######.", "##....##", "##....##", "##....##", "##....##", ".##..##.", "..####.." } },
        { '7', { "########", "......##", ".....##.", ".....##.", "....##..", "....##..", "...##...", "...##...", "..##....", "..##....", "..##...." } },
        { '8', { "..####..", ".##..##.", "##....##", ".##..##.", "..####..", ".##..##.", "##....##", "##....##", "##....##", ".##..##.", "..####.." } },
        { '9', { "..####..", ".##..##.", "##....##", "##....##", "##....##", ".#######", "......##", "......##", "......##", ".....##.", ".#####.." } },
        { ':', { "........", "........", "...##...", "...##...", "........", "........", "........", "...##...", "...##...", "........", "........" } },
        { '?', { "..####..", ".##..##.", "##....##", "......##", ".....##.", "....##..", "...##...", "...##...", "........", "...##...", "...##..." } },
    };

    // A blank tan tile: the I tile (the thinnest letter) with its ink filled in from the tan around it.
    Picture blank_tile(const Picture& tile) {
        Picture out = tile;
        auto at = [&](std::vector<char>& v, int x, int y) { return reinterpret_cast<uint8_t*>(&v[(y * 16 + x) * 4]); };
        auto ink = [](const uint8_t* p) { return p[0] + p[1] + p[2] < 3 * 110; };
        std::vector<char> next = out.rgba;
        for (int pass = 0; pass < 8; pass++) {
            bool any = false;
            for (int y = 0; y < 16; y++) {
                for (int x = 0; x < 16; x++) {
                    uint8_t* p = at(out.rgba, x, y);
                    if (!ink(p) || p[3] == 0) {
                        continue;
                    }
                    int sum[3] = {}, n = 0;
                    for (int dy = -2; dy <= 2; dy++) {
                        for (int dx = -2; dx <= 2; dx++) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx > 15 || ny > 15) continue;
                            const uint8_t* q = at(out.rgba, nx, ny);
                            if (!ink(q) && q[3] != 0) {
                                for (int k = 0; k < 3; k++) sum[k] += q[k];
                                n++;
                            }
                        }
                    }
                    if (n > 0) {
                        uint8_t* d = at(next, x, y);
                        for (int k = 0; k < 3; k++) d[k] = (uint8_t)(sum[k] / n);
                    }
                    any = true;
                }
            }
            out.rgba = next;
            if (!any) break;
        }
        return out;
    }

    // One of our glyphs on a blank tile: dark ink (the letters' own), a step bolder, a soft brown edge.
    Picture glyph_tile(const Picture& blank, const Glyph& g) {
        Picture out = blank;
        bool on[16][16] = {};
        for (int y = 0; y < 11; y++) {
            for (int x = 0; x < 8; x++) {
                if (g.rows[y][x] == '#') {
                    const int px = 4 + x, py = 2 + y;
                    on[py][px] = true;
                    if (px + 1 < 16) on[py][px + 1] = true; // bold, like the tiles' letters
                }
            }
        }
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                uint8_t* p = reinterpret_cast<uint8_t*>(&out.rgba[(y * 16 + x) * 4]);
                if (on[y][x]) {
                    p[0] = 24; p[1] = 16; p[2] = 8;
                } else {
                    bool edge = false;
                    for (int dy = -1; dy <= 1 && !edge; dy++)
                        for (int dx = -1; dx <= 1 && !edge; dx++) {
                            const int nx = x + dx, ny = y + dy;
                            edge = nx >= 0 && ny >= 0 && nx < 16 && ny < 16 && on[ny][nx];
                        }
                    if (edge) {
                        p[0] = (uint8_t)((p[0] + 90) / 2); p[1] = (uint8_t)((p[1] + 58) / 2); p[2] = (uint8_t)((p[2] + 28) / 2);
                    }
                }
            }
        }
        return out;
    }

    std::string tile_src(char c) { return std::string("?/redux/tile_") + c; }

    // Each character's width as a part of its height (the ROM's tiles and our glyphs are square;
    // a custom font's letters are as wide as their pictures).
    std::array<float, 128> glyph_aspect = [] { std::array<float, 128> a{}; a.fill(1.0f); return a; }();

    // The custom font (Conker Redux's own): assets/redux_font/<name>.png, one picture per character,
    // A-Z, 0-9, colon.png and question.png; each one there replaces the ROM's tile or our glyph.
    // Its size comes from the PNG header (width and height, big-endian, at 16 and 20).
    void load_custom_font() {
        for (int c = 32; c < 128; c++) {
            const bool letter = c >= 'A' && c <= 'Z', digit = c >= '0' && c <= '9';
            if (!letter && !digit && c != ':' && c != '?') {
                continue;
            }
            const std::string name = c == ':' ? "colon" : c == '?' ? "question" : std::string(1, (char)c);
            std::ifstream file(recompui::file::get_asset_path(("redux_font/" + name + ".png").c_str()), std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (bytes.size() < 24 || std::memcmp(bytes.data() + 1, "PNG", 3) != 0) {
                continue;
            }
            auto be32 = [&](size_t at) {
                return ((uint32_t)(uint8_t)bytes[at] << 24) | ((uint32_t)(uint8_t)bytes[at + 1] << 16) |
                       ((uint32_t)(uint8_t)bytes[at + 2] << 8) | (uint32_t)(uint8_t)bytes[at + 3];
            };
            const uint32_t width = be32(16), height = be32(20);
            if (width == 0 || height == 0) {
                continue;
            }
            glyph_aspect[c] = (float)width / (float)height;
            recompui::queue_image_from_bytes_file(tile_src((char)c), bytes);
        }
    }

    // The menu's words (Conker Redux's own art, made by tools/redux_menu_art.py from
    // host/art/redux_menu_atlas.webp): assets/redux_menu/<name>.png, shown as "?/redux/w_<name>", each
    // as wide as its picture. Knob frames there (knob0.png...) replace the ROM's, as many as there are.
    const char* const word_names[] = {
        "title_video", "title_quit_game", "game_settings",
        "label_resolution", "label_aspect", "label_refresh_rate", "label_full_screen",
        "res_480", "res_720", "res_1080", "res_1440", "res_4k", "aspect_4_3", "aspect_16_9",
        "hz_30", "hz_60", "off", "on", "yn_y", "yn_slash", "yn_n", "label_camera_follow",
    };
    struct Word { std::string name; float aspect = 0.0f; }; // aspect 0: not there
    std::vector<Word> words;
    int knob_frames = 6;

    bool read_png(const std::string& file_name, std::vector<char>& bytes, float* aspect) {
        std::ifstream file(recompui::file::get_asset_path(("redux_menu/" + file_name + ".png").c_str()), std::ios::binary);
        bytes.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.size() < 24 || std::memcmp(bytes.data() + 1, "PNG", 3) != 0) {
            return false;
        }
        auto be32 = [&](size_t at) {
            return ((uint32_t)(uint8_t)bytes[at] << 24) | ((uint32_t)(uint8_t)bytes[at + 1] << 16) |
                   ((uint32_t)(uint8_t)bytes[at + 2] << 8) | (uint32_t)(uint8_t)bytes[at + 3];
        };
        const uint32_t width = be32(16), height = be32(20);
        if (width == 0 || height == 0) {
            return false;
        }
        *aspect = (float)width / (float)height;
        return true;
    }

    void load_words() {
        std::vector<char> bytes;
        for (const char* name : word_names) {
            Word w{ name };
            if (read_png(name, bytes, &w.aspect)) {
                recompui::queue_image_from_bytes_file("?/redux/w_" + w.name, bytes);
            }
            words.push_back(w);
        }
        int custom_knobs = 0;
        float unused;
        while (custom_knobs < 6 && read_png("knob" + std::to_string(custom_knobs), bytes, &unused)) {
            recompui::queue_image_from_bytes_file("?/redux/knob" + std::to_string(custom_knobs), bytes);
            custom_knobs++;
        }
        if (custom_knobs > 0) {
            knob_frames = custom_knobs;
        }
    }

    float word_aspect(const std::string& name) {
        for (const Word& w : words) {
            if (w.name == name) return w.aspect;
        }
        return 0.0f;
    }

    // Decodes everything and hands it to recompui. False (and the features stay off) if the ROM's
    // assets aren't where the US ROM has them.
    bool load_art() {
        const std::filesystem::path path = recomp::get_config_path() / "conker.n64.us.1.0.z64";
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> rom((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (rom.size() < 0x800000) {
            std::printf("[game menu] no stored ROM at %s\n", path.string().c_str());
            return false;
        }
        Picture letters[26];
        for (int i = 0; i < 26; i++) {
            if (!decode_tile(rom, letter_offsets[i], letters[i])) {
                std::printf("[game menu] the ROM's letter tiles aren't where the US ROM has them: off\n");
                return false;
            }
        }
        Picture knobs[6];
        bool ok = true;
        for (int i = 0; i < 6 && ok; i++) {
            ok = decode_rgba32(rom, knob_offsets[i], knobs[i]);
        }
        if (!ok) {
            std::printf("[game menu] the ROM's menu sprites aren't where the US ROM has them: off\n");
            return false;
        }
        auto queue = [](const std::string& src, const Picture& p) {
            recompui::queue_image_from_bytes_rgba32(src, p.rgba, p.width, p.height);
        };
        for (int i = 0; i < 26; i++) {
            queue(tile_src((char)('A' + i)), letters[i]);
        }
        const Picture blank = blank_tile(letters['I' - 'A']);
        for (const Glyph& g : glyphs) {
            queue(tile_src(g.c), glyph_tile(blank, g));
        }
        for (int i = 0; i < 6; i++) {
            queue("?/redux/knob" + std::to_string(i), knobs[i]);
        }
        load_custom_font(); // after the tiles: its pictures take their names
        load_words();
        return true;
    }

    // ---- The game's state, and ours ---------------------------------------------------------------

    std::atomic<uint32_t> vis{ 0 };
    std::atomic<uint32_t> front_end_vi{ 0 };   // the VI the front end last ran in (0: never)
    std::atomic<uint32_t> options_page_vi{ 0 }; // the VI the Options page last drew its rows in
    std::atomic<uint32_t> heading_page_vi{ 0 }; // the VI any heading's page (Options, the games...) last ran in
    std::atomic<int> mode{ -1 }, second_mode{ 0 }, options_cursor{ 0 }, cheat_entry{ 0 };
    std::atomic<bool> art_ok{ false };

    enum class State : int { None, RowSelected, Page, Popup };
    std::atomic<State> state{ State::None };
    std::atomic<int> page_row{ 0 };            // 0-3
    std::atomic<int> popup_yes{ 0 };           // 1: Y picked
    std::atomic<int> pending_row{ -1 }, pending_dir{ 0 }; // a change for the UI thread to make
    std::atomic<bool> pending_quit{ false };
    // Up from GAME SETTINGS: our knobs glide back to CHEATS before the game's own show there again
    // (until then theirs stay hidden). Steady-clock milliseconds; 0: not handing back.
    std::atomic<int64_t> handing_back_until{ 0 };
    constexpr int64_t hand_back_ms = 170;

    int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    constexpr int cheats_row = 21;
    constexpr int page_rows = 5;

    bool front_end_running() {
        const uint32_t seen = front_end_vi.load();
        return seen != 0 && vis.load() - seen < 8;
    }
    // Inside Options: its page drew its rows lately (not while OPTIONS is only the carousel's
    // heading: the front end's mode is already 1 then).
    bool in_options() {
        const uint32_t seen = options_page_vi.load();
        return front_end_running() && mode == 1 && second_mode == 0 && cheat_entry == 0 &&
            seen != 0 && vis.load() - seen < 6;
    }
    // The heading carousel: the front end runs and no heading's page has lately (the front end's mode
    // follows whichever heading is in front, so it can't tell).
    bool on_carousel() {
        const uint32_t seen = heading_page_vi.load();
        return front_end_running() && (seen == 0 || vis.load() - seen >= 6);
    }

    // ---- Input -------------------------------------------------------------------------------------

    constexpr uint16_t button_a = 0x8000, button_b = 0x4000, button_du = 0x0800, button_dd = 0x0400,
                       button_dl = 0x0200, button_dr = 0x0100;

    // Directions from the stick or the D-pad: a press, then repeats while held.
    struct Repeat {
        bool held = false;
        std::chrono::steady_clock::time_point next{};
        bool step(bool down) {
            const auto now = std::chrono::steady_clock::now();
            if (!down) {
                held = false;
                return false;
            }
            if (!held) {
                held = true;
                next = now + std::chrono::milliseconds(380);
                return true;
            }
            if (now >= next) {
                next = now + std::chrono::milliseconds(130);
                return true;
            }
            return false;
        }
    };
    Repeat rep_up, rep_down, rep_left, rep_right;
    uint16_t previous_buttons = 0;
    // Up from GAME SETTINGS hands the cursor back on CHEATS: the game doesn't see that up (still
    // held for a moment), or it would take it as a fresh one and go on past CHEATS.
    bool hide_up_until_released = false;
}

void conker::game_menu::filter_input(uint16_t* buttons, float* x, float* y) {
    const uint16_t now = *buttons;
    const uint16_t pressed = now & ~previous_buttons;
    previous_buttons = now;
    const bool up = rep_up.step(*y > 0.5f || (now & button_du));
    const bool down = rep_down.step(*y < -0.5f || (now & button_dd));
    const bool left = rep_left.step(*x < -0.5f || (now & button_dl));
    const bool right = rep_right.step(*x > 0.5f || (now & button_dr));
    if (hide_up_until_released) {
        if (*y > 0.05f || (now & button_du)) {
            *y = std::min(*y, 0.0f);
            *buttons &= (uint16_t)~button_du;
        } else {
            hide_up_until_released = false;
        }
    }
    if (!art_ok) {
        return;
    }
    auto swallow = [&]() {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
    };

    switch (state.load()) {
        case State::None:
            if (in_options() && options_cursor == cheats_row && down) {
                state = State::RowSelected;
                swallow();
            } else if (on_carousel() && (pressed & button_b)) {
                popup_yes = 0;
                state = State::Popup;
                swallow();
            }
            break;
        case State::RowSelected:
            if (!in_options()) {
                state = State::None;
                break;
            }
            if (pressed & button_b) {
                state = State::None; // B goes on to the game: out of Options
                break;
            }
            if (down) {
                // On to the top row, as the game's own cursor wraps: it's still on CHEATS, so a fresh
                // down (it saw none while ours was picked) takes it round.
                state = State::None;
                *buttons &= (uint16_t)~(button_a | button_b);
                break;
            }
            if (up) {
                handing_back_until = now_ms() + hand_back_ms;
                hide_up_until_released = true;
                state = State::None;
            } else if (pressed & button_a) {
                page_row = 0;
                state = State::Page;
            }
            swallow();
            break;
        case State::Page:
            if (!in_options()) {
                state = State::None;
                break;
            }
            if (pressed & button_b) {
                state = State::RowSelected;
            } else if (up) {
                page_row = (page_row + page_rows - 1) % page_rows;
            } else if (down) {
                page_row = (page_row + 1) % page_rows;
            } else if (left || right || (pressed & button_a)) {
                pending_dir = left ? -1 : 1;
                pending_row = page_row.load();
            }
            swallow();
            break;
        case State::Popup:
            if (pressed & button_b) {
                state = State::None;
            } else if (left || right) {
                popup_yes = popup_yes ? 0 : 1;
            } else if (pressed & button_a) {
                if (popup_yes) {
                    pending_quit = true;
                }
                state = State::None;
            }
            swallow();
            break;
    }
}

void conker::game_menu::on_vi(uint8_t* rdram) {
    vis.fetch_add(1);
    const uint32_t state_ptr = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8008FDD4);
    if (state_ptr >= 0x80000000u && state_ptr < 0x80800000u) {
        mode = (int)(int8_t)MEM_B(0x2C, (gpr)(int32_t)state_ptr);
        second_mode = (int)(int8_t)MEM_B(0x3E, (gpr)(int32_t)state_ptr);
    } else {
        mode = -1;
    }
    options_cursor = (int)(uint8_t)MEM_B(0, (gpr)(int32_t)0x8008FE34);
    cheat_entry = (int)(int8_t)MEM_B(0, (gpr)(int32_t)0x8008FEF8);
    // Out of the front end (into a level): nothing of ours stays up.
    if (!front_end_running() && state.load() != State::None) {
        state = State::None;
    }
}

// func_151DE8F0 (every frame in the front end), after its first instruction.
extern "C" void conker_menu_front_end(uint8_t* rdram, recomp_context* ctx) {
    front_end_vi = std::max<uint32_t>(vis.load(), 1);
}

// Each heading's page (func_151DF1BC Options, func_151DF574, func_151DFF38 the games, and the
// multiplayer ones), after its first instruction: a heading is open, not the carousel.
extern "C" void conker_menu_heading_page(uint8_t* rdram, recomp_context* ctx) {
    heading_page_vi = std::max<uint32_t>(vis.load(), 1);
}

// func_151E3344 (the Options page: runs every frame once Options is entered), after its first instruction.
extern "C" void conker_menu_options_page(uint8_t* rdram, recomp_context* ctx) {
    options_page_vi = std::max<uint32_t>(vis.load(), 1);
}

// func_151ED430 (draws a sprite), with its arguments as given: $a2 x, $a3 y. The Options page's
// sprites that ours replace go off screen.
extern "C" void conker_menu_sprite(uint8_t* rdram, recomp_context* ctx) {
    if (!in_options()) {
        return;
    }
    const State s = state.load();
    const int x = (int)ctx->r6;
    const bool knob = x == 98 || x == 198; // the knobs either side of the picked row
    const bool handing_back = now_ms() < handing_back_until.load();
    if (s == State::Page || ((s == State::RowSelected || handing_back) && knob)) {
        ctx->r6 = 900;
        ctx->r7 = 900;
    }
}

namespace {
    // ---- Drawing -----------------------------------------------------------------------------------

    // The layout, as parts of the window's height (the game's 2D is drawn to the full height and
    // centred across), measured on the Options page: CHEATS's row centre and the rows' spacing.
    constexpr float cheats_y = 0.487f, row_step = 0.168f;
    constexpr float row_height = 0.045f;                 // GAME SETTINGS
    constexpr float knob_size = 0.13f, knob_gap = 0.075f;
    constexpr float cheats_knob_half = 0.233f;           // the game's knobs on CHEATS, either side of centre
    constexpr float knob_glide_seconds = 0.045f;         // the knobs ease to a new row (as the game's glide)
    constexpr float title_y = 0.32f, title_height = 0.085f;
    constexpr float page_first_y = 0.43f, page_step = 0.09f, page_height = 0.05f;
    constexpr float page_label_right = -0.05f, page_value_x = 0.32f;
    constexpr float page_knob = 0.09f, page_knob_x = 0.66f;
    constexpr float popup_w = 0.95f, popup_h = 0.46f, popup_title_y = 0.42f, popup_choice_y = 0.58f,
                    popup_choice_height = 0.075f, popup_choice_dx = 0.11f, popup_knob = 0.09f, popup_knob_gap = 0.06f;

    const char* const page_labels[page_rows] = { "label_resolution", "label_aspect", "label_refresh_rate", "label_full_screen", "label_camera_follow" };
    const char* const page_values[page_rows][5] = {
        { "res_480", "res_720", "res_1080", "res_1440", "res_4k" },
        { "aspect_4_3", "aspect_16_9" }, { "hz_30", "hz_60" }, { "off", "on" }, { "off", "on" },
    };

    bool ui_ready = false, created = false;
    recompui::ContextId context = recompui::ContextId::null();
    recompui::Element* panel = nullptr;

    // A line of the ROM's tiles (GAME SETTINGS, until its own picture is made): a pool of images.
    struct Text {
        std::vector<recompui::Image*> tiles;
    };
    Text row_text;
    recompui::Image* row_word = nullptr;
    recompui::Image* title_word = nullptr;
    recompui::Image* label_words[page_rows] = {};
    recompui::Image* value_words[page_rows] = {};
    recompui::Image* choice_words[3] = {}; // Y, /, N
    recompui::Image* knobs[2] = {};

    // Where the pair of knobs is drawn (window pixels): eased toward where they belong each update,
    // so they glide between rows and push in or out to a row's width, as the game's own do.
    struct KnobPose { float y = 0, half = 0, size = 0; };
    KnobPose shown_knobs;
    bool knobs_shown = false;
    std::chrono::steady_clock::time_point last_knob_update{};

    recompui::Image* make_image(recompui::Element* root, const std::string& src) {
        recompui::Image* image = context.create_element<recompui::Image>(root, src);
        image->set_position(recompui::Position::Absolute);
        image->set_display(recompui::Display::None);
        return image;
    }

    void place(recompui::Element* e, float cx, float cy, float w, float h) {
        // Window pixels (recompui's default unit is DPI-scaled).
        e->set_left(cx - w / 2, recompui::Unit::Px);
        e->set_top(cy - h / 2, recompui::Unit::Px);
        e->set_width(w, recompui::Unit::Px);
        e->set_height(h, recompui::Unit::Px);
        e->set_display(recompui::Display::Block);
    }

    // A word picture `height` tall; `anchor` 0 puts its left edge at x, 0.5 its centre, 1 its right
    // edge. Returns its width (0: the picture isn't there).
    float place_word(recompui::Image* image, const std::string& name, float x, float cy, float height, float anchor) {
        const float aspect = word_aspect(name);
        if (aspect <= 0.0f) {
            image->set_display(recompui::Display::None);
            return 0.0f;
        }
        const float w = height * aspect;
        image->set_src("?/redux/w_" + name);
        place(image, x + (0.5f - anchor) * w, cy, w, height);
        return w;
    }

    // Tiles: characters 5% of the height apart, a space half a tile.
    float advance(char c, float tile) {
        return c == ' ' ? tile * 0.5f : tile * (glyph_aspect[(unsigned char)c & 127] + 0.05f);
    }

    float text_width(const std::string& s, float tile) {
        float w = 0.0f;
        for (char c : s) w += advance(c, tile);
        return w;
    }

    void place_text(Text& text, const std::string& s, float left, float cy, float tile) {
        size_t used = 0;
        float x = left;
        for (size_t i = 0; i < s.size() && used < text.tiles.size(); i++) {
            if (s[i] != ' ') {
                recompui::Image* t = text.tiles[used++];
                t->set_src(tile_src(s[i]));
                const float w = tile * glyph_aspect[(unsigned char)s[i] & 127];
                place(t, x + w / 2, cy, w, tile);
            }
            x += advance(s[i], tile);
        }
        for (; used < text.tiles.size(); used++) {
            text.tiles[used]->set_display(recompui::Display::None);
        }
    }

    void create() {
        context = recompui::create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        recompui::Element* root = context.get_root_element();
        panel = context.create_element<recompui::Element>(root);
        panel->set_position(recompui::Position::Absolute);
        panel->set_background_color(recompui::Color{ 0x1A, 0x10, 0x06, 0xD8 });
        panel->set_border_color(recompui::Color{ 0xE8, 0xC8, 0x80, 0xFF });
        panel->set_display(recompui::Display::None);
        for (int i = 0; i < 13; i++) {
            row_text.tiles.push_back(make_image(root, tile_src('A')));
        }
        row_word = make_image(root, "?/redux/w_game_settings");
        title_word = make_image(root, "?/redux/w_title_video");
        for (int r = 0; r < page_rows; r++) {
            label_words[r] = make_image(root, "?/redux/w_label_resolution");
            value_words[r] = make_image(root, "?/redux/w_res_1080");
        }
        for (recompui::Image*& c : choice_words) {
            c = make_image(root, "?/redux/w_yn_y");
        }
        knobs[0] = make_image(root, "?/redux/knob0");
        knobs[1] = make_image(root, "?/redux/knob0");
        context.close();
        created = true;
    }

    void hide_all() {
        for (recompui::Image* t : row_text.tiles) t->set_display(recompui::Display::None);
        for (recompui::Image* e : { row_word, title_word, knobs[0], knobs[1], choice_words[0], choice_words[1], choice_words[2] }) {
            e->set_display(recompui::Display::None);
        }
        for (int r = 0; r < page_rows; r++) {
            label_words[r]->set_display(recompui::Display::None);
            value_words[r]->set_display(recompui::Display::None);
        }
        panel->set_display(recompui::Display::None);
    }
}

void conker::game_menu::on_ui_ready() {
    ui_ready = true;
}

void conker::game_menu::update() {
    if (!ui_ready) {
        return;
    }
    static bool art_tried = false;
    if (!art_tried) {
        art_tried = true;
        art_ok = load_art();
    }
    if (pending_quit.exchange(false)) {
        ultramodern::quit();
        return;
    }
    const int row = pending_row.exchange(-1);
    if (row >= 0) {
        conker::video::change(row, pending_dir.load());
    }
    // CONKER_MENU_DEBUG=1: what this sees, every 2 seconds.
    static const bool debug = std::getenv("CONKER_MENU_DEBUG") != nullptr;
    static auto last_debug = std::chrono::steady_clock::now();
    if (debug && std::chrono::steady_clock::now() - last_debug > std::chrono::seconds(2)) {
        last_debug = std::chrono::steady_clock::now();
        int dw = 0, dh = 0;
        conker::frontend::window_pixels(&dw, &dh);
        std::printf("[game menu] art %d front end %d (vi %u, seen %u) mode %d second %d cheat %d cursor %d state %d window %dx%d\n",
            (int)art_ok.load(), (int)front_end_running(), vis.load(), front_end_vi.load(), mode.load(), second_mode.load(),
            cheat_entry.load(), options_cursor.load(), (int)state.load(), dw, dh);
        std::fflush(stdout);
    }
    if (!art_ok) {
        return;
    }

    const bool options = in_options();
    const State s = state.load();
    const bool show_row = options && s != State::Page;
    const bool show_page = options && s == State::Page;
    const bool show_popup = s == State::Popup;
    if (!show_row && !show_page && !show_popup) {
        knobs_shown = false;
        if (created && recompui::is_context_shown(context)) {
            recompui::hide_context(context);
        }
        return;
    }
    if (!created) {
        create();
    }
    if (!recompui::is_context_shown(context)) {
        recompui::show_context(context, "");
    }

    int w = 0, h = 0;
    conker::frontend::window_pixels(&w, &h);
    if (w <= 0 || h <= 0) {
        return;
    }
    const float H = (float)h, cx = w / 2.0f;
    const int knob_frame = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 90 % 10);
    const int frame = knob_frame < 6 ? knob_frame : 10 - knob_frame; // back and forth, as the game's
    const std::string knob_src = "?/redux/knob" + std::to_string(std::clamp(frame, 0, 5) % knob_frames);
    auto put_knob = [&](int k, float x, float y, float size) {
        knobs[k]->set_src(knob_src);
        place(knobs[k], x, y, size, size);
    };

    // The pair's target this update, if any (the popup's single knob isn't animated).
    const KnobPose cheats_pose{ cheats_y * H, cheats_knob_half * H, knob_size * H };
    KnobPose knob_target;
    bool have_target = false;

    context.open();
    hide_all();
    if (show_row) {
        const float y = (cheats_y + row_step) * H, height = row_height * H;
        float width = place_word(row_word, "game_settings", cx, y, height * 1.6f, 0.5f);
        if (width <= 0.0f) {
            const std::string label = "GAME SETTINGS";
            width = text_width(label, height);
            place_text(row_text, label, cx - width / 2, y, height);
        }
        if (s == State::RowSelected) {
            knob_target = { y, width / 2 + knob_gap * H, knob_size * H };
            have_target = true;
        } else if (now_ms() < handing_back_until.load()) {
            knob_target = cheats_pose; // gliding back to CHEATS, then the game's own take over
            have_target = true;
        }
    }
    if (show_page) {
        place_word(title_word, "title_video", cx, title_y * H, title_height * H, 0.5f);
        const float height = page_height * H;
        for (int r = 0; r < page_rows; r++) {
            const float y = (page_first_y + r * page_step) * H;
            place_word(label_words[r], page_labels[r], cx + page_label_right * H, y, height, 1.0f);
            const int v = std::clamp(conker::video::value(r), 0, conker::video::count(r) - 1);
            place_word(value_words[r], page_values[r][v], cx + page_value_x * H, y, height, 0.5f);
            if (r == page_row) {
                knob_target = { y, page_knob_x * H, page_knob * H };
                have_target = true;
            }
        }
    }
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - last_knob_update).count(), 0.0f, 0.1f);
    last_knob_update = now;
    if (have_target) {
        if (!knobs_shown) {
            // Freshly picked from CHEATS: start where the game's own knobs are.
            shown_knobs = (s == State::RowSelected) ? cheats_pose : knob_target;
            knobs_shown = true;
        }
        const float ease = 1.0f - std::exp(-dt / knob_glide_seconds);
        shown_knobs.y += (knob_target.y - shown_knobs.y) * ease;
        shown_knobs.half += (knob_target.half - shown_knobs.half) * ease;
        shown_knobs.size += (knob_target.size - shown_knobs.size) * ease;
        put_knob(0, cx - shown_knobs.half, shown_knobs.y, shown_knobs.size);
        put_knob(1, cx + shown_knobs.half, shown_knobs.y, shown_knobs.size);
    } else {
        knobs_shown = false;
    }
    if (show_popup) {
        place(panel, cx, 0.5f * H, popup_w * H, popup_h * H);
        panel->set_border_width(0.006f * H, recompui::Unit::Px);
        panel->set_border_radius(0.03f * H, recompui::Unit::Px);
        place_word(title_word, "title_quit_game", cx, popup_title_y * H, title_height * H, 0.5f);
        const float height = popup_choice_height * H, y = popup_choice_y * H;
        const float y_width = place_word(choice_words[0], "yn_y", cx - popup_choice_dx * H, y, height, 0.5f);
        place_word(choice_words[1], "yn_slash", cx, y, height, 0.5f);
        const float n_width = place_word(choice_words[2], "yn_n", cx + popup_choice_dx * H, y, height, 0.5f);
        // The knob beside the one picked: left of Y, right of N.
        const float k = popup_knob * H;
        if (popup_yes) {
            put_knob(0, cx - popup_choice_dx * H - y_width / 2 - popup_knob_gap * H, y, k);
        } else {
            put_knob(0, cx + popup_choice_dx * H + n_width / 2 + popup_knob_gap * H, y, k);
        }
    }
    context.close();
}
