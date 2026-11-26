// SPDX-License-Identifier: GPL-2.0-only
// File by: 'femboy'
// Archivo hecho por: 'femboy'

#ifndef TRIONYX_UNIX_SOCKET_HPP
#define TRIONYX_UNIX_SOCKET_HPP

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace trionyx::ipc {

inline constexpr std::uint32_t default_max_frame_size = 16U * 1024U * 1024U;

namespace detail {

inline bool make_address(const char *path, sockaddr_un &address,
                         socklen_t &address_size) noexcept {
    if (path == nullptr || path[0] == '\0') {
        errno = EINVAL;
        return false;
    }

    const std::size_t path_size = std::strlen(path);
    if (path_size >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return false;
    }

    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path, path_size + 1);
    address_size =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path_size + 1);
    return true;
}

inline bool set_close_on_exec(int fd) noexcept {
    int descriptor_flags;
    do {
        descriptor_flags = ::fcntl(fd, F_GETFD);
    } while (descriptor_flags < 0 && errno == EINTR);
    if (descriptor_flags < 0) {
        return false;
    }

    int result;
    do {
        result = ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

inline int create_stream_socket() noexcept {
#if defined(SOCK_CLOEXEC)
    const int cloexec_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (cloexec_fd >= 0 || (errno != EINVAL && errno != EPROTONOSUPPORT)) {
        return cloexec_fd;
    }
#endif

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (!set_close_on_exec(fd)) {
        const int saved_errno = errno;
        (void)::close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

inline int accept_stream_socket(int listening_fd) noexcept {
    int client_fd;
#if defined(__linux__) && defined(SOCK_CLOEXEC)
    do {
        client_fd = ::accept4(listening_fd, nullptr, nullptr, SOCK_CLOEXEC);
    } while (client_fd < 0 && errno == EINTR);
    if (client_fd >= 0 || (errno != ENOSYS && errno != EINVAL)) {
        return client_fd;
    }
#endif

    do {
        client_fd = ::accept(listening_fd, nullptr, nullptr);
    } while (client_fd < 0 && errno == EINTR);
    if (client_fd < 0) {
        return -1;
    }
    if (!set_close_on_exec(client_fd)) {
        const int saved_errno = errno;
        (void)::close(client_fd);
        errno = saved_errno;
        return -1;
    }
    return client_fd;
}

inline bool configure_sigpipe(int fd) noexcept {
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    return ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                        sizeof(enabled)) == 0;
#else
    (void)fd;
    return true;
#endif
}

inline int send_flags() noexcept {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

} // namespace detail

class UnixStream;

inline bool send_frame(UnixStream &stream, const void *payload,
                       std::uint32_t size) noexcept;
inline bool send_frame(UnixStream &stream, std::string_view payload) noexcept;
inline bool receive_frame(UnixStream &stream, void *buffer,
                          std::uint32_t capacity,
                          std::uint32_t &received_size) noexcept;
inline bool receive_frame(UnixStream &stream, std::string &payload,
                          std::uint32_t max_size = default_max_frame_size);

// Owns one connected AF_UNIX SOCK_STREAM descriptor. Errors are reported
// through errno; methods do not throw exceptions.
class UnixStream {
  public:
    UnixStream() noexcept = default;
    explicit UnixStream(int fd) noexcept : fd_(fd) {}

    ~UnixStream() { close(); }

    UnixStream(const UnixStream &) = delete;
    UnixStream &operator=(const UnixStream &) = delete;

    UnixStream(UnixStream &&other) noexcept : fd_(other.release()) {}

    UnixStream &operator=(UnixStream &&other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.release();
        }
        return *this;
    }

    bool connect(const char *path) noexcept {
        close();

        sockaddr_un address{};
        socklen_t address_size = 0;
        if (!detail::make_address(path, address, address_size)) {
            return false;
        }

        const int fd = detail::create_stream_socket();
        if (fd < 0) {
            return false;
        }
        if (!detail::configure_sigpipe(fd)) {
            const int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            return false;
        }
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                      address_size) < 0) {
            const int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            return false;
        }

        fd_ = fd;
        return true;
    }

    bool connect(const std::string &path) noexcept {
        if (path.find('\0') != std::string::npos) {
            close();
            errno = EINVAL;
            return false;
        }
        return connect(path.c_str());
    }

    // Compact client-facing spelling; it connects to a listening AF_UNIX path.
    bool hear(const char *path) noexcept { return connect(path); }
    bool hear(const std::string &path) noexcept { return connect(path); }

    // Framed, binary-safe member I/O. Existing free functions are unchanged.
    // Returns nullopt on I/O/size errors; an empty optional value is a valid
    // zero-length frame. The output-parameter overload remains for compatibility.
    std::optional<std::string> read(
        std::uint32_t max_size = default_max_frame_size);
    bool read(std::string &payload,
              std::uint32_t max_size = default_max_frame_size);
    bool read(void *buffer, std::uint32_t capacity,
              std::uint32_t &received_size) noexcept;
    bool write(std::string_view payload) noexcept;
    bool write(const void *payload, std::uint32_t size) noexcept;

    bool valid() const noexcept { return fd_ >= 0; }
    int native_handle() const noexcept { return fd_; }

    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void close() noexcept {
        if (fd_ >= 0) {
            const int fd = fd_;
            fd_ = -1;
            ::close(fd);
        }
    }

    bool send_all(const void *data, std::size_t size) noexcept {
        if (fd_ < 0 || (data == nullptr && size != 0)) {
            errno = (fd_ < 0) ? EBADF : EINVAL;
            return false;
        }

        const auto *bytes = static_cast<const unsigned char *>(data);
        std::size_t sent = 0;
        while (sent < size) {
            const ssize_t result =
                ::send(fd_, bytes + sent, size - sent, detail::send_flags());
            if (result > 0) {
                sent += static_cast<std::size_t>(result);
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result == 0) {
                errno = EPIPE;
            }
            return false;
        }
        return true;
    }

    bool receive_exact(void *data, std::size_t size) noexcept {
        if (fd_ < 0 || (data == nullptr && size != 0)) {
            errno = (fd_ < 0) ? EBADF : EINVAL;
            return false;
        }

        auto *bytes = static_cast<unsigned char *>(data);
        std::size_t received = 0;
        while (received < size) {
            const ssize_t result =
                ::recv(fd_, bytes + received, size - received, 0);
            if (result > 0) {
                received += static_cast<std::size_t>(result);
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result == 0) {
                errno = ECONNRESET;
            }
            return false;
        }
        return true;
    }

  private:
    int fd_ = -1;
};

