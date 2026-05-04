#include <android_native_app_glue.h>
#include "app.hh"

void handleCommand(struct android_app* state, int32_t cmd) {
  App* app = (App*)state->userData;
  switch(cmd) {
    case APP_CMD_INIT_WINDOW:
      app->init(state->window, state->activity->assetManager);
      app->initialized = true;
      break;
    case APP_CMD_TERM_WINDOW:
      app->cleanup();
      new (app) App();
      //app->initialized = false;
      break;
  }
}

void android_main(struct android_app* state)
{
  App app;
  bool dirty = true;
  state->userData = &app;
  state->onAppCmd = handleCommand;
  while(true) {
    int events;
    struct android_poll_source* source;
    ALooper_pollOnce(0, nullptr, &events, (void**)&source);
    if(source) source->process(state, source);
    if(app.initialized && dirty) {
      app.drawFrame();
      dirty = false;
    };
  };
}
