// The setup dialog - see ui.h for what this is and why it is a separate window.
#include "ui.h"

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "rhi/vk_core.h" // phxvk::LogInfo/LogWarn - a printf wrapper, safe pre-device

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace phxplayer
{
namespace
{

//////////////////////////////////////////////////////////////////////////////
// The two combos' contents
//////////////////////////////////////////////////////////////////////////////

// One entry of the "Resolution" combo, which the original builds with
// EnumDisplaySettings ("%d x %d %dbpp @ %dhz", SetupDialog.cpp:91).
struct DisplayMode
{
  int width = 0;
  int height = 0;
  int hz = 0;
};

// SetupDialog.cpp:55-56 - the same nine presets in the same order, because
// entry i of the combo has to mean what it meant in the release player.  The
// original's `AspectCnt` is 10: the "(auto)" entry plus these nine.
const int kAspectX[] = {4, 5, 16, 21, 1, 16, 3, 17, 64};
const int kAspectY[] = {3, 4, 9, 9, 1, 10, 2, 9, 27};
const int kAspectCount = (int)(sizeof(kAspectX) / sizeof(kAspectX[0]));

int Gcd(int x, int y) { return y == 0 ? x : Gcd(y, x % y); }

// The display's fullscreen modes with duplicates removed.  The mode the desktop
// is in right now is always first, because that is the one the original
// pre-selects (SetupDialog.cpp:94-99); the rest are sorted largest first, since
// SDL does not promise EnumDisplaySettings' order.
std::vector<DisplayMode> CollectModes(SDL_DisplayID display, const DisplayMode &desktop)
{
  std::vector<DisplayMode> modes;
  auto add = [&modes](int w, int h, int hz) {
    if (w <= 0 || h <= 0) return;
    for (const DisplayMode &m : modes)
      if (m.width == w && m.height == h && m.hz == hz) return;
    modes.push_back(DisplayMode{w, h, hz});
  };

  add(desktop.width, desktop.height, desktop.hz);

  int count = 0;
  SDL_DisplayMode **list = SDL_GetFullscreenDisplayModes(display, &count);
  if (list)
  {
    for (int i = 0; i < count; i++)
    {
      const SDL_DisplayMode *m = list[i];
      if (m) add(m->w, m->h, (int)lroundf(m->refresh_rate));
    }
    SDL_free(list);
  }

  if (modes.size() > 1)
    std::sort(modes.begin() + 1, modes.end(), [](const DisplayMode &a, const DisplayMode &b) {
      if (a.width != b.width) return a.width > b.width;
      if (a.height != b.height) return a.height > b.height;
      return a.hz > b.hz;
    });
  return modes;
}

std::string ModeLabel(const DisplayMode &m)
{
  // The original's format string, "32bpp" included: every mode it accepted had
  // dmBitsPerPel >= 32 (SetupDialog.cpp:89).
  char buf[64];
  snprintf(buf, sizeof(buf), "%d x %d 32bpp @ %dhz", m.width, m.height, m.hz);
  return buf;
}

std::string AspectLabel(int index, const DisplayMode &desktop)
{
  char buf[48];
  if (index == 0)
  {
    // "<w>:<h> (auto)", the reduced form of the display's own resolution
    // (SetupDialog.cpp:106-114).
    const int g = Gcd(desktop.width, desktop.height);
    snprintf(buf, sizeof(buf), "%d:%d (auto)", g > 0 ? desktop.width / g : 16,
             g > 0 ? desktop.height / g : 9);
    return buf;
  }
  snprintf(buf, sizeof(buf), "%d:%d", kAspectX[index - 1], kAspectY[index - 1]);
  return buf;
}

//////////////////////////////////////////////////////////////////////////////
// A stock Win32 dialog's colours
//
// The template is a WS_POPUP | DS_3DLOOK dialog holding ordinary Button, Static
// and ComboBox controls, and SetupDialog.cpp handles no WM_CTLCOLOR*/
// WM_DRAWITEM/owner-draw message, so what it looks like is exactly COLOR_3DFACE
// / COLOR_BTNTEXT / COLOR_3DSHADOW / COLOR_3DHILIGHT / COLOR_WINDOW.  These are
// those system colours as a light-themed Windows reports them.
const ImVec4 kFace(0.941f, 0.941f, 0.941f, 1.00f);   // COLOR_3DFACE    #F0F0F0
const ImVec4 kText(0.000f, 0.000f, 0.000f, 1.00f);   // COLOR_BTNTEXT   #000000
const ImVec4 kShadow(0.627f, 0.627f, 0.627f, 1.00f); // COLOR_3DSHADOW  #A0A0A0
const ImVec4 kField(1.000f, 1.000f, 1.000f, 1.00f);  // COLOR_WINDOW    #FFFFFF
const ImVec4 kSelect(0.000f, 0.470f, 0.831f, 1.00f); // the selection highlight

void ApplyDialogStyle()
{
  ImGuiStyle &s = ImGui::GetStyle();

  s.WindowRounding = 1.0f;
  s.ChildRounding = 0.0f;
  s.FrameRounding = 1.0f;
  s.PopupRounding = 0.0f;
  s.GrabRounding = 1.0f;
  s.ScrollbarRounding = 0.0f;
  s.TabRounding = 0.0f;

  s.WindowBorderSize = 1.0f;
  s.ChildBorderSize = 1.0f;
  s.FrameBorderSize = 1.0f;
  s.PopupBorderSize = 1.0f;

  s.WindowPadding = ImVec2(11.0f, 11.0f);
  s.FramePadding = ImVec2(5.0f, 3.0f);
  s.ItemSpacing = ImVec2(7.0f, 6.0f);
  s.ItemInnerSpacing = ImVec2(5.0f, 4.0f);

  ImVec4 *c = s.Colors;
  c[ImGuiCol_WindowBg] = kFace;
  c[ImGuiCol_ChildBg] = kFace;
  c[ImGuiCol_PopupBg] = kField;
  c[ImGuiCol_Border] = kShadow;
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

  c[ImGuiCol_Text] = kText;
  c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

  // A stock combo/edit well is a flat bevel: white inside, shadow around.
  c[ImGuiCol_FrameBg] = kField;
  c[ImGuiCol_FrameBgHovered] = kField;
  c[ImGuiCol_FrameBgActive] = kField;

  c[ImGuiCol_Button] = kFace;
  c[ImGuiCol_ButtonHovered] = ImVec4(0.898f, 0.898f, 0.898f, 1.00f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.827f, 0.827f, 0.827f, 1.00f);

  c[ImGuiCol_Header] = kSelect;
  c[ImGuiCol_HeaderHovered] = ImVec4(0.800f, 0.898f, 0.976f, 1.00f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.700f, 0.847f, 0.976f, 1.00f);

  c[ImGuiCol_CheckMark] = kText;
  c[ImGuiCol_ScrollbarBg] = kFace;
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.753f, 0.753f, 0.753f, 1.00f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.678f, 0.678f, 0.678f, 1.00f);
  c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.600f, 0.600f, 0.600f, 1.00f);
  c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);
}

