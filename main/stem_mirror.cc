#include "stem_mirror.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#define TAG "StemMirror"

extern const char stem_mirror_html_start[] asm("_binary_stem_mirror_html_start");
extern const char stem_mirror_html_end[] asm("_binary_stem_mirror_html_end");

namespace {

// LWIP allows 10 sockets in total and the assistant's own connections need some of them
constexpr int kMaxClients = 3;
constexpr size_t kMaxIncomingFrame = 1024;

void KeepContext(void*) {}  // the instance is static; httpd must not free() it

}  // namespace

bool StemMirror::Start(int canvas_width, int canvas_height) {
    if (server_ != nullptr) {
        return true;
    }
    canvas_w_ = canvas_width;
    canvas_h_ = canvas_height;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32770;
    config.max_open_sockets = kMaxClients;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    config.global_user_ctx = this;
    config.global_user_ctx_free_fn = KeepContext;
    config.close_fn = OnClose;
    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start the big-screen web server");
        server_ = nullptr;
        return false;
    }
    httpd_uri_t page = {};
    page.uri = "/";
    page.method = HTTP_GET;
    page.handler = ServePage;
    httpd_register_uri_handler(server_, &page);

    httpd_uri_t ws = {};
    ws.uri = "/ws";
    ws.method = HTTP_GET;
    ws.handler = OnWebSocket;
    ws.is_websocket = true;
    httpd_register_uri_handler(server_, &ws);

    ESP_LOGI(TAG, "Big screen ready at %s", Url().c_str());
    return true;
}

std::string StemMirror::Url() const {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return "";
    }
    char url[32];
    snprintf(url, sizeof(url), "http://" IPSTR "/", IP2STR(&info.ip));
    return url;
}

void StemMirror::Publish(Kind kind, const std::string& json) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (kind) {
            case Kind::kContent:
                state_.assign(1, json);
                last_scene_.clear();
                break;
            case Kind::kUpdate:
                state_.push_back(json);
                break;
            case Kind::kScene:
                last_scene_ = json;
                break;
            case Kind::kClear:
                state_.clear();
                last_scene_.clear();
                break;
            case Kind::kTransient:
                break;
        }
    }
    if (server_ != nullptr) {
        Queue(-1, false, json);
    }
}

void StemMirror::Queue(int fd, bool replay, std::string payload) {
    auto job = new SendJob{this, fd, replay, std::move(payload)};
    if (httpd_queue_work(server_, DoSend, job) != ESP_OK) {
        delete job;
    }
}

void StemMirror::DoSend(void* arg) {
    std::unique_ptr<SendJob> job(static_cast<SendJob*>(arg));
    StemMirror* self = job->mirror;
    if (job->replay) {
        // Bring a new browser up to date: canvas size, current content, current scene
        char hello[64];
        snprintf(hello, sizeof(hello), "{\"type\":\"hello\",\"w\":%d,\"h\":%d}", self->canvas_w_,
                 self->canvas_h_);
        std::vector<std::string> messages{hello};
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            messages.insert(messages.end(), self->state_.begin(), self->state_.end());
            if (!self->last_scene_.empty()) {
                messages.push_back(self->last_scene_);
            }
        }
        for (const auto& message : messages) {
            self->Send(job->fd, message);
        }
        return;
    }
    // Send() may drop a client, so iterate over a copy
    std::vector<int> clients = self->clients_;
    for (int fd : clients) {
        self->Send(fd, job->payload);
    }
}

void StemMirror::Send(int fd, const std::string& payload) {
    if (httpd_ws_get_fd_info(server_, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
        return;
    }
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(payload.data()));
    frame.len = payload.size();
    if (httpd_ws_send_frame_async(server_, fd, &frame) != ESP_OK) {
        ESP_LOGW(TAG, "Dropping big-screen client %d", fd);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
    }
}

esp_err_t StemMirror::ServePage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, stem_mirror_html_start,
                           stem_mirror_html_end - stem_mirror_html_start);
}

esp_err_t StemMirror::OnWebSocket(httpd_req_t* req) {
    auto self = static_cast<StemMirror*>(httpd_get_global_user_ctx(req->handle));
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        // Handshake done
        if (std::find(self->clients_.begin(), self->clients_.end(), fd) == self->clients_.end()) {
            self->clients_.push_back(fd);
        }
        ESP_LOGI(TAG, "Big screen connected (%u open)", (unsigned)self->clients_.size());
        self->Queue(fd, true, "");
        return ESP_OK;
    }
    // The page does not send anything meaningful; read and discard frames, honour close
    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    if (frame.len > kMaxIncomingFrame) {
        return ESP_FAIL;  // closes the connection
    }
    if (frame.len > 0) {
        std::unique_ptr<uint8_t[]> buf(new uint8_t[frame.len]);
        frame.payload = buf.get();
        if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        self->clients_.erase(std::remove(self->clients_.begin(), self->clients_.end(), fd),
                             self->clients_.end());
    }
    return ESP_OK;
}

void StemMirror::OnClose(httpd_handle_t handle, int fd) {
    auto self = static_cast<StemMirror*>(httpd_get_global_user_ctx(handle));
    self->clients_.erase(std::remove(self->clients_.begin(), self->clients_.end(), fd),
                         self->clients_.end());
    close(fd);
}
