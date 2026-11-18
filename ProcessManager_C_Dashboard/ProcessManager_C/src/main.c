#include <windows.h>
#include "gui.h"
int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command_line,int show_cmd) {
    (void)previous; (void)command_line;
    return gui_run(instance,show_cmd);
}
