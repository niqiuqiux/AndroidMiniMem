#pragma once

#include "Window.h"

class LogWindow : public Window {
public:
    LogWindow();
    void onDraw() override;
    unsigned int getWindowFlags() const override;
};

