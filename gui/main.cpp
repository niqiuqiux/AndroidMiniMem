#include "ExceptionHandler.h"
#include "gui/Gui.h"
#include "ipc/IpcServer.h"
#include "mem/SystemMemService.h"
#include "renderer/AppWindow.h"

#include <cstdio>

int main(int, char**) {
    ExceptionHandler::Initialize("MiniMem", "./CrashDumps/",
        [](const ExceptionHandler::ExceptionInfo& info) {
            std::printf("程序崩溃!\n%s\n%s\n",
                        info.description.c_str(),
                        info.dumpFilePath.c_str());
        });

    AppWindow window;
    if (!window.init("MiniMem - Android Memory Debugger", 1280, 800)) {
        return 1;
    }

    auto& memService = Mem::getSystemMemService();
    IpcServer::GetInstance().Start(memService, 28100);

    window.run([&window, &memService] {
        if (!Gui::mainLoop(memService)) {
            window.requestClose();
        }
    });

    IpcServer::GetInstance().Stop();
    window.shutdown();

    return 0;
}