// The original dialog is 209x146 dialog units at Verdana 8pt, i.e. about
// 313x237 pixels, which is roughly 21 text lines tall.  With ImGui's 13px font
// the same proportions are about 400x280, and the two heights below are the
// template's frames (58 and 58 DLU of 146) scaled to it.
const float kDialogWidth = 400.0f;
const float kDialogHeight = 280.0f;
const float kNamesBoxHeight = 102.0f;
const float kOptionsBoxHeight = 103.0f;

// A centred, non-interactive label at (x, y), where x/y are in the same
// coordinate space ImGui::GetCursorPos/SetCursorPos use inside this window.
void CenteredLabel(const char *text, float x, float y, float columnWidth)
{
  const float textWidth = ImGui::CalcTextSize(text).x;
  ImGui::SetCursorPos(ImVec2(x + (columnWidth - textWidth) * 0.5f, y));
  ImGui::TextUnformatted(text);
}

void DrawDialog(const SetupInfo &info, SetupChoice *choice, const std::vector<std::string> &modeLabels,
                const std::vector<std::string> &aspectLabels, int *resIndex, int *aspectIndex,
                bool *decided)
{
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);

  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

  ImGui::Begin("##setup", nullptr, flags);

  const ImGuiStyle &style = ImGui::GetStyle();
  const float lineH = ImGui::GetTextLineHeight();
  const float spacing = style.ItemSpacing.x;
  const float windowX = ImGui::GetCursorPosX();

  // -------------------------------------------------------------------
  // Upper frame: the group and the release, which the release player took
  // from the compiled-in Strings[0]/Strings[1] (MinimalPlayer.cpp:129-139) and
  // put in static 1017 (group, above) and 1016 (release).  Both are SS_CENTER |
  // SS_CENTERIMAGE across the full frame width, so they are centred.
  if (ImGui::BeginChild("##names", ImVec2(0, kNamesBoxHeight), ImGuiChildFlags_Borders))
  {
    const float x = ImGui::GetCursorPosX();
    const float w = ImGui::GetContentRegionAvail().x;
    const float blockH = lineH * 2.0f + spacing;
    const float y = ImGui::GetCursorPosY() + (ImGui::GetContentRegionAvail().y - blockH) * 0.5f;

    CenteredLabel(info.group.empty() ? "CONSPIRACY" : info.group.c_str(), x, y, w);
    CenteredLabel(info.title.empty() ? "[INSERT TITLE]" : info.title.c_str(), x, y + lineH + spacing,
                  w);
  }
  ImGui::EndChild();

  // -------------------------------------------------------------------
  // Lower frame: Resolution, Monitor Aspect, Fullscreen, V-sync.  The template
  // puts combo 1001 at x 17 and combo 1004 at x 135 of a 196-wide frame (52%
  // and 29%), each with its (centred) caption immediately above it.
  if (ImGui::BeginChild("##options", ImVec2(0, kOptionsBoxHeight), ImGuiChildFlags_Borders))
  {
    const float x = ImGui::GetCursorPosX();
    const float innerW = ImGui::GetContentRegionAvail().x;
    const float arrow = ImGui::GetFrameHeight() + 6.0f; // the combo's own arrow button

    // The template's 102:57 split of a 196-wide frame, but never narrower than
    // the longest entry each list actually holds: the desktop this runs on can
    // report "1470 x 956 32bpp @ 60hz" and reduce to "735:478 (auto)", both
    // longer than anything the original had to fit in those 102 and 57 dialog
    // units.  Text that does not fit is text the user cannot choose by.
    auto widest = [](const std::vector<std::string> &labels) {
      float w = 0.0f;
      for (const std::string &s : labels) w = std::max(w, ImGui::CalcTextSize(s.c_str()).x);
      return w;
    };
    float col1 = std::max(innerW * 0.52f, widest(modeLabels) + arrow);
    float col2 = std::max(innerW * 0.29f, widest(aspectLabels) + arrow);
    if (col1 + col2 > innerW - spacing && col1 + col2 > 0.0f)
    {
      const float scale = (innerW - spacing) / (col1 + col2);
      col1 *= scale;
      col2 *= scale;
    }

    float y = ImGui::GetCursorPosY();
    CenteredLabel("Resolution", x, y, col1);
    CenteredLabel("Monitor Aspect", x + col1 + spacing, y, col2);

    y += lineH + spacing;
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::SetNextItemWidth(col1);
    if (ImGui::BeginCombo("##resolution", modeLabels[*resIndex].c_str()))
    {
      for (int i = 0; i < (int)modeLabels.size(); i++)
      {
        ImGui::PushID(i);
        if (ImGui::Selectable(modeLabels[i].c_str(), i == *resIndex)) *resIndex = i;
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }

    ImGui::SetCursorPos(ImVec2(x + col1 + spacing, y));
    ImGui::SetNextItemWidth(col2);
    if (ImGui::BeginCombo("##aspect", aspectLabels[*aspectIndex].c_str()))
    {
      for (int i = 0; i < (int)aspectLabels.size(); i++)
      {
        ImGui::PushID(i);
        if (ImGui::Selectable(aspectLabels[i].c_str(), i == *aspectIndex)) *aspectIndex = i;
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }

    // Checkbox 1002 is at x 46 and 1006 at x 146 of the frame - 20% and 71%.
    y += ImGui::GetFrameHeight() + spacing * 2.0f;
    ImGui::SetCursorPos(ImVec2(x + innerW * 0.20f, y));
    ImGui::Checkbox("Fullscreen", &choice->fullscreen);
    ImGui::SetCursorPos(ImVec2(x + innerW * 0.71f, y));
    ImGui::Checkbox("V-sync", &choice->vsync);
  }
  ImGui::EndChild();

  // -------------------------------------------------------------------
  // The buttons, centred as the template has them (GO! at 54 of 209, Quit at
  // 106).  GO! is BS_DEFPUSHBUTTON, i.e. the default action.
  const float buttonW = 96.0f;
  const float buttonH = lineH + style.FramePadding.y * 2.0f + 6.0f;
  const float rowW = buttonW * 2.0f + spacing * 2.0f;
  const float availW = ImGui::GetContentRegionAvail().x;
  const float buttonY = ImGui::GetCursorPosY() +
                        std::max(0.0f, (ImGui::GetContentRegionAvail().y - buttonH) * 0.5f);
  ImGui::SetCursorPos(ImVec2(windowX + std::max(0.0f, (availW - rowW) * 0.5f), buttonY));

  if (ImGui::Button("GO!", ImVec2(buttonW, buttonH)))
  {
    choice->go = true;
    *decided = true;
  }
  ImGui::SameLine(0.0f, spacing * 2.0f);
  if (ImGui::Button("Quit", ImVec2(buttonW, buttonH)))
  {
    choice->go = false;
    *decided = true;
  }

  ImGui::End();
}

// A borderless window has no caption, and the original made up for that by
// turning any client-area drag into a caption drag (SetupDialog.cpp:72-74:
// WM_LBUTTONDOWN -> WM_NCLBUTTONDOWN / HTCAPTION).
void DragWindow(SDL_Window *window, bool *dragging, ImVec2 *grabWindow, ImVec2 *grabMouse)
{
  float mx = 0.0f, my = 0.0f;
  const SDL_MouseButtonFlags buttons = SDL_GetGlobalMouseState(&mx, &my);
  if (!(buttons & SDL_BUTTON_LMASK))
  {
    *dragging = false;
    return;
  }

  if (!*dragging)
  {
    int wx = 0, wy = 0;
    SDL_GetWindowPosition(window, &wx, &wy);
    *grabWindow = ImVec2((float)wx, (float)wy);
    *grabMouse = ImVec2(mx, my);
    *dragging = true;
    return;
  }

  SDL_SetWindowPosition(window, (int)lroundf(grabWindow->x + (mx - grabMouse->x)),
                        (int)lroundf(grabWindow->y + (my - grabMouse->y)));
}

} // namespace