// A small AF_UNIX stream listener. It does not remove the filesystem socket
// path on close; unlink the path explicitly after closing the server.
class UnixServer {
  public:
    UnixServer() noexcept = default;
    ~UnixServer() { close(); }

    UnixServer(const UnixServer &) = delete;
    UnixServer &operator=(const UnixServer &) = delete;

    UnixServer(UnixServer &&other) noexcept : fd_(other.release()) {}

    UnixServer &operator=(UnixServer &&other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.release();
        }
        return *this;
    }

    bool listen(const char *path, int backlog = 16) noexcept {
        close();

        sockaddr_un address{};
        socklen_t address_size = 0;
        if (!detail::make_address(path, address, address_size)) {
            return false;
        }

        const int fd = detail::create_stream_socket();
        if (fd < 0) {
            return false;
        }
        if (!detail::configure_sigpipe(fd)) {
            const int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            return false;
        }
        if (::bind(fd, reinterpret_cast<const sockaddr *>(&address),
                   address_size) < 0 ||
            ::listen(fd, backlog) < 0) {
            const int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            return false;
        }

        fd_ = fd;
        return true;
    }

    bool listen(const std::string &path, int backlog = 16) noexcept {
        if (path.find('\0') != std::string::npos) {
            close();
            errno = EINVAL;
            return false;
        }
        return listen(path.c_str(), backlog);
    }

    UnixStream accept(int *error_code = nullptr) noexcept {
        if (fd_ < 0) {
            if (error_code != nullptr) {
                *error_code = EBADF;
            }
            errno = EBADF;
            return UnixStream{};
        }

        const int client_fd = detail::accept_stream_socket(fd_);

        if (client_fd < 0) {
            if (error_code != nullptr) {
                *error_code = errno;
            }
            return UnixStream{};
        }
        if (!detail::configure_sigpipe(client_fd)) {
            const int saved_errno = errno;
            ::close(client_fd);
            if (error_code != nullptr) {
                *error_code = saved_errno;
            }
            errno = saved_errno;
            return UnixStream{};
        }

        if (error_code != nullptr) {
            *error_code = 0;
        }
        return UnixStream{client_fd};
    }

    bool valid() const noexcept { return fd_ >= 0; }
    int native_handle() const noexcept { return fd_; }

    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void close() noexcept {
        if (fd_ >= 0) {
            const int fd = fd_;
            fd_ = -1;
            ::close(fd);
        }
    }

  private:
    int fd_ = -1;
};

