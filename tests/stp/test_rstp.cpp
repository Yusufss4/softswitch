#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <vector>

#include "softswitch/stp/rstp.hpp"

namespace softswitch::stp {
namespace {

using namespace std::chrono_literals;

/// A network of RSTP bridges joined by point-to-point links, with a simulated clock.
/// BPDUs are delivered in FIFO order until the network is quiet, then time advances 100 ms.
class SimNet {
 public:
  struct Bridge {
    std::unique_ptr<Rstp> rstp;
    std::size_t flushes = 0;
  };
  struct Link {
    std::size_t a;
    PortId pa;
    std::size_t b;
    PortId pb;
    bool up = true;
    bool lossy = false;  ///< Ports stay up but BPDUs vanish (e.g. unidirectional failure).
  };

  std::size_t add_bridge(std::uint16_t priority, std::uint64_t mac, std::size_t ports) {
    const std::size_t index = bridges_.size();
    BridgeConfig cfg;
    cfg.priority = priority;
    cfg.mac = MacAddress::from_u64(mac);
    Rstp::Callbacks cb;
    cb.send_bpdu = [this, index](PortId port, const pkt::Bpdu& bpdu) { send(index, port, bpdu); };
    cb.flush = [this, index](PortId) { ++bridges_[index].flushes; };
    bridges_.push_back(Bridge{std::make_unique<Rstp>(cfg, ports, cb)});
    return index;
  }

  std::size_t connect(std::size_t a, PortId pa, std::size_t b, PortId pb) {
    links_.push_back(Link{a, pa, b, pb});
    bridges_[a].rstp->set_port_enabled(pa, true, now_);
    bridges_[b].rstp->set_port_enabled(pb, true, now_);
    deliver();
    return links_.size() - 1;
  }

  void set_link_up(std::size_t link, bool up) {
    Link& l = links_[link];
    l.up = up;
    bridges_[l.a].rstp->set_port_enabled(l.pa, up, now_);
    bridges_[l.b].rstp->set_port_enabled(l.pb, up, now_);
    deliver();
  }
  void set_lossy(std::size_t link, bool lossy) { links_[link].lossy = lossy; }

  void run_for(std::chrono::milliseconds duration) {
    const auto end = now_ + duration;
    while (now_ < end) {
      deliver();
      now_ += 100ms;
      for (auto& b : bridges_) {
        b.rstp->tick(now_);
      }
    }
    deliver();
  }

  Rstp& operator[](std::size_t i) { return *bridges_[i].rstp; }
  [[nodiscard]] std::size_t flushes(std::size_t i) const { return bridges_[i].flushes; }
  [[nodiscard]] Clock::time_point now() const { return now_; }

  [[nodiscard]] const std::vector<Link>& links() const { return links_; }
  [[nodiscard]] std::size_t size() const { return bridges_.size(); }

  [[nodiscard]] std::size_t count_role(Role role) const {
    std::size_t n = 0;
    for (const auto& b : bridges_) {
      for (PortId p = 0; p < b.rstp->num_ports(); ++p) {
        n += (b.rstp->status(p).enabled && b.rstp->status(p).role == role) ? 1U : 0U;
      }
    }
    return n;
  }

 private:
  struct Message {
    std::size_t bridge;
    PortId port;
    pkt::Bpdu bpdu;
  };

  void send(std::size_t from, PortId port, const pkt::Bpdu& bpdu) {
    for (const Link& l : links_) {
      if (!l.up || l.lossy) {
        continue;
      }
      if (l.a == from && l.pa == port) {
        queue_.push_back({l.b, l.pb, bpdu});
      } else if (l.b == from && l.pb == port) {
        queue_.push_back({l.a, l.pa, bpdu});
      }
    }
  }

  void deliver() {
    std::size_t delivered = 0;
    while (!queue_.empty()) {
      ASSERT_LT(++delivered, 100000U) << "BPDU storm: the protocol never went quiet";
      const Message m = queue_.front();
      queue_.pop_front();
      bridges_[m.bridge].rstp->receive(m.port, m.bpdu, now_);
    }
  }

  std::vector<Bridge> bridges_;
  std::vector<Link> links_;
  std::deque<Message> queue_;
  Clock::time_point now_{};
};

/// The integration-test topology: A is the intended root, B < C by MAC.
///   A0 -- B0,   A1 -- C0,   B1 -- C1
class TriangleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    a_ = net_.add_bridge(4096, 0x02000000000A, 3);
    b_ = net_.add_bridge(32768, 0x02000000000B, 3);
    c_ = net_.add_bridge(32768, 0x02000000000C, 3);
    ab_ = net_.connect(a_, 0, b_, 0);
    ac_ = net_.connect(a_, 1, c_, 0);
    bc_ = net_.connect(b_, 1, c_, 1);
  }

