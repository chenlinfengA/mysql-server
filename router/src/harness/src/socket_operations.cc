/*
  Copyright (c) 2018, 2020, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "socket_operations.h"

#include <array>
#ifndef _WIN32
#include <arpa/inet.h>  // inet_ntop
#include <unistd.h>     // gethostname
#ifndef __APPLE__
#include <ifaddrs.h>
#include <net/if.h>
#endif
#else
#include <windows.h>
#include <winsock2.h>  // gethostname
#endif

#include "mysql/harness/net_ts/impl/poll.h"
#include "mysql/harness/net_ts/impl/resolver.h"
#include "mysql/harness/net_ts/impl/socket.h"
#include "mysql/harness/net_ts/impl/socket_error.h"
#include "mysql/harness/stdx/expected.h"

namespace mysql_harness {

SocketOperations *SocketOperations::instance() {
  static SocketOperations instance_;
  return &instance_;
}

SocketOperations::result<size_t> SocketOperations::poll(
    struct pollfd *fds, size_t nfds, std::chrono::milliseconds timeout_ms) {
  return net::impl::poll::poll(fds, nfds, timeout_ms);
}
SocketOperations::result<void> SocketOperations::connect_non_blocking_wait(
    socket_t sock, std::chrono::milliseconds timeout_ms) {
  std::array<struct pollfd, 1> fds = {
      {{sock, POLLOUT, 0}},
  };

  const auto poll_res = poll(fds.data(), fds.size(), timeout_ms);

  if (!poll_res) {
    return stdx::make_unexpected(poll_res.error());
  }

  const bool connect_writable = (fds[0].revents & POLLOUT) != 0;

  if (!connect_writable) {
    return stdx::make_unexpected(make_error_code(std::errc::invalid_argument));
  }

  return {};
}

SocketOperations::result<void> SocketOperations::connect_non_blocking_status(
    socket_t sock) {
  socklen_t so_error;
  auto error_len = static_cast<socklen_t>(sizeof(so_error));
  const auto res = net::impl::socket::getsockopt(sock, SOL_SOCKET, SO_ERROR,
                                                 &so_error, &error_len);
  if (!res) {
    return stdx::make_unexpected(res.error());
  }

  if (so_error) {
    return stdx::make_unexpected(net::impl::socket::make_error_code(so_error));
  }

  return {};
}

SocketOperations::result<size_t> SocketOperations::write(socket_t fd,
                                                         const void *buffer,
                                                         size_t nbyte) {
  return net::impl::socket::write(fd, buffer, nbyte);
}

SocketOperations::result<size_t> SocketOperations::read(socket_t fd,
                                                        void *buffer,
                                                        size_t nbyte) {
  return net::impl::socket::read(fd, buffer, nbyte);
}

SocketOperations::result<void> SocketOperations::close(socket_t fd) {
  return net::impl::socket::close(fd);
}

SocketOperations::result<void> SocketOperations::shutdown(socket_t fd) {
#ifndef _WIN32
  const int shut = SHUT_RDWR;
#else
  const int shut = SD_BOTH;
#endif

  return net::impl::socket::shutdown(fd, shut);
}

SocketOperations::result<std::unique_ptr<addrinfo, void (*)(addrinfo *)>>
SocketOperations::getaddrinfo(const char *node, const char *service,
                              const addrinfo *hints) {
  return net::impl::resolver::getaddrinfo(node, service, hints);
}

SocketOperations::result<void> SocketOperations::bind(
    socket_t fd, const struct sockaddr *addr, size_t len) {
  return net::impl::socket::bind(fd, addr, len);
}

SocketOperations::result<socket_t> SocketOperations::socket(int domain,
                                                            int type,
                                                            int protocol) {
  return net::impl::socket::socket(domain, type, protocol);
}

SocketOperations::result<void> SocketOperations::setsockopt(
    socket_t fd, int level, int optname, const void *optval, size_t optlen) {
  return net::impl::socket::setsockopt(fd, level, optname, optval, optlen);
}

SocketOperations::result<void> SocketOperations::listen(socket_t fd, int n) {
  return net::impl::socket::listen(fd, n);
}

SocketOperations::result<const char *> SocketOperations::inetntop(
    int af, const void *cp, char *buf, size_t len) {
  return net::impl::resolver::inetntop(af, cp, buf, len);
}

SocketOperations::result<void> SocketOperations::getpeername(
    socket_t fd, struct sockaddr *addr, size_t *len) {
  return net::impl::socket::getpeername(fd, addr, len);
}

std::string SocketOperations::get_local_hostname() {
  std::string buf;
  buf.resize(1024);

#if defined(_WIN32) || defined(__APPLE__) || defined(__FreeBSD__)
  auto res = net::impl::resolver::gethostname(&buf.front(), buf.size());
  if (!res) {
    throw LocalHostnameResolutionError(
        "Could not get local hostname: " + res.error().message() +
        " (error: " + std::to_string(res.error().value()) + ")");
  }
#else
  int family;

  std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> iface_addrs(nullptr,
                                                               &freeifaddrs);

  struct ifaddrs *ifa = nullptr;
  if (0 != getifaddrs(&ifa)) {
    const auto ec = net::impl::socket::last_error_code();

    throw LocalHostnameResolutionError(
        "Could not get local host address: " + ec.message() +
        "(errno: " + std::to_string(ec.value()) + ")");
  }

  // move ownership to the unique-ptr
  iface_addrs.reset(ifa);

  stdx::expected<void, std::error_code> getnameinfo_res{};
  for (const auto *ifap = iface_addrs.get(); ifap != nullptr;
       ifap = ifap->ifa_next) {
    if ((ifap->ifa_addr == nullptr) || (ifap->ifa_flags & IFF_LOOPBACK) ||
        (!(ifap->ifa_flags & IFF_UP)))
      continue;
    family = ifap->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6) continue;
    if (family == AF_INET6) {
      struct sockaddr_in6 *sin6;

      sin6 = (struct sockaddr_in6 *)ifap->ifa_addr;
      if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) ||
          IN6_IS_ADDR_MC_LINKLOCAL(&sin6->sin6_addr))
        continue;
    }
    const auto addrlen = static_cast<socklen_t>(
        (family == AF_INET) ? sizeof(struct sockaddr_in)
                            : sizeof(struct sockaddr_in6));

    getnameinfo_res =
        net::impl::resolver::getnameinfo(ifap->ifa_addr, addrlen, &buf.front(),
                                         buf.size(), nullptr, 0, NI_NAMEREQD);

    if (getnameinfo_res) break;
  }

  if (!getnameinfo_res &&
      getnameinfo_res.error() !=
          make_error_code(net::ip::resolver_errc::host_not_found)) {
    throw LocalHostnameResolutionError(
        "Could not get local hostname: " + getnameinfo_res.error().message() +
        " (ret: " + std::to_string(getnameinfo_res.error().value()) + ")");
  }
#endif

  // resize string to the first 0-char
  size_t nul_pos = buf.find('\0');
  if (nul_pos != std::string::npos) {
    buf.resize(nul_pos);
  }
  return buf;
}

SocketOperations::result<void> SocketOperations::set_socket_blocking(
    socket_t sock, bool blocking) {
  return net::impl::socket::native_non_blocking(sock, !blocking);
}

}  // namespace mysql_harness
