#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "softswitch/shm/region_mapping.hpp"
#include "softswitch/sys/fd.hpp"
#include "softswitch/sys/shared_memory.hpp"
#include "softswitch/sys/unix_socket.hpp"

namespace softswitch {
namespace {

TEST(SharedMemory, CreateAndAttachSeeSameBytes) {
  auto a = sys::SharedMemory::create("test", 4096);
  ASSERT_NE(a.data(), nullptr);
  EXPECT_EQ(a.size(), 4096U);
  sys::UniqueFd dup{::dup(a.fd())};
  auto b = sys::SharedMemory::attach(std::move(dup));
  EXPECT_EQ(b.size(), 4096U);
  static_cast<char*>(a.data())[100] = 'x';
  EXPECT_EQ(static_cast<char*>(b.data())[100], 'x');
}

TEST(SharedMemory, SizeIsSealed) {
  auto a = sys::SharedMemory::create("sealed", 4096);
  EXPECT_LT(::ftruncate(a.fd(), 8192), 0);  // F_SEAL_GROW
}

TEST(Eventfd, SignalAndDrain) {
  auto efd = sys::make_eventfd();
  EXPECT_EQ(sys::eventfd_drain(efd.get()), 0U);  // non-blocking when empty
  EXPECT_TRUE(sys::eventfd_signal(efd.get()));
  EXPECT_TRUE(sys::eventfd_signal(efd.get()));
  EXPECT_EQ(sys::eventfd_drain(efd.get()), 2U);
}

TEST(Region, ConstructThenAttachValidates) {
  auto mem = shm::create_region_memory("region");
  auto& region = shm::construct_region(mem);
  region.tables[1].num_ports = 3;
  sys::UniqueFd dup{::dup(mem.fd())};
  auto other = sys::SharedMemory::attach(std::move(dup));
  auto& attached = shm::attach_region(other);
  EXPECT_EQ(attached.tables[1].num_ports, 3U);

  region.magic = 0;
  EXPECT_THROW((void)shm::attach_region(other), std::runtime_error);
}

// The real handshake: pass a memfd and an eventfd to a child process over a Unix socket
// (SCM_RIGHTS). The child writes through the shared mapping and signals the eventfd.
TEST(FdPassing, ChildProcessUsesPassedDescriptors) {
  auto mem = shm::create_region_memory("ipc");
  auto& region = shm::construct_region(mem);
  auto efd = sys::make_eventfd();

  std::array<int, 2> pair{};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair.data()), 0);
  sys::UniqueFd parent_end{pair[0]};
  sys::UniqueFd child_end{pair[1]};

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    parent_end.reset();
    int rc = 1;
    try {
      std::array<std::byte, 16> buf{};
      std::vector<sys::UniqueFd> fds;
      const auto n = sys::recv_with_fds(child_end.get(), buf, fds);
      if (n == 5 && fds.size() == 2) {
        auto shared = sys::SharedMemory::attach(std::move(fds[0]));
        auto& r = shm::attach_region(shared);
        (void)r.learn.try_push(shm::LearnEvent{MacAddress::from_u64(0x0200000000AA), 10, 2});
        r.dp.attached.store(1);
        rc = sys::eventfd_signal(fds[1].get()) ? 0 : 2;
      }
    } catch (...) {
      rc = 3;
    }
    ::_exit(rc);
  }
  child_end.reset();
  const std::array<int, 2> fds{mem.fd(), efd.get()};
  const char hello[] = "hello";
  sys::send_with_fds(parent_end.get(), std::as_bytes(std::span{hello}.first(5)), fds);

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(sys::eventfd_drain(efd.get()), 1U);
  EXPECT_EQ(region.dp.attached.load(), 1U);
  shm::LearnEvent ev;
  ASSERT_TRUE(region.learn.try_pop(ev));
  EXPECT_EQ(ev.mac, MacAddress::from_u64(0x0200000000AA));
  EXPECT_EQ(ev.vlan, 10);
  EXPECT_EQ(ev.port, 2);
}

TEST(UnixSocket, ListenConnectRoundTrip) {
  const std::string path = "/tmp/softswitch-test-" + std::to_string(::getpid()) + ".sock";
  auto listener = sys::unix_listen(path, SOCK_SEQPACKET);
  auto client = sys::unix_connect(path, SOCK_SEQPACKET);
  sys::UniqueFd server{::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)};
  ASSERT_TRUE(server.valid());
  const char msg[] = "ping";
  sys::send_with_fds(client.get(), std::as_bytes(std::span{msg}.first(4)), {});
  std::array<std::byte, 8> buf{};
  std::vector<sys::UniqueFd> fds;
  EXPECT_EQ(sys::recv_with_fds(server.get(), buf, fds), 4U);
  EXPECT_TRUE(fds.empty());
  client.reset();
  EXPECT_EQ(sys::recv_with_fds(server.get(), buf, fds), 0U);  // EOF
  ::unlink(path.c_str());
}

}  // namespace
}  // namespace softswitch
