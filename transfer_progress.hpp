#pragma once

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Flex.H>
#include <string>
#include <functional>

class TransferProgressWindow : public Fl_Double_Window {
public:
    TransferProgressWindow(const std::string& title, const std::string& filename, 
                          std::function<void()> cancel_callback);
    ~TransferProgressWindow();

    void update_progress(float percentage);
    void set_status(const std::string& status);
    void complete();
    void show() override;

private:
    Fl_Box* filename_box;
    Fl_Progress* progress_bar;
    Fl_Box* status_box;
    Fl_Button* cancel_button;
    std::function<void()> on_cancel;

    static void cancel_cb(Fl_Widget* w, void* data);
};