#include "app/launcher/LauncherWindow.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "app/DataDir.h"
#include "app/Files.h"
#include "app/ImportFiles.h"
#include "app/launcher/Launcher.h"
#include "app/launcher/LauncherJobs.h"
#include "app/launcher/LauncherSetup.h"
#include "app/launcher/LauncherText.h"

namespace sfcop::app::launcher {

namespace {

constexpr int kWidth = 760;
constexpr int kHeight = 560;
constexpr float kFontSize = 17.0f;
constexpr Uint64 kWaitLimitMs = 10ull * 60ull * 1000ull;
constexpr std::size_t kPathCapacity = 4096;

// When a file was last written, as the Ready screen shows it ("2026-11-12 14:03"), in local time.
std::string modifiedTimeOf(const std::string &path) {
    std::error_code error;
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(path, error);
    if (error) {
        return std::string();
    }
    // C++17 has no clock_cast: carry the file clock's offset from now over to the system clock.
    const auto system = std::chrono::system_clock::now() +
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(
                            written - std::filesystem::file_time_type::clock::now());
    const std::time_t seconds = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &seconds) != 0) {
        return std::string();
    }
#else
    if (localtime_r(&seconds, &local) == nullptr) {
        return std::string();
    }
#endif
    char text[32];
    if (std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local) == 0) {
        return std::string();
    }
    return text;
}

// Whether `program` is on PATH, for the folder picker's zenity fallback on Linux.
bool onPath(const EnvLookup &env, const std::string &program) {
    const std::optional<std::string> path = env ? env("PATH") : std::nullopt;
    if (!path) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path->size()) {
        std::size_t end = path->find(':', start);
        if (end == std::string::npos) {
            end = path->size();
        }
        if (end > start) {
            // A regular file someone may run, as a shell's PATH search would want.
            std::error_code error;
            const std::filesystem::path candidate =
                std::filesystem::path(path->substr(start, end - start)) / program;
            const std::filesystem::file_status status = std::filesystem::status(candidate, error);
            const auto runnable = std::filesystem::perms::owner_exec |
                                  std::filesystem::perms::group_exec |
                                  std::filesystem::perms::others_exec;
            if (!error && std::filesystem::is_regular_file(status) &&
                (status.permissions() & runnable) != std::filesystem::perms::none) {
                return true;
            }
        }
        start = end + 1;
    }
    return false;
}

ImVec4 toneColour(Tone tone) {
    switch (tone) {
    case Tone::kSuccess:
        return ImVec4(0.47f, 0.84f, 0.52f, 1.0f);
    case Tone::kWarning:
        return ImVec4(0.96f, 0.76f, 0.35f, 1.0f);
    case Tone::kError:
        return ImVec4(0.98f, 0.47f, 0.43f, 1.0f);
    case Tone::kPlain:
        break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

void applyStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(28.0f, 24.0f);
    style.FramePadding = ImVec2(12.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.FrameRounding = 5.0f;
    style.ChildRounding = 5.0f;
    style.ScrollbarSize = 14.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.13f, 0.16f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.19f, 0.23f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.23f, 0.28f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.27f, 0.33f, 1.0f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.30f, 0.33f, 0.40f, 1.0f);
    style.FrameBorderSize = 1.0f;
    style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.24f, 0.29f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.29f, 0.32f, 0.38f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.34f, 0.37f, 0.44f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.30f, 0.56f, 0.90f, 1.0f);
    style.ScaleAllSizes(scale);
    style.FontSizeBase = kFontSize;
    style.FontScaleDpi = scale;
}