// Wire format: a 4-byte unsigned payload length in network byte order,
// followed by exactly that many payload bytes. Payloads are binary-safe.
// If receive_frame reports EMSGSIZE, close/discard the stream: the frame body
// has not been consumed and the next read would otherwise be misaligned.
inline bool send_frame(UnixStream &stream, const void *payload,
                       std::uint32_t size) noexcept {
    if (payload == nullptr && size != 0) {
        errno = EINVAL;
        return false;
    }

    const std::uint32_t network_size = htonl(size);
    if (!stream.send_all(&network_size, sizeof(network_size))) {
        return false;
    }
    return size == 0 || stream.send_all(payload, size);
}

// Convenient binary-safe overload for std::string and string literals.
inline bool send_frame(UnixStream &stream, std::string_view payload) noexcept {
    if (payload.size() > UINT32_MAX) {
        errno = EMSGSIZE;
        return false;
    }
    return send_frame(stream, payload.data(),
                      static_cast<std::uint32_t>(payload.size()));
}

inline bool receive_frame(UnixStream &stream, void *buffer,
                          std::uint32_t capacity,
                          std::uint32_t &received_size) noexcept {
    received_size = 0;
    std::uint32_t network_size = 0;
    if (!stream.receive_exact(&network_size, sizeof(network_size))) {
        return false;
    }

    const std::uint32_t frame_size = ntohl(network_size);
    if (frame_size > capacity || (buffer == nullptr && frame_size != 0)) {
        errno = (buffer == nullptr && frame_size != 0) ? EINVAL : EMSGSIZE;
        return false;
    }
    if (frame_size != 0 && !stream.receive_exact(buffer, frame_size)) {
        return false;
    }

    received_size = frame_size;
    return true;
}

// Reads a frame directly into a std::string. The default cap avoids allocating
// unbounded memory from an untrusted peer. On EMSGSIZE, the frame body remains
// unread, so the caller should discard/close the stream.
inline bool receive_frame(UnixStream &stream, std::string &payload,
                          std::uint32_t max_size) {
    payload.clear();
    std::uint32_t network_size = 0;
    if (!stream.receive_exact(&network_size, sizeof(network_size))) {
        return false;
    }

    const std::uint32_t frame_size = ntohl(network_size);
    if (frame_size > max_size) {
        errno = EMSGSIZE;
        return false;
    }

    payload.resize(frame_size);
    if (frame_size != 0 && !stream.receive_exact(payload.data(), frame_size)) {
        payload.clear();
        return false;
    }
    return true;
}

// Convenience alias, not a second implementation: old UnixStream users and
// sysdata keep the same class and descriptor semantics.
using sockstream = UnixStream;

inline std::optional<std::string> UnixStream::read(std::uint32_t max_size) {
    std::string payload;
    if (!receive_frame(*this, payload, max_size)) {
        return std::nullopt;
    }
    return payload;
}

inline bool UnixStream::read(std::string &payload, std::uint32_t max_size) {
    return receive_frame(*this, payload, max_size);
}

inline bool UnixStream::read(void *buffer, std::uint32_t capacity,
                             std::uint32_t &received_size) noexcept {
    return receive_frame(*this, buffer, capacity, received_size);
}

inline bool UnixStream::write(std::string_view payload) noexcept {
    return send_frame(*this, payload);
}

inline bool UnixStream::write(const void *payload, std::uint32_t size) noexcept {
    return send_frame(*this, payload, size);
}

} // namespace trionyx::ipc

#endif // TRIONYX_UNIX_SOCKET_HPP
