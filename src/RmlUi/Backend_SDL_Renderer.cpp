#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Profiling.h>

#include "Backend.h"
#include "Platform_SDL.h"
#include "Renderer_SDL.h"


struct BackendData {
  BackendData(SDL_Window* window, SDL_Renderer* renderer) : render_interface(renderer) {}

  SystemInterface_SDL system_interface;
  RenderInterface_SDL render_interface;
  TextInputMethodEditor_SDL text_input_method_editor;

  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;

  bool running = true;
};

static Rml::UniquePtr<BackendData> data;

// ---------------------------------------------------------------------------
// Analog sticks and triggers (DualSense and friends).
//
// SDL only reports axis *motion*, so holding a stick generates no events;
// we keep the latest axis values here and synthesize key presses with
// keyboard-style auto-repeat each frame. Both sticks navigate/seek (they
// map to the arrow keys, exactly like the dpad) and the triggers page like
// the shoulder buttons.
// ---------------------------------------------------------------------------

namespace {

constexpr Sint16 kStickPress = 16000;   // ~50% deflection engages...
constexpr Sint16 kStickRelease = 9000;  // ...and it disengages down here
constexpr Sint16 kTriggerPress = 20000;
constexpr Sint16 kTriggerRelease = 8000;
constexpr Uint64 kRepeatDelayMs = 400;  // before the first repeat
constexpr Uint64 kRepeatRateMs = 140;

struct AxisKey {
  SDL_GameControllerAxis axis;
  int sign;                        // which half of the axis
  Rml::Input::KeyIdentifier key;
  bool repeats;
  Sint16 press, release;
  // state
  bool held = false;
  Uint64 next_fire = 0;
};

AxisKey axis_keys[] = {
  {SDL_CONTROLLER_AXIS_LEFTX,  -1, Rml::Input::KI_LEFT,  true,  kStickPress, kStickRelease},
  {SDL_CONTROLLER_AXIS_LEFTX,  +1, Rml::Input::KI_RIGHT, true,  kStickPress, kStickRelease},
  {SDL_CONTROLLER_AXIS_LEFTY,  -1, Rml::Input::KI_UP,    true,  kStickPress, kStickRelease},
  {SDL_CONTROLLER_AXIS_LEFTY,  +1, Rml::Input::KI_DOWN,  true,  kStickPress, kStickRelease},
  {SDL_CONTROLLER_AXIS_RIGHTX, -1, Rml::Input::KI_LEFT,  true,  kStickPress, kStickRelease},
  {SDL_CONTROLLER_AXIS_RIGHTX, +1, Rml::Input::KI_RIGHT, true,  kStickPress, kStickRelease},
  {SDL_CONTROLLER_AXIS_RIGHTY, -1, Rml::Input::KI_UP,    true,  kStickPress, kStickRelease},
  {SDL_CONTROLLER_AXIS_RIGHTY, +1, Rml::Input::KI_DOWN,  true,  kStickPress, kStickRelease},
  // L2/R2 page exactly like L1/R1; single-shot so a squeeze skips one track.
  {SDL_CONTROLLER_AXIS_TRIGGERLEFT,  +1, Rml::Input::KI_PRIOR, false, kTriggerPress, kTriggerRelease},
  {SDL_CONTROLLER_AXIS_TRIGGERRIGHT, +1, Rml::Input::KI_NEXT,  false, kTriggerPress, kTriggerRelease},
};

Sint16 axis_value[SDL_CONTROLLER_AXIS_MAX] = {};

void ProcessControllerAxes(Rml::Context* context) {
  const Uint64 now = SDL_GetTicks64();

  for (AxisKey& ak : axis_keys) {
    const int v = axis_value[ak.axis] * ak.sign;

    if (!ak.held) {
      if (v >= ak.press) {
        ak.held = true;
        ak.next_fire = now + kRepeatDelayMs;
        context->ProcessKeyDown(ak.key, 0);
      }
    } else {
      if (v <= ak.release) {
        ak.held = false;
        context->ProcessKeyUp(ak.key, 0);
      } else if (ak.repeats && now >= ak.next_fire) {
        ak.next_fire = now + kRepeatRateMs;
        context->ProcessKeyDown(ak.key, 0);
      }
    }
  }
}

} // namespace

bool Backend::Initialize(const char* window_name, int width, int height, bool allow_resize) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    return false;
  }

  const Uint32 window_flags = (allow_resize ? SDL_WINDOW_RESIZABLE : 0);
  SDL_Window* window = SDL_CreateWindow(window_name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, window_flags);
  SDL_StopTextInput();

  if (!window) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "SDL error on create window: %s", SDL_GetError());
    return false;
  }

  // Software rendering only: no OpenGL (or any other GPU API) is used.
  SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "SDL error on create renderer: %s", SDL_GetError());
    return false;
  }

  SDL_RendererInfo info;
  if (SDL_GetRendererInfo(renderer, &info) == 0) {
    Rml::Log::Message(Rml::Log::LT_INFO, "Using SDL renderer: %s", info.name);
  }

  data = Rml::MakeUnique<BackendData>(window, renderer);
  data->window = window;
  data->renderer = renderer;

  Rml::SetTextInputHandler(&data->text_input_method_editor);

  return true;
}

void Backend::Shutdown() {
  SDL_DestroyRenderer(data->renderer);
  SDL_DestroyWindow(data->window);
  data.reset();
  SDL_Quit();
}

Rml::SystemInterface* Backend::GetSystemInterface() {
  return &data->system_interface;
}

Rml::RenderInterface* Backend::GetRenderInterface() {
  return &data->render_interface;
}

SDL_Renderer* Backend::GetSDLRenderer() {
  return data ? data->renderer : nullptr;
}

bool Backend::ProcessEvents(Rml::Context* context) {
  SDL_GameController* controller;
  SDL_Event ev;

  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
    case SDL_QUIT:
      return false;

    case SDL_TEXTEDITING:
      data->text_input_method_editor.HandleEdit(ev.edit);
      break;

    case SDL_CONTROLLERDEVICEADDED:
      SDL_GameControllerOpen(ev.cdevice.which);
      break;

    case SDL_CONTROLLERDEVICEREMOVED:
      if ((controller=SDL_GameControllerFromInstanceID(ev.cdevice.which))) {
        SDL_GameControllerClose(controller);
      }
      break;

    case SDL_CONTROLLERAXISMOTION:
      if (ev.caxis.axis < SDL_CONTROLLER_AXIS_MAX) {
        axis_value[ev.caxis.axis] = ev.caxis.value;
      }
      break;

    // DualSense touchpad click: same as Square (next video track).
    case SDL_CONTROLLERBUTTONDOWN:
      if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_TOUCHPAD) {
        context->ProcessKeyDown(Rml::Input::KI_SPACE, 0);
        break;
      }
      RmlSDL::InputEventHandler(context, data->window, ev);
      break;

    default:
      RmlSDL::InputEventHandler(context, data->window, ev);
      break;
    }
  }

  ProcessControllerAxes(context);

  return true;
}

void Backend::BeginFrame() {
  SDL_SetRenderDrawColor(data->renderer, 0, 0, 0, 0);
  SDL_RenderClear(data->renderer);
  data->render_interface.BeginFrame();
}

void Backend::PresentFrame() {
  data->render_interface.EndFrame();
  SDL_RenderPresent(data->renderer);
}