// The window, its renderer and Dear ImGui, released in reverse order on every way out.
class Surface {
public:
    ~Surface() {
        if (imguiRenderer_) {
            ImGui_ImplSDLRenderer3_Shutdown();
        }
        if (imguiPlatform_) {
            ImGui_ImplSDL3_Shutdown();
        }
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::DestroyContext();
        }
        if (renderer_ != nullptr) {
            SDL_DestroyRenderer(renderer_);
        }
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
        }
        if (video_) {
            // Nothing the launcher queued reaches the game's event loop.
            SDL_PumpEvents();
            SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    // False, with the reason on `why`, when no window can be opened here.
    bool open(std::string &why) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            why = SDL_GetError();
            return false;
        }
        video_ = true;
        const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        const float dpi = scale > 0.0f ? scale : 1.0f;
        if (!SDL_CreateWindowAndRenderer(text::kWindowTitle, static_cast<int>(kWidth * dpi),
                                         static_cast<int>(kHeight * dpi),
                                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                         &window_, &renderer_)) {
            why = SDL_GetError();
            return false;
        }
        SDL_SetRenderVSync(renderer_, 1);
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_ShowWindow(window_);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr; // nothing written beside the client
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontDefaultVector();
        applyStyle(dpi);
        imguiPlatform_ = ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
        imguiRenderer_ = imguiPlatform_ && ImGui_ImplSDLRenderer3_Init(renderer_);
        if (!imguiRenderer_) {
            why = "Dear ImGui could not start on the SDL renderer";
            return false;
        }
        return true;
    }

    SDL_Window *window() const { return window_; }
    SDL_Renderer *renderer() const { return renderer_; }

private:
    bool video_ = false;
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    bool imguiPlatform_ = false;
    bool imguiRenderer_ = false;
};

// Plays `--launcher-steps` one step at a time, between frames.
class StepPlayer {
public:
    StepPlayer(const std::vector<Step> &steps, const std::string &name)
        : steps_(steps), name_(name) {}

    bool done() const { return next_ >= steps_.size(); }
    const std::string &error() const { return error_; }
    // A shot asked for, to be taken of the frame drawn next; cleared by the caller.
    std::string shot;

    // One frame's steps: any number of waits that are met and types, then at most one action.
    void advance(Launcher &launcher, char *path, std::size_t capacity) {
        while (!done() && error_.empty()) {
            const Step &step = steps_[next_];
            switch (step.kind) {
            case StepKind::kWait:
                if (launcher.view().screen == step.screen) {
                    next();
                    continue;
                }
                if (waitStarted_ == 0) {
                    waitStarted_ = SDL_GetTicks();
                } else if (SDL_GetTicks() - waitStarted_ > kWaitLimitMs) {
                    fail(step, "the screen did not show in 10 minutes");
                }
                return;
            case StepKind::kFrames:
                if (framesLeft_ == 0) {
                    framesLeft_ = step.frames;
                }
                if (--framesLeft_ == 0) {
                    next();
                }
                return;
            case StepKind::kType:
                std::snprintf(path, capacity, "%s", step.text.c_str());
                next();
                continue;
            case StepKind::kChoose:
                if (!launcher.view().acceptsDrop || launcher.busy()) {
                    fail(step, "no folder can be chosen on this screen");
                    return;
                }
                launcher.choose(step.text);
                next();
                return;
            case StepKind::kPress: {
                bool shown = false;
                for (const Button &button : launcher.view().buttons) {
                    shown = shown || button.action == step.action;
                }
                if (!shown) {
                    fail(step, "the screen shows no such button");
                    return;
                }
                launcher.press(step.action, path);
                next();
                return;
            }
            case StepKind::kClose:
                launcher.close();
                next();
                return;
            case StepKind::kShot:
                shot = step.text;
                next();
                return;
            }
        }
    }

private:
    void next() {
        ++next_;
        waitStarted_ = 0;
        framesLeft_ = 0;
    }
    void fail(const Step &step, const std::string &what) {
        error_ = name_ + " line " + std::to_string(step.line) + ": " + what;
    }

    const std::vector<Step> &steps_;
    const std::string &name_;
    std::size_t next_ = 0;
    Uint64 waitStarted_ = 0;
    unsigned framesLeft_ = 0;
    std::string error_;
};

void wrapped(const std::string &line, std::optional<ImVec4> colour = std::nullopt) {
    if (colour) {
        ImGui::PushStyleColor(ImGuiCol_Text, *colour);
    }
    ImGui::TextWrapped("%s", line.c_str());
    if (colour) {
        ImGui::PopStyleColor();
    }
}

void heading(const std::string &title, float scale) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
    wrapped(title);
    ImGui::PopFont();
}

