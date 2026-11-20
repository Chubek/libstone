#include "boot_display.hpp"

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <algorithm>
#include <cstring>

namespace domvirt {

BootDisplay::BootDisplay(bool headless) {
    if (!headless) thread_ = std::thread(&BootDisplay::run, this);
}

BootDisplay::~BootDisplay() { stop(); }

void BootDisplay::stop() {
    stop_requested_.store(true);
    if (thread_.joinable()) thread_.join();
}

void BootDisplay::set_status(std::string status, float progress) {
    std::lock_guard lock(state_mutex_);
    status_ = std::move(status);
    progress_ = std::clamp(progress, 0.0f, 1.0f);
}

void BootDisplay::append_message(std::string message) {
    std::lock_guard lock(state_mutex_);
    messages_.push_back(std::move(message));
    if (messages_.size() > 8) messages_.erase(messages_.begin());
}

void BootDisplay::set_guest_framebuffer(const void* base, unsigned width, unsigned height,
                                       unsigned pitch, std::uint32_t format) {
    std::lock_guard lock(framebuffer_mutex_);
    const unsigned bpp = format == 3 ? 2 : 4;
    guest_framebuffer_ = base && width && height && width <= 8192 && height <= 8192 &&
        pitch >= width * bpp && pitch <= 32768 && format >= 1 && format <= 3
        ? static_cast<const std::uint8_t*>(base) : nullptr;
    guest_width_ = width;
    guest_height_ = height;
    guest_pitch_ = pitch;
    guest_format_ = format;
    if (guest_framebuffer_) {
        framebuffer_copy_.assign(guest_framebuffer_, guest_framebuffer_ + static_cast<std::size_t>(pitch) * height);
        guest_framebuffer_ = framebuffer_copy_.data();
    }
}

void BootDisplay::set_guest_text_buffer(const void* base, unsigned columns, unsigned rows,
                                      unsigned pitch) {
    std::lock_guard lock(framebuffer_mutex_);
    guest_text_buffer_ = base && columns && rows && columns <= 256 && rows <= 128 &&
        pitch >= columns * 2 && pitch <= 4096 ? static_cast<const std::uint8_t*>(base) : nullptr;
    guest_text_columns_ = columns;
    guest_text_rows_ = rows;
    guest_text_pitch_ = pitch;
    if (guest_text_buffer_) {
        text_copy_.assign(guest_text_buffer_, guest_text_buffer_ + static_cast<std::size_t>(pitch) * rows);
        guest_text_buffer_ = text_copy_.data();
    }
}

bool BootDisplay::try_read_input(std::uint8_t& value) {
    std::lock_guard lock(input_mutex_);
    if (input_queue_.empty()) return false;
    value = input_queue_.front();
    input_queue_.pop_front();
    return true;
}

void BootDisplay::enqueue_input(std::uint8_t value) {
    std::lock_guard lock(input_mutex_);
    if (input_queue_.size() < 4096) input_queue_.push_back(value);
}

void BootDisplay::run() {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        if (!SDL_WasInit(0)) SDL_Quit();
        SDL_TLSCleanup();
        return;
    }
    auto* window = SDL_CreateWindow("ArchVirt", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    auto* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!renderer) {
        if (window) SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        if (!SDL_WasInit(0)) SDL_Quit();
        SDL_TLSCleanup();
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    SDL_StartTextInput();
    available_.store(true);
    SDL_Texture* texture = nullptr;
    unsigned texture_width = 0, texture_height = 0;
    std::vector<std::uint32_t> pixels;
    while (!stop_requested_.load()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                closed_.store(true);
                stop_requested_.store(true);
            } else if (event.type == SDL_TEXTINPUT) {
                for (const unsigned char* p = reinterpret_cast<unsigned char*>(event.text.text); *p; ++p)
                    enqueue_input(*p);
            } else if (event.type == SDL_KEYDOWN) {
                auto key = event.key.keysym.sym;
                if ((event.key.keysym.mod & KMOD_CTRL) && key >= SDLK_F1 && key <= SDLK_F4)
                    enqueue_input(static_cast<std::uint8_t>(0x11 + key - SDLK_F1));
                else if (key == SDLK_RETURN) enqueue_input('\n');
                else if (key == SDLK_BACKSPACE) enqueue_input('\b');
                else if (key == SDLK_TAB) enqueue_input('\t');
            }
        }
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        const auto size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("ArchVirt", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
        {
            std::lock_guard lock(state_mutex_);
            ImGui::TextUnformatted(status_.c_str());
            ImGui::ProgressBar(progress_, ImVec2(-1, 0));
        }
        {
            // Keep the binding locked until the last read so unbinding before RAM release is safe.
            std::lock_guard lock(framebuffer_mutex_);
            if (guest_framebuffer_) {
                if (!texture || texture_width != guest_width_ || texture_height != guest_height_) {
                    if (texture) SDL_DestroyTexture(texture);
                    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING, guest_width_, guest_height_);
                    texture_width = guest_width_;
                    texture_height = guest_height_;
                }
                pixels.resize(static_cast<std::size_t>(guest_width_) * guest_height_);
                for (unsigned y = 0; y < guest_height_; ++y) {
                    for (unsigned x = 0; x < guest_width_; ++x) {
                        std::uint32_t p = 0;
                        const unsigned bpp = guest_format_ == 3 ? 2 : 4;
                        std::memcpy(&p, guest_framebuffer_ + y * guest_pitch_ + x * bpp, bpp);
                        if (guest_format_ == 2)
                            p = ((p & 255) << 16) | (p & 0xff00) | ((p >> 16) & 255);
                        if (guest_format_ == 3)
                            p = (((p >> 11) & 31) * 255 / 31 << 16) |
                                (((p >> 5) & 63) * 255 / 63 << 8) | ((p & 31) * 255 / 31);
                        pixels[y * guest_width_ + x] = p | 0xff000000u;
                    }
                }
                if (texture) {
                    SDL_UpdateTexture(texture, nullptr, pixels.data(), guest_width_ * 4);
                    const auto room = ImGui::GetContentRegionAvail();
                    float scale = std::min(room.x / guest_width_, room.y / guest_height_);
                    ImGui::Image((ImTextureID)(intptr_t)texture,
                                 ImVec2(guest_width_ * scale, guest_height_ * scale));
                }
            } else if (guest_text_buffer_) {
                static constexpr ImU32 colors[] = {
                    0xff000000, 0xffaa0000, 0xff00aa00, 0xffaaaa00,
                    0xff0000aa, 0xffaa00aa, 0xff0055aa, 0xffaaaaaa,
                    0xff555555, 0xffff5555, 0xff55ff55, 0xffffff55,
                    0xff5555ff, 0xffff55ff, 0xff55ffff, 0xffffffff};
                auto* draw = ImGui::GetWindowDrawList();
                const auto origin = ImGui::GetCursorScreenPos();
                const float cell_w = ImGui::CalcTextSize("M").x;
                const float cell_h = ImGui::GetTextLineHeight();
                for (unsigned y = 0; y < guest_text_rows_; ++y)
                    for (unsigned x = 0; x < guest_text_columns_; ++x) {
                        const auto* cell = guest_text_buffer_ + y * guest_text_pitch_ + x * 2;
                        const ImVec2 at(origin.x + x * cell_w, origin.y + y * cell_h);
                        draw->AddRectFilled(at, ImVec2(at.x + cell_w, at.y + cell_h), colors[cell[1] >> 4]);
                        const char ch[] = {cell[0] >= 32 && cell[0] < 127 ? static_cast<char>(cell[0]) : ' ', 0};
                        draw->AddText(at, colors[cell[1] & 15], ch);
                    }
                ImGui::Dummy(ImVec2(cell_w * guest_text_columns_, cell_h * guest_text_rows_));
                ImGui::TextUnformatted("Ctrl+F1..F4: console");
            } else {
                std::lock_guard state_lock(state_mutex_);
                for (const auto& message : messages_) ImGui::TextUnformatted(message.c_str());
            }
        }
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 12, 14, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        SDL_Delay(10);
    }
    if (texture) SDL_DestroyTexture(texture);
    SDL_StopTextInput();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (!SDL_WasInit(0)) SDL_Quit();
    SDL_TLSCleanup();
    available_.store(false);
}

} // namespace domvirt
