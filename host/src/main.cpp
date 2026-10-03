// Conker BFD: Reloaded -- entry point.
//
// Usage: ConkerRedux [--rom <US ROM>] [--launcher] [--seconds N]
//   --rom PATH   the US ROM (only needed once; it's then kept with the game's data,
//                and the launcher can load one too)
//   --launcher   show the launcher (mods, another ROM) instead of starting the game;
//                holding Shift while it starts does the same
//   --seconds N  skip the launcher, start the game and quit after N seconds of game
//                time (tests; see testing.cpp for fast-forward)
//
// Click and play (Conker Redux): with no ROM yet, one next to the program is taken (.z64, .n64
// or .v64), or the player is asked for one, once; then it starts straight into the game.
//
// The runtime setup follows CBFD-Recompiled's host (MIT, Sean Ciaschi).

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#ifdef _WIN32
#   include <Windows.h>
#   include <DbgHelp.h>
#endif

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <SDL.h>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"
#include "nfd.h"
#include "recompui/program_config.h"
#include "recompui/renderer.h"
#include "util/file.h"

#include "conker.hpp"
#include "version.h"

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
// The audio microcode, recompiled by RSPRecomp (recomp/audio_ucode.toml).
RspExitReason conker_audio_ucode(uint8_t* rdram, uint32_t ucode_addr);

namespace {
    const std::u8string game_id = u8"conker.n64.us.1.0";
    uint8_t* g_rdram = nullptr;
    std::atomic<uint32_t> vi_count{0};

#ifdef _WIN32
    // On a crash, write where it happened (recompiled functions are named after their
    // N64 address) to crash.log in the game's data folder (an installed program's own
    // folder can't be written to), or next to the exe until that folder is known.
    std::filesystem::path crash_log_path;

    LONG WINAPI crash_handler(EXCEPTION_POINTERS* info) {
        std::filesystem::path log_path = crash_log_path;
        if (log_path.empty()) {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            log_path = std::filesystem::path(exe).parent_path() / "crash.log";
        }
        FILE* log = _wfopen(log_path.c_str(), L"a");
        FILE* outs[] = { stderr, log };
        HANDLE process = GetCurrentProcess();
        SymInitialize(process, nullptr, TRUE);
        void* frames[32];
        frames[0] = info->ExceptionRecord->ExceptionAddress;
        USHORT count = 1 + CaptureStackBackTrace(0, 31, frames + 1, nullptr);
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        for (FILE* f : outs) {
            if (f == nullptr) continue;
            std::fprintf(f, "[host] exception 0x%08lX at %p\n", info->ExceptionRecord->ExceptionCode, frames[0]);
            if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && g_rdram != nullptr) {
                uintptr_t fault = (uintptr_t)info->ExceptionRecord->ExceptionInformation[1];
                std::fprintf(f, "[host] %s N64 address 0x%08X\n", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                    (uint32_t)(fault - (uintptr_t)g_rdram + 0x80000000u));
            }
            for (USHORT i = 0; i < count; i++) {
                DWORD64 offset = 0;
                if (SymFromAddr(process, (DWORD64)frames[i], &offset, symbol)) {
                    std::fprintf(f, "  %s+0x%llx\n", symbol->Name, (unsigned long long)offset);
                } else {
                    std::fprintf(f, "  %p\n", frames[i]);
                }
            }
            std::fflush(f);
        }
        if (log != nullptr) std::fclose(log);
        return EXCEPTION_CONTINUE_SEARCH;
    }