// What the player did this frame, beyond the buttons.
struct Pressed {
    std::optional<Action> action;
    std::optional<std::string> suggestion;
};

void drawWelcome(const View &view, char *path, std::size_t capacity, Pressed &pressed) {
    if (!view.suggestions.empty()) {
        ImGui::Spacing();
        wrapped(text::kSuggestionsHeading);
        for (const Suggestion &suggestion : view.suggestions) {
            const std::string label =
                suggestion.path + "  (" +
                (suggestion.holdsGame ? text::kSuggestionHoldsGame : text::kSuggestionLookBelow) +
                ")";
            if (ImGui::Selectable(label.c_str())) {
                pressed.suggestion = suggestion.path;
            }
        }
    }
    if (view.pathField) {
        ImGui::Spacing();
        wrapped(text::kPathFieldLabel);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##path", path, capacity, ImGuiInputTextFlags_EnterReturnsTrue)) {
            pressed.action = Action::kUseTypedPath;
        }
    }
    if (view.acceptsDrop) {
        wrapped(text::kDropHint, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    if (!view.pickerNote.empty()) {
        wrapped(view.pickerNote, toneColour(Tone::kWarning));
    }
}

void drawWorking(const View &view) {
    ImGui::Spacing();
    wrapped(view.phase);
    const float barHeight = ImGui::GetFrameHeight() * 1.2f;
    if (view.fraction) {
        ImGui::ProgressBar(static_cast<float>(*view.fraction), ImVec2(-1.0f, barHeight),
                           view.amount.empty() ? nullptr : view.amount.c_str());
    } else {
        // A negative fraction is Dear ImGui's indeterminate bar.
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()),
                           ImVec2(-1.0f, barHeight),
                           view.amount.empty() ? nullptr : view.amount.c_str());
    }
    if (!view.file.empty()) {
        wrapped(view.file, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
}

// The message and the lists, in a region that scrolls when they are long.
void drawBody(const View &view, float buttonsHeight) {
    const bool scrolls = view.screen == Screen::kResult || !view.lists.empty();
    if (scrolls) {
        ImGui::BeginChild("##body", ImVec2(-1.0f, -buttonsHeight), ImGuiChildFlags_Borders);
    }
    const std::optional<ImVec4> colour =
        view.tone == Tone::kPlain ? std::nullopt : std::optional<ImVec4>(toneColour(view.tone));
    for (std::size_t i = 0; i < view.paragraphs.size(); ++i) {
        // The first paragraph carries the tone; the rest are the report's detail.
        wrapped(view.paragraphs[i], i == 0 ? colour : std::nullopt);
    }
    for (const ReportList &list : view.lists) {
        ImGui::Spacing();
        wrapped(list.heading, toneColour(list.tone));
        for (const std::string &line : list.lines) {
            ImGui::BulletText("%s", line.c_str());
        }
    }
    if (scrolls) {
        ImGui::EndChild();
    }
}

std::optional<Action> drawButtons(const View &view) {
    std::optional<Action> pressed;
    bool first = true;
    for (const Button &button : view.buttons) {
        if (!first) {
            ImGui::SameLine();
        }
        first = false;
        if (button.primary) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.53f, 0.90f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.40f, 0.72f, 1.0f));
        }
        if (ImGui::Button(button.label.c_str())) {
            pressed = button.action;
        }
        if (button.primary) {
            ImGui::PopStyleColor(3);
        }
    }
    return pressed;
}

