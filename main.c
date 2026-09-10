#include "platform/win32.h"
#include "showcase/showcase.h"
#include <stdlib.h>

static void resized(void *user, float width, float height) {
    showcase_resize(user,width,height);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    (void)previous; (void)command;
    Ui *ui=calloc(1,sizeof *ui);
    Showcase showcase;
    if (!ui) return 1;
    ui_init(ui,NULL,NULL);
    if (!showcase_init(&showcase,ui)) { free(ui); return 1; }
    UiWindowConfig config={ui,L"Dark UI / Native Foundation",resized,&showcase,1280,790,600,420};
    int result=ui_win32_run(instance,show,&config);
    free(ui);
    return result;
}
