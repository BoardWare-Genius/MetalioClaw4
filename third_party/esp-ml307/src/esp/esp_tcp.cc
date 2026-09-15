#include "esp_tcp.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

static const char *TAG = "EspTcp";

EspTcp::EspTcp() {
    event_group_ = xEventGroupCreate();
}

EspTcp::~EspTcp() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspTcp::Connect(const std::string& host, int port) {
    // A peer may have closed the socket while the receive task is still
    // unwinding its callback. Reap that task before reusing this object.
    if (receive_task_handle_ != nullptr || tcp_fd_.load() != -1) {
        Disconnect();
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // host is domain
    struct hostent *server = gethostbyname(host.c_str());
    if (server == NULL) {
        last_error_ = h_errno;
        ESP_LOGE(TAG, "Failed to get host by name");
        return false;
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    tcp_fd_.store(fd);
    if (fd < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    int ret = connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, last_error_);
        close(tcp_fd_.exchange(-1));
        return false;
    }

    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
    const BaseType_t task_created = xTaskCreate([](void* arg) {
        EspTcp* tcp = (EspTcp*)arg;
        tcp->ReceiveTask();
        xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "tcp_receive", 4096, this, 1, &receive_task_handle_);
    if (task_created != pdPASS) {
        last_error_ = ENOMEM;
        connected_ = false;
        const int failed_fd = tcp_fd_.exchange(-1);
        if (failed_fd >= 0) {
            close(failed_fd);
        }
        receive_task_handle_ = nullptr;
        ESP_LOGE(TAG, "Failed to create TCP receive task");
        return false;
    }
    return true;
}

void EspTcp::Disconnect() {
    const bool called_from_receive_task =
        receive_task_handle_ != nullptr &&
        xTaskGetCurrentTaskHandle() == receive_task_handle_;
    DoDisconnect(!called_from_receive_task);
}

void EspTcp::DoDisconnect(bool wait_for_task) {
    const bool notify_disconnected = connected_.exchange(false);

    // shutdown() wakes a blocking recv() without allowing the descriptor to be
    // recycled while the receive task might still be using it.
    const int fd = tcp_fd_.load();
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }

    // The owner must not destroy callbacks/condition variables until the
    // receive task has returned from them. shutdown() and HttpClient::Close()
    // make this wait bounded in normal operation.
    if (wait_for_task && receive_task_handle_ != nullptr) {
        xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT,
                            pdFALSE, pdFALSE, portMAX_DELAY);
        receive_task_handle_ = nullptr;
    }

    const int fd_to_close = tcp_fd_.exchange(-1);
    if (fd_to_close >= 0) {
        close(fd_to_close);
    }

    // 断开连接时触发断开回调
    if (notify_disconnected && disconnect_callback_) {
        disconnect_callback_();
    }
}

int EspTcp::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();

    while (total_sent < data_size) {
        const int fd = tcp_fd_.load();
        if (fd < 0) {
            return -1;
        }
        int ret = send(fd, data_ptr + total_sent, data_size - total_sent, 0);

        if (ret <= 0) {
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        total_sent += ret;
    }

    return total_sent;
}

void EspTcp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        const int fd = tcp_fd_.load();
        if (fd < 0) {
            break;
        }
        int ret = recv(fd, data.data(), data.size(), 0);
        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "TCP receive failed: %d", ret);
            }
            // 被动断开，不需要等待接收任务退出（当前就是接收任务）
            DoDisconnect(false);
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}

int EspTcp::GetLastError() {
    return last_error_;
}
