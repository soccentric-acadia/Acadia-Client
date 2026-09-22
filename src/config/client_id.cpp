#include "config/client_id.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <vector>
#include <spdlog/spdlog.h>

namespace acadia::client::config {

namespace {

Result<std::string> generate_uuid() {
    std::ifstream f("/proc/sys/kernel/random/uuid");
    if (!f) {
        return Result<std::string>::err("cannot open /proc/sys/kernel/random/uuid");
    }
    std::string uuid;
    if (!std::getline(f, uuid) || uuid.empty()) {
        return Result<std::string>::err("empty read from /proc/sys/kernel/random/uuid");
    }
    // Trim any stray whitespace
    while (!uuid.empty() && (uuid.back() == '\n' || uuid.back() == '\r' || uuid.back() == ' ')) {
        uuid.pop_back();
    }
    return Result<std::string>::ok(std::move(uuid));
}

} // namespace

Result<std::string> load_or_create_client_id(const std::string& path) {
    // X-003: ONE device identity. When running under Acadia-OS, the OS's
    // device-identity service has provisioned /var/lib/acadia/device-id
    // ("acadia-<serial>") — that is the canonical, factory-provisioned
    // identity and takes precedence over this daemon's random UUID, so the
    // Client's stream board_id matches the OS device id and HQ pairing.
    {
        std::ifstream os_identity("/var/lib/acadia/device-id");
        if (os_identity) {
            std::string id;
            if (std::getline(os_identity, id) && !id.empty()) {
                spdlog::debug("using OS provisioned device-id from /var/lib/acadia/device-id");
                return Result<std::string>::ok(id);
            }
        }
    }

    // CLI-051: validate the persisted id — a garbage file is replaced with a
    // fresh identity (and a loud warning) instead of being trusted verbatim.
    // Accepted forms: a UUID (v4-shaped), or the OS device-id form
    // ("acadia-<serial>").
    {
        std::ifstream in(path);
        if (in) {
            std::string id;
            if (std::getline(in, id) && !id.empty()) {
                while (!id.empty() && (id.back() == '\r' || id.back() == ' ')) id.pop_back();
                bool uuid_like = (id.size() == 36 && id[8] == '-' && id[13] == '-' &&
                                  id[14] == '4' && id[18] == '-' && id[23] == '-');
                bool os_form = id.rfind("acadia-", 0) == 0 && id.size() > 7;
                if (uuid_like || os_form) {
                    spdlog::debug("loaded client_id from {}", path);
                    return Result<std::string>::ok(std::move(id));
                }
                spdlog::warn("client_id file contains an invalid id — regenerating");
            }
        }
    }

    // Generate a fresh UUID
    auto uuid = generate_uuid();
    if (!uuid) {
        return Result<std::string>::err("UUID generation failed: " + uuid.error());
    }

    // Ensure the parent directory exists
    std::error_code ec;
    std::filesystem::path p(path);
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) {
        return Result<std::string>::err(
            "cannot create directory " + p.parent_path().string() + ": " + ec.message());
    }

    // CLI-053/055: unique temp name (no concurrent-instance race) created with
    // 0644 permissions.
    std::string tmp_path = path + ".XXXXXX";
    std::vector<char> tmp_buf(tmp_path.begin(), tmp_path.end());
    tmp_buf.push_back('\0');
    const int tmp_fd = ::mkstemp(tmp_buf.data());
    if (tmp_fd < 0) {
        return Result<std::string>::err(std::string("mkstemp failed: ") + std::strerror(errno));
    }
    ::fchmod(tmp_fd, 0644);
    {
        std::string payload = *uuid + "\n";
        ssize_t written = ::write(tmp_fd, payload.data(), payload.size());
        // CLI-052: fsync the FILE before rename so the identity survives a
        // power cut between rename and flush.
        ::fsync(tmp_fd);
        ::close(tmp_fd);
        if (written < 0) {
            ::unlink(tmp_buf.data());
            return Result<std::string>::err("cannot write identity temp file");
        }
    }
    // CLI-052: fsync the DIRECTORY too so the rename itself is durable.
    {
        const int dir_fd = ::open(p.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (dir_fd >= 0) {
            ::fsync(dir_fd);
            ::close(dir_fd);
        }
    }
    std::filesystem::rename(tmp_buf.data(), path, ec);
    if (ec) {
        return Result<std::string>::err(
            std::string("cannot rename identity temp file -> ") + path + ": " + ec.message());
    }

    spdlog::info("generated new client_id={}", *uuid);
    return uuid;
}

} // namespace acadia::client::config
