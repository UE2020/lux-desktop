#include "transfer_progress.hpp"
#include <FL/fl_ask.H>
#include <sstream>
#include <iomanip>

TransferProgressWindow::TransferProgressWindow(const std::string& title, const std::string& filename,
                                             std::function<void()> cancel_callback)
    : Fl_Double_Window(400, 150, title.c_str()), on_cancel(cancel_callback) {
    
    begin();
    
    // Create a vertical layout
    Fl_Flex* layout = new Fl_Flex(10, 10, w() - 20, h() - 20, Fl_Flex::COLUMN);
    layout->spacing(10);
    
    // Filename display
    filename_box = new Fl_Box(0, 0, layout->w(), 25);
    filename_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    filename_box->copy_label(filename.c_str());
    layout->fixed(filename_box, 25);
    
    // Progress bar
    progress_bar = new Fl_Progress(0, 0, layout->w(), 30);
    progress_bar->minimum(0.0);
    progress_bar->maximum(100.0);
    progress_bar->value(0.0);
    progress_bar->color(FL_BACKGROUND_COLOR);
    progress_bar->selection_color(FL_BLUE);
    layout->fixed(progress_bar, 30);
    
    // Status text
    status_box = new Fl_Box(0, 0, layout->w(), 25);
    status_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    status_box->label("Initializing...");
    layout->fixed(status_box, 25);
    
    // Cancel button
    cancel_button = new Fl_Button(0, 0, 100, 25, "Cancel");
    cancel_button->callback(cancel_cb, this);
    layout->fixed(cancel_button, 25);
    
    layout->end();
    
    end();
    
    set_modal();
    resizable(this);
}

TransferProgressWindow::~TransferProgressWindow() {
    // Clean up if needed
}

void TransferProgressWindow::update_progress(float percentage) {
    progress_bar->value(percentage);
    
    std::stringstream ss;
    ss << std::fixed << std::setprecision(1) << percentage << "% complete";
    status_box->copy_label(ss.str().c_str());
    
    // Force redraw
    progress_bar->redraw();
    status_box->redraw();
    Fl::check();
}

void TransferProgressWindow::set_status(const std::string& status) {
    status_box->copy_label(status.c_str());
    status_box->redraw();
    Fl::check();
}

void TransferProgressWindow::complete() {
    progress_bar->value(100.0);
    status_box->copy_label("Transfer complete");
    cancel_button->label("Close");
    redraw();
    Fl::check();
}

void TransferProgressWindow::show() {
    Fl_Double_Window::show();
    Fl::check();
}

void TransferProgressWindow::cancel_cb(Fl_Widget* w, void* data) {
    TransferProgressWindow* window = static_cast<TransferProgressWindow*>(data);
    
    // If transfer is complete, just close the window
    if (window->progress_bar->value() >= 100.0) {
        window->hide();
        return;
    }
    
    // Otherwise, confirm cancellation
    if (fl_choice("Cancel file transfer?", "No", "Yes", nullptr) == 1) {
        if (window->on_cancel) {
            window->on_cancel();
        }
        window->hide();
    }
}