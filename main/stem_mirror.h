#ifndef STEM_MIRROR_H
#define STEM_MIRROR_H

#include <esp_http_server.h>

#include <mutex>
#include <string>
#include <vector>

// Mirrors the STEM overlay to any browser on the same network (a TV, laptop or projector).
// It serves a single web page at http://<device-ip>/ and streams JSON messages to it over a
// WebSocket at /ws; the page redraws diagrams, graphs, stories etc. at full resolution.
// Publish() may be called from any task; sockets are only touched from the httpd task.
class StemMirror {
public:
    enum class Kind {
        kContent,    // replaces what is shown (a new diagram, story, quiz...)
        kUpdate,     // changes the current content (a quiz answer)
        kScene,      // story scene change; only the latest is kept for late joiners
        kClear,      // nothing is shown
        kTransient,  // speech subtitles; not replayed to late joiners
    };

    static StemMirror& GetInstance() {
        static StemMirror instance;
        return instance;
    }

    // Starts the web server once; later calls do nothing. Needs the network to be up.
    bool Start(int canvas_width, int canvas_height);
    void Publish(Kind kind, const std::string& json);
    // http://<ip>/ of the Wi-Fi station interface, or an empty string when not connected.
    std::string Url() const;

private:
    struct SendJob {
        StemMirror* mirror;
        int fd;  // -1 broadcasts to every client
        bool replay;
        std::string payload;
    };

    httpd_handle_t server_ = nullptr;
    int canvas_w_ = 0;
    int canvas_h_ = 0;
    std::vector<int> clients_;  // httpd task only

    std::mutex mutex_;  // guards the replay state below
    std::vector<std::string> state_;
    std::string last_scene_;

    StemMirror() = default;
    void Queue(int fd, bool replay, std::string payload);
    void Send(int fd, const std::string& payload);

    static esp_err_t ServePage(httpd_req_t* req);
    static esp_err_t OnWebSocket(httpd_req_t* req);
    static void OnClose(httpd_handle_t handle, int fd);
    static void DoSend(void* arg);
};

#endif  // STEM_MIRROR_H
