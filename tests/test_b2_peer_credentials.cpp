#include "test/b2_control.hpp"

#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "[FAIL] " << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; return 1; } } while (false)

int main() {
    int fds[2]{};
    const int socketpair_result = ::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds);
    CHECK(socketpair_result == 0);
    if (socketpair_result != 0) return 1;

    pid_t peer_pid{};
    uid_t peer_uid{};
    gid_t peer_gid{};
    const bool credentials_ok = yarilo::test::b2::get_peer_credentials(fds[0], peer_pid, peer_uid, peer_gid);
    CHECK(credentials_ok);
    if (!credentials_ok) {
        ::close(fds[0]);
        ::close(fds[1]);
        return 1;
    }
    CHECK(peer_pid == ::getpid());
    CHECK(peer_uid == ::getuid());
    CHECK(peer_gid == ::getgid());

    ::close(fds[0]);
    ::close(fds[1]);

    peer_pid = 123;
    peer_uid = 456;
    peer_gid = 789;
    const bool invalid_fd_credentials = yarilo::test::b2::get_peer_credentials(-1, peer_pid, peer_uid, peer_gid);
    CHECK(!invalid_fd_credentials);
    CHECK(peer_pid == 123 && peer_uid == 456 && peer_gid == 789);
    return invalid_fd_credentials ? 1 : 0;
}