  void expect_converged_with_root(std::size_t root) {
    for (std::size_t i : {a_, b_, c_}) {
      EXPECT_EQ(net_[i].root_id(), net_[root].bridge_id()) << "bridge " << i;
    }
    EXPECT_TRUE(net_[root].is_root_bridge());
  }

  SimNet net_;
  std::size_t a_ = 0, b_ = 0, c_ = 0;
  std::size_t ab_ = 0, ac_ = 0, bc_ = 0;
};

TEST_F(TriangleTest, ConvergesInUnderASecondWithExactlyOneAlternate) {
  net_.run_for(1000ms);  // far below the 15 s forward delay: only proposal/agreement can do this
  expect_converged_with_root(a_);
  EXPECT_EQ(net_[b_].root_port(), PortId{0});
  EXPECT_EQ(net_[c_].root_port(), PortId{0});
  EXPECT_EQ(net_.count_role(Role::kAlternate), 1U);
  // B wins the B-C segment (same cost to the root, lower bridge ID): C's side blocks.
  EXPECT_EQ(net_[b_].status(1).role, Role::kDesignated);
  EXPECT_EQ(net_[c_].status(1).role, Role::kAlternate);
  EXPECT_EQ(net_[c_].status(1).state, State::kDiscarding);
  for (std::size_t i : {a_, b_, c_}) {
    for (PortId p = 0; p < 2; ++p) {
      if (i == c_ && p == 1) {
        continue;
      }
      EXPECT_EQ(net_[i].status(p).state, State::kForwarding) << "bridge " << i << " port " << p;
    }
  }
  EXPECT_EQ(net_[a_].root_cost(), 0U);
  EXPECT_EQ(net_[b_].root_cost(), 20000U);
}

TEST_F(TriangleTest, StaysStableOverTime) {
  net_.run_for(1000ms);
  const auto tc_before = net_[b_].topology_changes();
  net_.run_for(60s);  // hellos keep flowing, nothing should change
  EXPECT_EQ(net_.count_role(Role::kAlternate), 1U);
  EXPECT_EQ(net_[c_].status(1).state, State::kDiscarding);
  EXPECT_EQ(net_[b_].topology_changes(), tc_before);
}

TEST_F(TriangleTest, LinkFailureFailsOverInUnderASecond) {
  net_.run_for(1000ms);
  const auto flushes_before = net_.flushes(c_);
  net_.set_link_up(ab_, false);
  net_.run_for(1000ms);
  expect_converged_with_root(a_);
  EXPECT_EQ(net_[b_].root_port(), PortId{1});  // B now reaches A through C
  EXPECT_EQ(net_[b_].root_cost(), 40000U);
  EXPECT_EQ(net_[b_].status(1).state, State::kForwarding);
  EXPECT_EQ(net_[c_].status(1).role, Role::kDesignated);
  EXPECT_EQ(net_[c_].status(1).state, State::kForwarding);
  EXPECT_EQ(net_.count_role(Role::kAlternate), 0U);
  EXPECT_GT(net_.flushes(c_), flushes_before);  // topology change flushed learned MACs
}

TEST_F(TriangleTest, LinkRecoveryRestoresTheOriginalTree) {
  net_.run_for(1000ms);
  net_.set_link_up(ab_, false);
  net_.run_for(1000ms);
  net_.set_link_up(ab_, true);
  net_.run_for(1000ms);
  EXPECT_EQ(net_[b_].root_port(), PortId{0});
  EXPECT_EQ(net_[c_].status(1).role, Role::kAlternate);
  EXPECT_EQ(net_.count_role(Role::kAlternate), 1U);
}

TEST_F(TriangleTest, SilentLinkFailureRecoversWhenInformationExpires) {
  net_.run_for(1000ms);
  net_.set_lossy(ab_, true);  // ports stay up, BPDUs vanish
  net_.run_for(4000ms);
  EXPECT_EQ(net_[b_].root_port(), PortId{0});  // still believes the old info (< 3 x hello)
  net_.run_for(3000ms);
  EXPECT_EQ(net_[b_].root_port(), PortId{1});
  expect_converged_with_root(a_);
}

TEST_F(TriangleTest, RootBridgeFailureElectsTheNextBest) {
  net_.run_for(1000ms);
  net_.set_link_up(ab_, false);
  net_.set_link_up(ac_, false);
  net_.run_for(1000ms);
  EXPECT_TRUE(net_[b_].is_root_bridge());  // B < C by MAC
  EXPECT_EQ(net_[c_].root_id(), net_[b_].bridge_id());
  EXPECT_EQ(net_[c_].root_port(), PortId{1});
  EXPECT_EQ(net_[c_].status(1).state, State::kForwarding);
}

