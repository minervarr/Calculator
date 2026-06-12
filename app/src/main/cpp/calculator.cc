#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/log.h>
#include "app.hh"

void handleCommand(struct android_app* state, int32_t cmd) {
  App* app = (App*)state->userData;
  switch(cmd) {
    case APP_CMD_INIT_WINDOW:
      __android_log_print(ANDROID_LOG_DEBUG, "APP", "INIT_WINDOW fired");
      app->init(state->window, state->activity->assetManager);
      app->initialized = true;
      app->dirty = true;
      break;
    case APP_CMD_TERM_WINDOW:
      __android_log_print(ANDROID_LOG_DEBUG, "APP", "TERM_WINDOW fired");
      app->cleanup();
      new (app) App();
      //app->initialized = false;
      break;
    case APP_CMD_WINDOW_REDRAW_NEEDED:
    case APP_CMD_CONFIG_CHANGED:
      app->dirty = true;
      break;
  }
}

int32_t handleInput(struct android_app* state, AInputEvent* event) {
  if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;
  int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
  if (action != AMOTION_EVENT_ACTION_DOWN) return 0;
  App* app = (App*)state->userData;
  if (!app->initialized) return 0;
  float x = AMotionEvent_getX(event, 0);
  float y = AMotionEvent_getY(event, 0);
  app->onTouch(x, y);
  return 1;
}

void android_main(struct android_app* state)
{
  App app;
  state->userData = &app;
  state->onAppCmd = handleCommand;
  state->onInputEvent = handleInput;
  while(true) {
    int events;
    struct android_poll_source* source;
    ALooper_pollOnce(0, nullptr, &events, (void**)&source);
    if(source) source->process(state, source);
    if(app.initialized && app.dirty) {
      app.drawFrame();
      app.dirty = false;
    };
  };
}
