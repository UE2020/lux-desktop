#include "file_transfer.hpp"
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <filesystem>
#include <cstring>

using nlohmann::json;

FileTransfer::FileTransfer(std::shared_ptr<rtc::DataChannel> ordered_channel)
    : ordered_channel(ordered_channel), send_buffer(16384 + sizeof(uint32_t)) {
}

FileTransfer::~FileTransfer() {
    // Cancel all active transfers
    std::lock_guard<std::mutex> lock(transfers_mutex);
    for (auto& [id, transfer] : transfers) {
        if (transfer.state == TransferState::InProgress || 
            transfer.state == TransferState::Pending) {
            cancel_transfer(id);
        }
    }
}

void FileTransfer::set_transfer_callback(TransferCallback callback) {
    on_transfer_update = callback;
}

void FileTransfer::show_progress_window(const TransferInfo& transfer) {
    std::string title;
    std::string filename = std::filesystem::path(transfer.filename).filename().string();
    
    if (transfer.type == TransferType::Upload) {
        title = "Uploading File";
    } else {
        title = "Downloading File";
    }
    
    auto progress_window = std::make_unique<TransferProgressWindow>(
        title, filename,
        [this, id = transfer.id]() {
            // Cancel callback
            cancel_transfer(id);
        }
    );
    
    progress_window->show();
    progress_windows[transfer.id] = std::move(progress_window);
}

void FileTransfer::update_progress_window(const TransferInfo& transfer) {
    auto it = progress_windows.find(transfer.id);
    if (it == progress_windows.end()) {
        return;
    }
    
    auto& window = it->second;
    
    if (transfer.state == TransferState::Completed) {
        window->complete();
    } else if (transfer.state == TransferState::Cancelled) {
        window->set_status("Transfer cancelled");
        window->hide();
        progress_windows.erase(it);
    } else if (transfer.state == TransferState::Failed) {
        window->set_status("Transfer failed");
    } else if (transfer.state == TransferState::InProgress) {
        float percentage = 0;
        if (transfer.size > 0) {
            percentage = (float)transfer.transferred / transfer.size * 100.0f;
        }
        window->update_progress(percentage);
    }
}

uint32_t FileTransfer::start_upload(const std::string& filename) {
    if (!ordered_channel || !ordered_channel->isOpen()) {
        fl_alert("Cannot start file transfer: Data channel is not open");
        return 0;
    }

    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        fl_alert("Failed to open file for upload: %s", filename.c_str());
        return 0;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    uint32_t transfer_id;
    {
        std::lock_guard<std::mutex> lock(transfers_mutex);
        transfer_id = next_transfer_id++;
        
        TransferInfo transfer;
        transfer.id = transfer_id;
        transfer.type = TransferType::Upload;
        transfer.state = TransferState::Pending;
        transfer.filename = std::filesystem::path(filename).filename().string();
        transfer.size = file_size;
        transfer.transferred = 0;
        transfer.input_file = std::make_unique<std::ifstream>(filename, std::ios::binary);
        
        transfers[transfer_id] = std::move(transfer);
    }

    // Send request to server
    json request = {
        {"type", "requesttransfer"},
        {"id", transfer_id},
        {"size", file_size}
    };
    
    // Get a reference to the transfer for the progress window
    TransferInfo& transfer = transfers[transfer_id];
    
    // Show progress window
    show_progress_window(transfer);
    
    ordered_channel->send(request.dump());
    return transfer_id;
}

uint32_t FileTransfer::request_download() {
    if (!ordered_channel || !ordered_channel->isOpen()) {
        fl_alert("Cannot start file transfer: Data channel is not open");
        return 0;
    }

    uint32_t transfer_id;
    {
        std::lock_guard<std::mutex> lock(transfers_mutex);
        transfer_id = next_transfer_id++;
        
        TransferInfo transfer;
        transfer.id = transfer_id;
        transfer.type = TransferType::Download;
        transfer.state = TransferState::Pending;
        transfer.size = 0; // Unknown until server responds
        transfer.transferred = 0;
        
        transfers[transfer_id] = std::move(transfer);
    }

    // Send request to server
    json request = {
        {"type", "requesttransfer"},
        {"id", transfer_id}
        // No size field for download requests
    };
    
    ordered_channel->send(request.dump());
    return transfer_id;
}

void FileTransfer::cancel_transfer(uint32_t id) {
    std::lock_guard<std::mutex> lock(transfers_mutex);
    
    auto it = transfers.find(id);
    if (it == transfers.end()) {
        return;
    }
    
    // Update transfer state
    it->second.state = TransferState::Cancelled;
    
    // Close file handles
    if (it->second.input_file) {
        it->second.input_file->close();
    }
    if (it->second.output_file) {
        it->second.output_file->close();
    }
    
    // Update progress window
    update_progress_window(it->second);
    
    // Notify callback
    if (on_transfer_update) {
        on_transfer_update(it->second);
    }
    
    // Send cancel message
    if (ordered_channel && ordered_channel->isOpen()) {
        json cancel = {
            {"type", "canceltransfer"},
            {"id", id}
        };
        ordered_channel->send(cancel.dump());
    }
}