TEST_F(TriangleTest, PriorityChangeMovesTheRoot) {
  net_.run_for(1000ms);
  net_[c_].set_bridge_priority(0, net_.now());
  net_.run_for(1000ms);
  expect_converged_with_root(c_);
  EXPECT_EQ(net_.count_role(Role::kAlternate), 1U);
  // A and B now meet on the A-B link; A (4096) beats B (32768) as designated, B blocks.
  EXPECT_EQ(net_[b_].status(0).role, Role::kAlternate);
}

TEST(Rstp, EdgePortForwardsImmediatelyAndLosesEdgeOnBpdu) {
  SimNet net;
  const auto a = net.add_bridge(32768, 0x02000000000A, 2);
  PortConfig edge;
  edge.edge = true;
  net[a].set_port_config(1, edge, net.now());
  net[a].set_port_enabled(1, true, net.now());
  EXPECT_EQ(net[a].status(1).state, State::kForwarding);
  EXPECT_TRUE(net[a].status(1).oper_edge);

  pkt::Bpdu bpdu;
  bpdu.root_id = make_bridge_id(32768, MacAddress::from_u64(0x02000000000F));
  bpdu.bridge_id = bpdu.root_id;
  bpdu.set_role(pkt::BpduRole::kDesignated);
  bpdu.max_age = 20 * 256;
  net[a].receive(1, bpdu, net.now());
  EXPECT_FALSE(net[a].status(1).oper_edge);
}

TEST(Rstp, ForwardDelayFallbackWithoutAnAgreement) {
  SimNet net;
  const auto a = net.add_bridge(32768, 0x02000000000A, 1);
  net[a].set_port_enabled(0, true, net.now());  // nobody on the other end answers
  net.run_for(1000ms);
  EXPECT_EQ(net[a].status(0).role, Role::kDesignated);
  EXPECT_EQ(net[a].status(0).state, State::kDiscarding);
  net.run_for(15000ms);
  EXPECT_EQ(net[a].status(0).state, State::kLearning);
  net.run_for(15000ms);
  EXPECT_EQ(net[a].status(0).state, State::kForwarding);
}

TEST(Rstp, DesignatedPortProposesInItsBpdus) {
  std::vector<pkt::Bpdu> sent;
  BridgeConfig cfg;
  cfg.mac = MacAddress::from_u64(0x02000000000A);
  Rstp::Callbacks cb;
  cb.send_bpdu = [&](PortId, const pkt::Bpdu& b) { sent.push_back(b); };
  Rstp rstp(cfg, 1, cb);
  rstp.set_port_enabled(0, true, Clock::time_point{});
  ASSERT_FALSE(sent.empty());
  const auto& b = sent.back();
  EXPECT_EQ(b.type, pkt::kBpduTypeRst);
  EXPECT_EQ(b.role(), pkt::BpduRole::kDesignated);
  EXPECT_TRUE(b.has(pkt::bpdu_flag::kProposal));
  EXPECT_EQ(b.root_id, rstp.bridge_id());
  EXPECT_EQ(b.port_id, 0x8001);
  EXPECT_EQ(b.hello_time, 2 * 256);
}

TEST(Rstp, IgnoresBpdusThatExceededMaxAge) {
  BridgeConfig cfg;
  cfg.mac = MacAddress::from_u64(0x02000000000B);
  Rstp rstp(cfg, 1, {});
  rstp.set_port_enabled(0, true, Clock::time_point{});
  pkt::Bpdu bpdu;
  bpdu.root_id = make_bridge_id(0, MacAddress::from_u64(1));
  bpdu.bridge_id = bpdu.root_id;
  bpdu.set_role(pkt::BpduRole::kDesignated);
  bpdu.max_age = 20 * 256;
  bpdu.message_age = 20 * 256;
  rstp.receive(0, bpdu, Clock::time_point{});
  EXPECT_TRUE(rstp.is_root_bridge());
  bpdu.message_age = 19 * 256;
  rstp.receive(0, bpdu, Clock::time_point{});
  EXPECT_FALSE(rstp.is_root_bridge());
}

TEST(Rstp, LoopedBackOwnBpduMakesABackupPort) {
  SimNet net;
  const auto a = net.add_bridge(32768, 0x02000000000A, 2);
  (void)net.connect(a, 0, a, 1);  // a cable from the bridge to itself
  net.run_for(1000ms);
  EXPECT_TRUE(net[a].is_root_bridge());
  EXPECT_EQ(net.count_role(Role::kBackup), 1U);
  EXPECT_EQ(net.count_role(Role::kDesignated), 1U);
}

