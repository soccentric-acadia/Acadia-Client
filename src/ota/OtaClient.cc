#include "ota/OtaClient.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern char** environ;

namespace acadia::client {

static std::mutex ota_mtx;
static bool       ota_running = false;

// CLI-148: swupdate accepts a small set of URL schemes; anything else is
// rejected before spawn. posix_spawn passes the URL as argv (never through a
// shell), so metacharacter injection is impossible, but explicit validation
// keeps malformed URLs from producing confusing swupdate failures.
static bool url_is_acceptable(const std::string& url) {
    static const char* kSchemes[] = {"http://", "https://", "ftp://", "ftps://"};
    bool has_scheme = false;
    for (const char* s : kSchemes) {
        if (url.rfind(s, 0) == 0) { has_scheme = true; break; }
    }
    if (!has_scheme) return false;
    if (url.size() < 8 || url.size() > 2048) return false;
    for (unsigned char c : url) {
        if (c <= 0x20 || c == 0x7f) return false;  // control chars / whitespace
        if (c == '\'' || c == '"' || c == '\\') return false;
    }
    return true;
}

OtaClient::OtaClient(OtaStatusCb cb) : cb_(std::move(cb)) {}

bool OtaClient::trigger(const std::string& image_url) {
    std::lock_guard<std::mutex> lk(ota_mtx);
    if (ota_running) {
        spdlog::warn("ota: already running, ignoring trigger");
        return false;
    }
    ota_running = true;
    spdlog::info("ota: triggering upgrade from {}", image_url);

    // CLI-148: never build a shell command from a server-supplied URL.
    // Validate the URL, then spawn swupdate directly with argv so the string
    // can never be interpreted by a shell.
    if (!url_is_acceptable(image_url)) {
        spdlog::error("ota: rejected invalid image_url (bad scheme or characters)");
        ota_running = false;
        if (cb_) cb_("error", "invalid image URL");
        return false;
    }

    // swupdate listens on /tmp/sockinstall; feed it the image via pipe.
    // For a network URL, swupdate can fetch directly with -u flag.
    const char* argv[] = {"swupdate", "-u", image_url.c_str(), nullptr};
    pid_t pid = -1;
    {
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0 ||
            posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                             "/dev/null", O_WRONLY, 0) != 0 ||
            posix_spawn(&pid, "/usr/bin/swupdate", &actions, nullptr,
                        const_cast<char* const*>(argv), environ) != 0) {
            pid = -1;
        }
        posix_spawn_file_actions_destroy(&actions);
    }
    if (pid <= 0) {
        spdlog::error("ota: swupdate spawn failed errno={}", errno);
        ota_running = false;
        if (cb_) cb_("error", "swupdate launch failed");
        return false;
    }

    // Reap the spawned process without blocking the caller for long: swupdate
    // continues in its own daemon context; we only wait to surface immediate
    // launch failures.
    int status = 0;
    pid_t r = waitpid(pid, &status, 0);
    if (r == pid && WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        spdlog::error("ota: swupdate exited rc={}", WEXITSTATUS(status));
        ota_running = false;
        if (cb_) cb_("error", "swupdate launch failed");
        return false;
    }

    if (cb_) cb_("started", "swupdate triggered for " + image_url);
    ota_running = false;
    return true;
}

bool OtaClient::rollback() {
    spdlog::info("ota: requesting rollback");
    // Write 0 to /sys/firmware/efi/efivars/... or use fw_setenv.
    int rc = std::system("fw_setenv upgrade_available 0 && reboot");  // NOLINT
    if (rc != 0) {
        spdlog::error("ota: rollback failed rc={}", rc);
        if (cb_) cb_("error", "rollback failed");
        return false;
    }
    if (cb_) cb_("rollback", "rollback initiated");
    return true;
}

} // namespace acadia::client