#endif

    // Conker runs with the FPU's FR bit set (32 independent float registers), which
    // Rare's hand-written math relies on; the boot code's own write is patched out.
    void set_fr_mode(recomp_context* ctx) {
        cop0_status_write(ctx, ctx->status_reg | 0x04000000);
    }

    void on_thread_create(uint8_t*, recomp_context* ctx) {
        set_fr_mode(ctx);
    }

    void on_init(uint8_t* rdram, recomp_context* ctx) {
        g_rdram = rdram;
        set_fr_mode(ctx);
        conker::register_tlb_code();
        conker::map_tlb_pages(rdram);
        // osCicId, normally left by the boot chip code: Conker's idle thread only
        // starts the main thread when it reads 6105.
        MEM_W(0, (int32_t)0x80000310) = 6105;
        // Game code reads libultra's __osRunningThread directly.
        ultramodern::set_running_thread_variable((int32_t)0x8002BE00);
    }

    RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
        if (task->t.type == M_AUDTASK) {
            return conker_audio_ucode;
        }
        std::fprintf(stderr, "[host] no RSP microcode for task type %u\n", (unsigned)task->t.type);
        return nullptr;
    }

    void on_vi() {
        // CONKER_PROBE=1: every half second, print the game's frame timer and room (debugging).
        static const bool probe = std::getenv("CONKER_PROBE") != nullptr;
        if (probe && g_rdram != nullptr && vi_count % 30 == 0) {
            uint8_t* rdram = g_rdram;
            std::printf("[probe] vi=%u timer=%d room=%08X stream=%d/%d\n", vi_count.load(),
                MEM_W(0, (int32_t)0x800E0A90), (uint32_t)MEM_W(0, (int32_t)0x800BE9F0), MEM_W(0, (int32_t)0x800E0E04),
                (int)MEM_H(0, (int32_t)0x800E0E14));
        }
        ++vi_count;
        conker::frontend::on_vi();
        conker::testing::on_vi(g_rdram);
        conker::qol::on_vi();
        if (g_rdram != nullptr) {
            conker::skip_intro_on_vi(g_rdram);
            conker::qol::on_vi_memory(g_rdram);
            conker::game_menu::on_vi(g_rdram);
        }
    }

    std::filesystem::path exe_directory() {
#ifdef _WIN32
        wchar_t buffer[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            return std::filesystem::path(buffer).parent_path();
        }
#endif
        return std::filesystem::current_path();
    }

    // The first run starts from Conker BFD: Reloaded's settings and saves if it was played on this
    // PC (the same files), so a player moving over keeps their progress. Not in portable mode.
    void adopt_reloaded_data(const std::filesystem::path& ours) {
        std::error_code ec;
        if (std::filesystem::exists(ours, ec)) {
            return;
        }
        const std::filesystem::path theirs = ours.parent_path() / conker::reloaded_program_id;
        if (std::filesystem::is_directory(theirs, ec)) {
            std::filesystem::copy(theirs, ours, std::filesystem::copy_options::recursive, ec);
            std::printf("[host] settings and saves copied from %s%s\n", theirs.string().c_str(), ec ? " (not all of them)" : "");
        }
    }

    bool is_rom_file(const std::filesystem::path& path) {
        std::string ext = path.extension().string();
        for (char& c : ext) {
            c = (char)std::tolower((unsigned char)c);
        }
        return ext == ".z64" || ext == ".n64" || ext == ".v64";
    }

    // A ROM next to the program is taken without asking (select_rom turns byte-swapped dumps
    // around, and keeps a copy with the game's data).
    bool find_rom_next_to_exe() {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(exe_directory(), ec)) {
            if (entry.is_regular_file(ec) && is_rom_file(entry.path()) &&
                recomp::select_rom(entry.path(), game_id) == recomp::RomValidationError::Good) {
                std::printf("[host] using the ROM next to the program: %s\n", entry.path().filename().string().c_str());
                return true;
            }
        }
        return false;
    }

    // Otherwise the player is asked for it, once. False if they cancel.
    bool ask_for_rom() {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, conker::program_name,
            "Welcome! Choose your copy of Conker's Bad Fur Day (the US version) to play.\n\n"
            "It's only needed this once: the game keeps it with its data.", nullptr);
        if (NFD_Init() != NFD_OKAY) {
            return false;
        }
        bool chosen = false;
        while (!chosen) {
            nfdu8char_t* out = nullptr;
            const nfdu8filteritem_t filter = { "N64 ROM", "z64,n64,v64" };
            if (NFD_OpenDialogU8(&out, &filter, 1, nullptr) != NFD_OKAY) {
                break;
            }
            const std::filesystem::path path{ std::u8string{ reinterpret_cast<const char8_t*>(out) } };
            NFD_FreePathU8(out);
            if (recomp::select_rom(path, game_id) == recomp::RomValidationError::Good) {
                chosen = true;
            } else {
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, conker::program_name,
                    "That isn't the US version of Conker's Bad Fur Day. Please choose another file.", nullptr);
            }
        }
        NFD_Quit();
        return chosen;
    }

    bool shift_held() {
#ifdef _WIN32
        return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
        return false;
#endif
    }

    // The US ROM, or a ROM hack of it that only changes the game's data (the uncensored one).
    bool accept_rom(std::span<const uint8_t> rom) {
        if (XXH3_64bits(rom.data(), rom.size()) == conker::us_rom_hash) {
            return true;
        }
        return rom.size() >= conker::rom_code_end &&
            XXH3_64bits(rom.data() + conker::rom_code_start, conker::rom_code_end - conker::rom_code_start) == conker::us_code_hash;
    }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
#endif
    std::filesystem::path rom_path;
    int seconds = 0;
    bool launcher_wanted = shift_held();
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (std::strcmp(argv[i], "--launcher") == 0) {
            launcher_wanted = true;
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else if (argv[i][0] != '-' && rom_path.empty()) {
            rom_path = argv[i]; // a ROM dropped onto the exe
        }
    }

    // recompui loads assets/ relative to the working directory.
    std::filesystem::current_path(exe_directory());
    recompui::programconfig::set_program_id(conker::program_id);
    adopt_reloaded_data(recompui::file::get_app_folder_path());
    recomp::register_config_path(recompui::file::get_app_folder_path());
    std::filesystem::create_directories(recomp::get_config_path());
