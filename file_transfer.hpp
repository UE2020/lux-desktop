#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <rtc/rtc.hpp>
#include "json.hpp"

class FileTransfer {
public:
    enum class TransferType {
        Upload,
        Download
    };

    enum class TransferState {
        Pending,
        InProgress,
        Completed,
        Cancelled,
        Failed
    };

    struct TransferInfo {
        uint32_t id;
        TransferType type;
        TransferState state;
        std::string filename;
        size_t size;
        size_t transferred;
        std::unique_ptr<std::ifstream> input_file;
        std::unique_ptr<std::ofstream> output_file;
    };

    using TransferCallback = std::function<void(const TransferInfo&)>;

private:
    std::shared_ptr<rtc::DataChannel> ordered_channel;
    std::mutex transfers_mutex;
    std::unordered_map<uint32_t, TransferInfo> transfers;
    uint32_t next_transfer_id = 1;
    TransferCallback on_transfer_update;
    
    // Buffer for sending file chunks
    std::vector<uint8_t> send_buffer;
    
    // Send a file chunk for the specified transfer
    bool send_chunk(TransferInfo& transfer, size_t chunk_size = 16384);
    
    // Process a received binary message containing file data
    void process_file_data(const rtc::binary& message);

public:
    FileTransfer(std::shared_ptr<rtc::DataChannel> ordered_channel);
    ~FileTransfer();

    // Set callback for transfer updates
    void set_transfer_callback(TransferCallback callback);

    // Start a file upload (client to server)
    uint32_t start_upload(const std::string& filename);

    // Start a file download (server to client)
    uint32_t request_download();

    // Cancel an active transfer
    void cancel_transfer(uint32_t id);

    // Process a received JSON message
    void process_message(const nlohmann::json& message);

    // Process a received binary message
    void process_binary(const rtc::binary& message);
};