Pressed drawView(const View &view, char *path, std::size_t capacity) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    Pressed pressed;
    heading(view.title, 1.45f);
    ImGui::Separator();
    ImGui::Spacing();
    const ImGuiStyle &style = ImGui::GetStyle();
    // The body's scrolling region leaves room for the buttons and for what is drawn below it.
    const float wrapWidth = ImGui::GetContentRegionAvail().x;
    const auto textHeight = [&](const std::string &text) {
        return ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapWidth).y +
               style.ItemSpacing.y;
    };
    float below = ImGui::GetFrameHeight() + style.ItemSpacing.y * 3.0f;
    if (view.screen == Screen::kResult && view.acceptsDrop) {
        below += textHeight(text::kDropHint);
    }
    if (!view.note.empty()) {
        below += textHeight(view.note) + style.ItemSpacing.y;
    }
    drawBody(view, below);
    switch (view.screen) {
    case Screen::kWelcome:
        drawWelcome(view, path, capacity, pressed);
        break;
    case Screen::kWorking:
        drawWorking(view);
        break;
    case Screen::kResult:
        if (view.acceptsDrop) {
            wrapped(text::kDropHint, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }
        break;
    case Screen::kReady:
    case Screen::kClosing:
        break;
    }
    if (!view.note.empty()) {
        ImGui::Spacing();
        wrapped(view.note, view.cancelling ? std::nullopt
                                           : std::optional<ImVec4>(toneColour(Tone::kWarning)));
    }
    // The buttons sit at the foot of the window.
    const float foot = ImGui::GetWindowHeight() - style.WindowPadding.y - ImGui::GetFrameHeight();
    if (ImGui::GetCursorPosY() < foot) {
        ImGui::SetCursorPosY(foot);
    }
    if (const std::optional<Action> action = drawButtons(view)) {
        pressed.action = action;
    }
    ImGui::End();
    return pressed;
}

bool saveShot(SDL_Renderer *renderer, const std::string &file, std::string &why) {
    SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr);
    if (surface == nullptr) {
        why = SDL_GetError();
        return false;
    }
    const bool saved = SDL_SaveBMP(surface, file.c_str());
    if (!saved) {
        why = SDL_GetError();
    }
    SDL_DestroySurface(surface);
    return saved;
}

} // namespace

void SDLCALL pickerAnswered(void *userdata, const char *const *filelist, int /*filter*/) {
    std::unique_ptr<std::shared_ptr<PickerInbox>> holder(
        static_cast<std::shared_ptr<PickerInbox> *>(userdata));
    PickerInbox &inbox = **holder;
    const std::lock_guard<std::mutex> lock(inbox.mutex);
    inbox.open = false;
    if (filelist == nullptr) {
        inbox.failed = SDL_GetError();
    } else if (filelist[0] != nullptr) {
        inbox.chosen = filelist[0];
    }
    // An empty list: the player closed the picker without choosing; nothing to do.
}

void openFolderPicker(const std::shared_ptr<PickerInbox> &inbox, const ShowFolderPicker &show) {
    {
        const std::lock_guard<std::mutex> lock(inbox->mutex);
        if (inbox->open) {
            return;
        }
        inbox->open = true;
    }
    show(pickerAnswered, new std::shared_ptr<PickerInbox>(inbox));
}