void FileTransfer::process_message(const json& message) {
    if (!message.contains("type") || !message["type"].is_string()) {
        return;
    }
    
    std::string type = message["type"];
    
    if (type == "transferready") {
        if (!message.contains("id") || !message.contains("size")) {
            return;
        }
        
        uint32_t id = message["id"];
        size_t size = message["size"];
        
        std::lock_guard<std::mutex> lock(transfers_mutex);
        auto it = transfers.find(id);
        if (it == transfers.end()) {
            return;
        }
        
        TransferInfo& transfer = it->second;
        transfer.size = size;
        
        if (transfer.type == TransferType::Upload) {
            // Server is ready to receive our upload
            transfer.state = TransferState::InProgress;
            
            // Start sending chunks
            send_chunk(transfer);
        } 
        else if (transfer.type == TransferType::Download) {
            // Server is ready to send us a file
            // Ask user where to save the file
            Fl_Native_File_Chooser chooser;
            chooser.title("Save File As");
            chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
            chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
            
            if (chooser.show() != 0) {
                // User cancelled
                cancel_transfer(id);
                return;
            }
            
            std::string save_path = chooser.filename();
            transfer.filename = std::filesystem::path(save_path).filename().string();
            transfer.output_file = std::make_unique<std::ofstream>(save_path, std::ios::binary);
            
            if (!transfer.output_file->is_open()) {
                fl_alert("Failed to open file for writing: %s", save_path.c_str());
                cancel_transfer(id);
                return;
            }
            
            transfer.state = TransferState::InProgress;
        }
        
        // Show progress window
        show_progress_window(transfer);
        
        // Notify callback
        if (on_transfer_update) {
            on_transfer_update(transfer);
        }
    }
    else if (type == "canceltransfer") {
        if (!message.contains("id")) {
            return;
        }
        
        uint32_t id = message["id"];
        
        std::lock_guard<std::mutex> lock(transfers_mutex);
        auto it = transfers.find(id);
        if (it == transfers.end()) {
            return;
        }
        
        // Update transfer state
        it->second.state = TransferState::Cancelled;
        
        // Close file handles
        if (it->second.input_file) {
            it->second.input_file->close();
        }
        if (it->second.output_file) {
            it->second.output_file->close();
        }
        
        // Notify callback
        if (on_transfer_update) {
            on_transfer_update(it->second);
        }
    }
}

void FileTransfer::process_binary(const rtc::binary& message) {
    if (message.size() < sizeof(uint32_t)) {
        return; // Message too small to contain transfer ID
    }
    
    // Extract transfer ID (first 4 bytes, big-endian)
    uint32_t id = 0;
    id |= static_cast<uint32_t>(message[0]) << 24;
    id |= static_cast<uint32_t>(message[1]) << 16;
    id |= static_cast<uint32_t>(message[2]) << 8;
    id |= static_cast<uint32_t>(message[3]);
    
    std::lock_guard<std::mutex> lock(transfers_mutex);
    auto it = transfers.find(id);
    if (it == transfers.end()) {
        return; // Unknown transfer ID
    }
    
    TransferInfo& transfer = it->second;
    
    if (transfer.state != TransferState::InProgress) {
        return; // Transfer not in progress
    }
    
    if (transfer.type != TransferType::Download || !transfer.output_file) {
        return; // Not a download or no output file
    }
    
    // Write data to file
    const uint8_t* data = message.data() + sizeof(uint32_t);
    size_t data_size = message.size() - sizeof(uint32_t);
    
    transfer.output_file->write(reinterpret_cast<const char*>(data), data_size);
    transfer.transferred += data_size;
    
    // Check if transfer is complete
    if (transfer.transferred >= transfer.size) {
        transfer.state = TransferState::Completed;
        transfer.output_file->close();
    }
    
    // Update progress window
    update_progress_window(transfer);
    
    // Notify callback
    if (on_transfer_update) {
        on_transfer_update(transfer);
    }
}

bool FileTransfer::send_chunk(TransferInfo& transfer, size_t chunk_size) {
    if (!ordered_channel || !ordered_channel->isOpen() || 
        !transfer.input_file || !transfer.input_file->is_open() ||
        transfer.state != TransferState::InProgress) {
        return false;
    }
    
    // Prepare buffer: first 4 bytes are transfer ID in big-endian format
    send_buffer[0] = (transfer.id >> 24) & 0xFF;
    send_buffer[1] = (transfer.id >> 16) & 0xFF;
    send_buffer[2] = (transfer.id >> 8) & 0xFF;
    send_buffer[3] = transfer.id & 0xFF;
    
    // Read data from file
    transfer.input_file->read(reinterpret_cast<char*>(send_buffer.data() + sizeof(uint32_t)), 
                             chunk_size);
    size_t bytes_read = transfer.input_file->gcount();
    
    if (bytes_read == 0) {
        // End of file
        transfer.state = TransferState::Completed;
        transfer.input_file->close();
        
        // Notify callback
        if (on_transfer_update) {
            on_transfer_update(transfer);
        }
        return true;
    }
    
    // Send data
    rtc::binary message(send_buffer.data(), send_buffer.data() + sizeof(uint32_t) + bytes_read);
    ordered_channel->send(message);
    
    // Update transfer progress
    transfer.transferred += bytes_read;
    
    // Update progress window
    update_progress_window(transfer);
    
    // Notify callback
    if (on_transfer_update) {
        on_transfer_update(transfer);
    }
    
    // If there's more data to send, schedule another chunk
    if (transfer.transferred < transfer.size) {
        // In a real implementation, you might want to throttle this or use a queue
        // For simplicity, we're sending chunks immediately
        return send_chunk(transfer, chunk_size);
    }
    
    return true;
}