#ifdef _WIN32
    crash_log_path = recomp::get_config_path() / "crash.log";
#endif

    recomp::GameEntry game{};
    game.rom_hash = conker::us_rom_hash;
    game.accept_rom = accept_rom;
    game.internal_name = "CONKER BFD";
    game.display_name = "Conker's Bad Fur Day";
    game.game_id = game_id;
    game.mod_game_id = "conker";
    game.save_type = recomp::SaveType::Eep16k;
    game.is_enabled = true;
    game.has_compressed_code = true;
    game.decompression_routine = conker::unpacked_rom;
    game.entrypoint_address = (gpr)(int32_t)0x80001000u;
    game.entrypoint = recomp_entrypoint;
    game.on_init_callback = on_init;
    game.thread_create_callback = on_thread_create;
    conker::frontend::init(game);
    recomp::register_game(game);
    conker::register_code_sections();

    // Texture packs (RT64's: an rt64.json listing replacement textures) as mods, in .rtz archives
    // in the mods folder, switched on and off in the Mods menu while playing.
    recomp::mods::ModContentType texture_pack_content{};
    texture_pack_content.content_filename = "rt64.json";
    texture_pack_content.allow_runtime_toggle = true;
    texture_pack_content.on_enabled = [](recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
        recompui::renderer::enable_texture_pack(context, mod);
    };
    texture_pack_content.on_disabled = [](recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
        recompui::renderer::disable_texture_pack(mod);
    };
    texture_pack_content.on_reordered = [](recomp::mods::ModContext&) {
        recompui::renderer::trigger_texture_pack_update();
    };
    const recomp::mods::ModContentTypeId texture_pack_type = recomp::mods::register_mod_content_type(texture_pack_content);
    recomp::mods::register_mod_container_type("rtz", { texture_pack_type }, false);

    bool start_directly = seconds > 0;
    if (!rom_path.empty()) {
        recomp::RomValidationError result = recomp::select_rom(rom_path, game_id);
        if (result != recomp::RomValidationError::Good) {
            std::fprintf(stderr, "[host] %s isn't the US ROM of Conker's Bad Fur Day (or a hack of it that keeps its code)\n", rom_path.string().c_str());
            return EXIT_FAILURE;
        }
    } else {
        recomp::check_all_stored_roms();
        if (start_directly && !recomp::is_rom_valid(game_id)) {
            std::fprintf(stderr, "[host] no ROM yet: run once with --rom <path>\n");
            return EXIT_FAILURE;
        }
        // Click and play: no ROM yet, so one next to the program, or ask (the launcher can still
        // load one if the player cancels and holds Shift next time).
        if (!recomp::is_rom_valid(game_id) && !launcher_wanted && !find_rom_next_to_exe() && !ask_for_rom()) {
            return EXIT_SUCCESS;
        }
    }
    // select_rom keeps the ROM with the game's data; this marks it valid.
    recomp::check_all_stored_roms();

    // Straight into the game, unless the launcher was asked for (Shift held, or --launcher). The
    // settings are in the game too (the View button or Esc).
    if (!launcher_wanted && recomp::is_rom_valid(game_id)) {
        start_directly = true;
    }

    std::vector<char*> runtime_argv{ argv[0] };
    if (start_directly) {
        runtime_argv.push_back((char*)"--game");
        runtime_argv.push_back((char*)"conker");
    }

    recomp::Configuration cfg{};
    cfg.argc = (int)runtime_argv.size();
    cfg.argv = runtime_argv.data();
    // Our own numbering (host/CMakeLists.txt).
    cfg.project_version = recomp::Version{ CONKER_VERSION_MAJOR, CONKER_VERSION_MINOR, CONKER_VERSION_PATCH, CONKER_VERSION_SUFFIX };
    cfg.rsp_callbacks.get_rsp_microcode = get_rsp_microcode;
    cfg.audio_callbacks = { conker::sound::queue_samples, conker::sound::frames_remaining, conker::sound::set_frequency };
    cfg.events_callbacks = { on_vi, nullptr };
    conker::frontend::set_callbacks(cfg);

    std::thread timer;
    if (seconds > 0) {
        timer = std::thread([seconds] {
            // Game time (60 screen refreshes a second), or wall time if the game stalls.
            auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(seconds + 30);
            while (vi_count < (uint32_t)seconds * 60 && std::chrono::steady_clock::now() < give_up) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            std::printf("[host] %d seconds, %u VIs; quitting\n", seconds, vi_count.load());
            ultramodern::quit();
        });
    }

    conker::testing::init();
    recomp::start(cfg);
    NFD_Quit();
    if (timer.joinable()) {
        timer.join();
    }
    return EXIT_SUCCESS;
}