LauncherChoice runLauncherWindow(const LauncherRequest &request, const WindowOptions &options,
                                 std::ostream &err) {
    FileSystem &files = request.files != nullptr ? *request.files : hostFileSystem();
    ImportFiles &importFiles =
        request.importFiles != nullptr ? *request.importFiles : hostImportFiles();
    const FolderPicker picker = folderPickerOn(
        request.platform, request.env,
        [&request](const std::string &program) { return onPath(request.env, program); });

    Environment environment;
    environment.rememberImport = [&request, &files]() {
        return rememberInstall(request.dataDir, kImportedGame, files);
    };
    environment.gameInUse = [&request, &files]() {
        return gameInUseOf(request.dataDir, request.gameDir, files, modifiedTimeOf);
    };
    environment.absolute = [&files](const std::string &path) { return files.absolutePath(path); };

    // The jobs outlive the surface: a job still running when the window goes is cancelled and
    // joined before anything it reports to is destroyed.
    FilesLibrary library(importFiles);
    WorkerJobs jobs(library);
    Launcher launcher(setupFor(request, picker), environment, jobs);

    std::optional<LauncherChoice> result;
    {
        Surface surface;
        std::string why;
        if (!surface.open(why)) {
            err << "sfcop: the launcher's window cannot open: " << why << "\n";
            return LauncherChoice::kNoWindow;
        }
        const auto inbox = std::make_shared<PickerInbox>();
        char path[kPathCapacity] = {};
        StepPlayer player(options.steps, options.stepsName);
        const bool scripted = !options.steps.empty();

        while (!result) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    launcher.close();
                    break;
                case SDL_EVENT_DROP_FILE:
                    if (event.drop.data != nullptr && launcher.view().acceptsDrop &&
                        !launcher.busy()) {
                        launcher.choose(event.drop.data);
                    }
                    break;
                default:
                    break;
                }
            }
            {
                const std::lock_guard<std::mutex> lock(inbox->mutex);
                if (inbox->chosen) {
                    std::snprintf(path, sizeof path, "%s", inbox->chosen->c_str());
                    launcher.choose(*inbox->chosen);
                    inbox->chosen.reset();
                }
                if (inbox->failed) {
                    launcher.pickerFailed(*inbox->failed);
                    inbox->failed.reset();
                }
            }
            JobEvents events = jobs.take();
            if (events.progress) {
                launcher.onProgress(*events.progress);
            }
            if (events.finished) {
                launcher.onFinished(*events.finished);
            }
            if (scripted) {
                player.advance(launcher, path, sizeof path);
                if (!player.error().empty()) {
                    err << "sfcop: " << player.error() << "\n";
                    launcher.close();
                }
            }

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            const Pressed pressed = drawView(launcher.view(), path, sizeof path);
            ImGui::Render();
            const ImGuiIO &io = ImGui::GetIO();
            SDL_SetRenderScale(surface.renderer(), io.DisplayFramebufferScale.x,
                               io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(surface.renderer(), 23, 25, 31, 255);
            SDL_RenderClear(surface.renderer());
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), surface.renderer());
            if (!player.shot.empty()) {
                if (!saveShot(surface.renderer(), player.shot, why)) {
                    err << "sfcop: " << player.shot << " cannot be written: " << why << "\n";
                }
                player.shot.clear();
            }
            SDL_RenderPresent(surface.renderer());

            if (pressed.suggestion) {
                std::snprintf(path, sizeof path, "%s", pressed.suggestion->c_str());
            }
            if (pressed.action == Action::kChooseFolder) {
                openFolderPicker(inbox, [&](SDL_DialogFileCallback callback, void *userdata) {
                    SDL_ShowOpenFolderDialog(callback, userdata, surface.window(),
                                             path[0] != '\0' ? path : nullptr, false);
                });
            } else if (pressed.action) {
                launcher.press(*pressed.action, path);
            }

            if (!player.error().empty() && launcher.outcome() != Outcome::kRunning) {
                result = LauncherChoice::kFailed;
            } else if (launcher.outcome() == Outcome::kPlay) {
                result = LauncherChoice::kPlay;
            } else if (launcher.outcome() == Outcome::kQuit) {
                result = LauncherChoice::kQuit;
            }
        }
    }
    if (!options.steps.empty()) {
        // For the captures: the game's window opens after this, on a clean video subsystem.
        err << "sfcop: the launcher's window is closed\n";
    }
    return *result;
}

std::optional<WindowOptions> readWindowOptions(const std::string &stepsPath, std::ostream &err) {
    WindowOptions options;
    if (stepsPath.empty()) {
        return options;
    }
    options.stepsName = stepsPath;
    std::ifstream file(stepsPath, std::ios::binary);
    std::ostringstream text;
    if (!file || !(text << file.rdbuf())) {
        err << "sfcop: cannot read the launcher steps " << stepsPath << "\n";
        return std::nullopt;
    }
    ParsedSteps parsed = parseLauncherSteps(text.str());
    if (!parsed.error.empty()) {
        err << "sfcop: launcher steps " << stepsPath << ", " << parsed.error << "\n";
        return std::nullopt;
    }
    options.steps = std::move(parsed.steps);
    return options;
}

std::function<LauncherChoice(const LauncherRequest &)> windowLauncher(std::string stepsPath) {
    return [stepsPath = std::move(stepsPath)](const LauncherRequest &request) {
        const std::optional<WindowOptions> options = readWindowOptions(stepsPath, std::cerr);
        if (!options) {
            return LauncherChoice::kFailed;
        }
        return runLauncherWindow(request, *options, std::cerr);
    };
}

} // namespace sfcop::app::launcher