bool RunSetupDialog(const SetupInfo &info, SetupChoice *choice)
{
  if (!choice) return false;

  // Video only.  This is a subsystem init, so it refcounts against the engine
  // window layer's own SDL_InitSubSystem(SDL_INIT_VIDEO) rather than fighting
  // it - see the teardown note at the end of this function.
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
  {
    phxvk::LogWarn("setup dialog: SDL_InitSubSystem(SDL_INIT_VIDEO) failed (%s); using the "
                   "command line's settings",
                   SDL_GetError());
    return false;
  }

  const SDL_DisplayID display = SDL_GetPrimaryDisplay();
  const SDL_DisplayMode *desktopMode = SDL_GetDesktopDisplayMode(display);
  if (!desktopMode)
  {
    phxvk::LogWarn("setup dialog: no desktop display mode (%s); using the command line's settings",
                   SDL_GetError());
    return false;
  }

  DisplayMode desktop;
  desktop.width = desktopMode->w;
  desktop.height = desktopMode->h;
  desktop.hz = (int)lroundf(desktopMode->refresh_rate);

  const std::vector<DisplayMode> modes = CollectModes(display, desktop);
  if (modes.empty())
  {
    phxvk::LogWarn("setup dialog: the display reports no usable modes");
    return false;
  }

  SDL_Window *window = SDL_CreateWindow(info.title.empty() ? "apEx setup" : info.title.c_str(),
                                        (int)kDialogWidth, (int)kDialogHeight,
                                        SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP);
  if (!window)
  {
    phxvk::LogWarn("setup dialog: SDL_CreateWindow failed (%s); using the command line's settings",
                   SDL_GetError());
    return false;
  }
  SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

  SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer)
  {
    phxvk::LogWarn("setup dialog: SDL_CreateRenderer failed (%s); using the command line's settings",
                   SDL_GetError());
    SDL_DestroyWindow(window);
    return false;
  }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  // A setup screen must not drop an imgui.ini into the user's working
  // directory, and has no log window to write.
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  ApplyDialogStyle();

  if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer) || !ImGui_ImplSDLRenderer3_Init(renderer))
  {
    phxvk::LogWarn("setup dialog: the ImGui SDL3/SDLRenderer3 backends failed to initialise; using "
                   "the command line's settings");
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    return false;
  }

  phxvk::LogInfo("setup dialog: %zu mode(s), desktop %dx%d @ %dHz", modes.size(), desktop.width,
                 desktop.height, desktop.hz);

  // The combo entries, built once: the template's list is fixed at ten aspect
  // entries and the display's own mode list, and both are needed every frame.
  std::vector<std::string> modeLabels;
  modeLabels.reserve(modes.size());
  for (const DisplayMode &m : modes) modeLabels.push_back(ModeLabel(m));

  std::vector<std::string> aspectLabels;
  aspectLabels.reserve(kAspectCount + 1);
  for (int i = 0; i <= kAspectCount; i++) aspectLabels.push_back(AspectLabel(i, desktop));

  // Pre-select the mode the command line asked for, else the desktop's own, and
  // the "(auto)" aspect - which is what the original opens on.
  int resIndex = 0;
  for (int i = 0; i < (int)modes.size(); i++)
    if (modes[i].width == choice->width && modes[i].height == choice->height)
    {
      resIndex = i;
      break;
    }
  int aspectIndex = 0;

  bool decided = false;
  bool dragging = false;
  ImVec2 grabWindow(0.0f, 0.0f), grabMouse(0.0f, 0.0f);

  while (!decided)
  {
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
      ImGui_ImplSDL3_ProcessEvent(&event);

      switch (event.type)
      {
        case SDL_EVENT_QUIT:
          choice->go = false;
          decided = true;
          break;

        case SDL_EVENT_KEY_DOWN:
          // The template's buttons are IDOK (GO!) and IDCANCEL (Quit), so Enter
          // accepts and Escape cancels - but not while a combo's popup is open,
          // where both keys already mean something to the widget.
          if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
          {
            if (ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                                                            ImGuiPopupFlags_AnyPopupLevel))
              break;
            choice->go = true;
            decided = true;
          }
          else if (event.key.key == SDLK_ESCAPE)
          {
            if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
              break;
            choice->go = false;
            decided = true;
          }
          break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
          if (event.button.button == SDL_BUTTON_LEFT && !ImGui::IsAnyItemHovered())
            DragWindow(window, &dragging, &grabWindow, &grabMouse);
          break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
          if (event.button.button == SDL_BUTTON_LEFT) dragging = false;
          break;

        case SDL_EVENT_MOUSE_MOTION:
          if (dragging) DragWindow(window, &dragging, &grabWindow, &grabMouse);
          break;

        default:
          break;
      }
    }

    if (decided) break;

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    DrawDialog(info, choice, modeLabels, aspectLabels, &resIndex, &aspectIndex, &decided);

    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, (Uint8)(kFace.x * 255.0f), (Uint8)(kFace.y * 255.0f),
                           (Uint8)(kFace.z * 255.0f), 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }

  choice->width = modes[resIndex].width;
  choice->height = modes[resIndex].height;
  choice->monitorAspectX = aspectIndex > 0 ? kAspectX[aspectIndex - 1] : 0;
  choice->monitorAspectY = aspectIndex > 0 ? kAspectY[aspectIndex - 1] : 0;

  if (choice->go)
    phxvk::LogInfo("setup: %dx%d %s, vsync %s, monitor aspect %s", choice->width, choice->height,
                   choice->fullscreen ? "fullscreen" : "windowed", choice->vsync ? "on" : "off",
                   aspectLabels[aspectIndex].c_str());
  else
    phxvk::LogInfo("setup: the user chose Quit");

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  // SDL_INIT_VIDEO is deliberately left initialised.  The engine's window layer
  // calls SDL_InitSubSystem(SDL_INIT_VIDEO) itself, and tearing the video
  // subsystem down and bringing it straight back up is what upsets the macOS
  // application and menu setup.

  return true;
}

} // namespace phxplayer