/// Union-find over bridges, to check the active topology.
struct Components {
  std::vector<std::size_t> parent;
  explicit Components(std::size_t n) : parent(n) {
    for (std::size_t i = 0; i < n; ++i) parent[i] = i;
  }
  std::size_t find(std::size_t x) { return parent[x] == x ? x : parent[x] = find(parent[x]); }
  bool unite(std::size_t a, std::size_t b) {
    a = find(a);
    b = find(b);
    if (a == b) return false;
    parent[a] = b;
    return true;
  }
};

/// The links forwarding at both ends must form a spanning tree of every physically connected
/// component: no cycles (no broadcast storm) and nothing cut off (full reachability).
void expect_spanning_tree(SimNet& net, unsigned seed, bool require_connectivity = true) {
  Components physical(net.size());
  Components active(net.size());
  for (const auto& l : net.links()) {
    if (!l.up) continue;
    physical.unite(l.a, l.b);
    const bool fwd = net[l.a].status(l.pa).state == State::kForwarding &&
                     net[l.b].status(l.pb).state == State::kForwarding;
    if (fwd) {
      EXPECT_TRUE(active.unite(l.a, l.b)) << "seed " << seed << ": forwarding loop";
    }
  }
  for (std::size_t i = 0; require_connectivity && i < net.size(); ++i) {
    for (std::size_t j = i + 1; j < net.size(); ++j) {
      if (physical.find(i) == physical.find(j)) {
        EXPECT_EQ(active.find(i), active.find(j))
            << "seed " << seed << ": bridges " << i << "," << j << " cut off";
      }
    }
  }
}

TEST(RstpProperty, RandomMeshesConvergeToASpanningTreeAndSurviveFailures) {
  for (unsigned seed = 1; seed <= 500; ++seed) {
    std::uint64_t rng = seed * 0x9E3779B97F4A7C15ULL;
    auto next = [&rng](std::uint64_t bound) {
      rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
      return (rng >> 33U) % bound;
    };
    SimNet net;
    const std::size_t n = 3 + next(5);  // 3..7 bridges
    constexpr std::size_t kPorts = 6;
    std::vector<PortId> used(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
      (void)net.add_bridge(static_cast<std::uint16_t>(4096 * (1 + next(8))), 0x020000000100ULL + i,
                           kPorts);
    }
    // A random spanning chain keeps it connected, plus random extra links (loops, parallel links).
    for (std::size_t i = 1; i < n; ++i) {
      const std::size_t j = next(i);
      (void)net.connect(i, used[i]++, j, used[j]++);
    }
    for (std::size_t extra = next(n + 2); extra > 0; --extra) {
      const std::size_t a = next(n);
      const std::size_t b = next(n);
      if (a != b && used[a] < kPorts && used[b] < kPorts) {
        (void)net.connect(a, used[a]++, b, used[b]++);
      }
    }
    net.run_for(1000ms);  // well below forward delay: proposal/agreement must do it
    expect_spanning_tree(net, seed);

    // Fail up to two random links and check again.
    for (int k = 0; k < 2; ++k) {
      std::vector<bool> was_root(net.size());
      for (std::size_t i = 0; i < net.size(); ++i) {
        was_root[i] = net[i].is_root_bridge();
      }
      net.set_link_up(next(net.links().size()), false);
      // Did some connected group lose its root bridge? Then the old root's information has to
      // die out first (bounded by max age: RSTP's count-to-infinity), which takes longer.
      Components physical(net.size());
      for (const auto& l : net.links()) {
        if (l.up) physical.unite(l.a, l.b);
      }
      std::vector<bool> has_root(net.size(), false);
      for (std::size_t i = 0; i < net.size(); ++i) {
        if (was_root[i]) has_root[physical.find(i)] = true;
      }
      bool root_lost = false;
      for (std::size_t i = 0; i < net.size(); ++i) {
        root_lost = root_lost || !has_root[physical.find(i)];
      }
      net.run_for(1000ms);
      expect_spanning_tree(net, seed, /*require_connectivity=*/!root_lost);  // never a loop
      if (root_lost) {
        // Stale root information must age out first. It gains one hop of message age per
        // relay, so a second root loss while the first is still draining can take about two
        // max-age periods (20 s each). This is RSTP's known count-to-infinity bound.
        net.run_for(60000ms);
        expect_spanning_tree(net, seed);
      }
    }
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
}

}  // namespace
}  // namespace softswitch::stp
