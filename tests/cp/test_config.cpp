#include <gtest/gtest.h>

#include <memory>
#include <sstream>

#include "softswitch/cp/config.hpp"

namespace softswitch::cp {
namespace {

SwitchConfig sample() { return SwitchConfig::with_ports("sw1", {"p1", "p2", "p3", "p4"}); }

CommandResult run(SwitchConfig& c, const std::string& line) {
  return apply_config(c, tokenize(line));
}

TEST(Tokenize, SplitsAndStripsComments) {
  EXPECT_EQ(tokenize("  vlan   10 name users  "),
            (std::vector<std::string>{"vlan", "10", "name", "users"}));
  EXPECT_TRUE(tokenize("! a comment").empty());
  EXPECT_EQ(tokenize("vlan 10 # trailing"), (std::vector<std::string>{"vlan", "10"}));
}

TEST(VlanList, ParseAndFormat) {
  const auto set = parse_vlan_list("10,20,30-33");
  ASSERT_TRUE(set.has_value());
  EXPECT_TRUE(set->test(10));
  EXPECT_TRUE(set->test(31));
  EXPECT_FALSE(set->test(34));
  EXPECT_EQ(set->count(), 6U);
  EXPECT_EQ(format_vlan_list(*set), "10,20,30-33");
  EXPECT_EQ(format_vlan_list(*parse_vlan_list("all")), "all");
  EXPECT_EQ(format_vlan_list(*parse_vlan_list("none")), "none");
  EXPECT_FALSE(parse_vlan_list("0").has_value());
  EXPECT_FALSE(parse_vlan_list("4095").has_value());
  EXPECT_FALSE(parse_vlan_list("20-10").has_value());
  EXPECT_FALSE(parse_vlan_list("10,,20").has_value());
}

TEST(ConfigCommands, VlanLifecycle) {
  auto c = sample();
  EXPECT_TRUE(run(c, "vlan 10 name users").ok);
  EXPECT_EQ(c.vlans.at(10).name, "users");
  EXPECT_TRUE(run(c, "vlan 20").ok);
  EXPECT_EQ(c.vlans.at(20).name, "VLAN20");
  EXPECT_TRUE(run(c, "no vlan 20").ok);
  EXPECT_FALSE(c.vlan_exists(20));
  EXPECT_FALSE(run(c, "no vlan 1").ok);
  EXPECT_FALSE(run(c, "vlan 5000").ok);
}

TEST(ConfigCommands, InterfaceCommands) {
  auto c = sample();
  EXPECT_TRUE(run(c, "interface p1 switchport access vlan 10").ok);
  EXPECT_TRUE(run(c, "interface p3 switchport mode trunk").ok);
  EXPECT_TRUE(run(c, "interface p3 switchport trunk allowed vlan 10,20").ok);
  EXPECT_TRUE(run(c, "interface p3 switchport trunk allowed vlan add 30").ok);
  EXPECT_TRUE(run(c, "interface p3 switchport trunk allowed vlan remove 10").ok);
  EXPECT_TRUE(run(c, "interface p3 switchport trunk native vlan 99").ok);
  EXPECT_TRUE(run(c, "interface p2 description uplink to core").ok);
  EXPECT_TRUE(run(c, "interface p4 shutdown").ok);
  EXPECT_EQ(c.ports[0].access_vlan, 10);
  EXPECT_EQ(c.ports[2].mode, shm::PortMode::kTrunk);
  EXPECT_EQ(format_vlan_list(c.ports[2].trunk_allowed), "20,30");
  EXPECT_EQ(c.ports[2].native_vlan, 99);
  EXPECT_EQ(c.ports[1].description, "uplink to core");
  EXPECT_TRUE(c.ports[3].shutdown);
  EXPECT_TRUE(run(c, "interface p4 no shutdown").ok);
  EXPECT_FALSE(c.ports[3].shutdown);
}

TEST(ConfigCommands, Errors) {
  auto c = sample();
  EXPECT_FALSE(run(c, "interface nope shutdown").ok);
  EXPECT_FALSE(run(c, "interface p1 switchport mode hybrid").ok);
  EXPECT_FALSE(run(c, "interface p1 switchport access vlan 0").ok);
  EXPECT_FALSE(run(c, "frobnicate").ok);
  EXPECT_FALSE(run(c, "mac address-table aging-time 5").ok);
  EXPECT_TRUE(run(c, "mac address-table aging-time 60").ok);
  EXPECT_EQ(c.mac_aging_seconds, 60U);
}

// Regression (found by fuzz_config): a malformed "vlan" command used to create the VLAN with
// an empty name before rejecting the arguments, producing an unreplayable running-config.
TEST(ConfigCommands, RejectedVlanCommandChangesNothing) {
  auto c = sample();
  const auto before = c;
  EXPECT_FALSE(run(c, "vlan 10 name").ok);
  EXPECT_FALSE(run(c, "vlan 10 bogus words").ok);
  EXPECT_EQ(c, before);
}

TEST(ConfigCommands, StaticMacEntries) {
  auto c = sample();
  EXPECT_TRUE(run(c, "mac address-table static 02:00:00:00:00:aa vlan 10 interface p2").ok);
  EXPECT_TRUE(
      run(c, "mac address-table static 02:00:00:00:00:aa vlan 10 interface p3").ok);  // replaces
  ASSERT_EQ(c.static_macs.size(), 1U);
  EXPECT_EQ(c.static_macs[0].port, "p3");
  EXPECT_FALSE(run(c, "mac address-table static 01:00:5e:00:00:01 vlan 10 interface p2").ok);
  EXPECT_FALSE(run(c, "mac address-table static 02:00:00:00:00:aa vlan 10 interface nope").ok);
  EXPECT_TRUE(run(c, "no mac address-table static 02:00:00:00:00:aa vlan 10").ok);
  EXPECT_TRUE(c.static_macs.empty());
}

TEST(ConfigCommands, IsConfigCommand) {
  EXPECT_TRUE(is_config_command(tokenize("vlan 10")));
  EXPECT_TRUE(is_config_command(tokenize("no vlan 10")));
  EXPECT_FALSE(is_config_command(tokenize("show vlan")));
  EXPECT_FALSE(is_config_command({}));
}

TEST(RunningConfig, RoundTripsThroughCommands) {
  auto c = sample();
  for (const char* line :
       {"vlan 10 name users", "vlan 20", "interface p1 switchport access vlan 10",
        "interface p2 description host two", "interface p3 switchport mode trunk",
        "interface p3 switchport trunk allowed vlan 10,20",
        "interface p3 switchport trunk native vlan 20", "interface p4 shutdown",
        "mac address-table aging-time 120", "trace sample 10"}) {
    ASSERT_TRUE(run(c, line).ok) << line;
  }
  const std::string rendered = render_running_config(c);
  auto replay = sample();
  std::istringstream in(rendered);
  std::string line;
  while (std::getline(in, line)) {
    ASSERT_TRUE(apply_config(replay, tokenize(line)).ok) << line;
  }
  EXPECT_EQ(replay, c) << rendered;
}

class L2ConfigTest : public ::testing::Test {
 protected:
  std::unique_ptr<shm::Tables> t_ = std::make_unique<shm::Tables>();
};

TEST_F(L2ConfigTest, AccessAndTrunkMembership) {
  auto c = sample();
  for (const char* line :
       {"vlan 10", "vlan 20", "interface p1 switchport access vlan 10",
        "interface p2 switchport access vlan 20", "interface p3 switchport mode trunk",
        "interface p3 switchport trunk allowed vlan 10,20"}) {
    ASSERT_TRUE(run(c, line).ok);
  }
  apply_l2_config(c, *t_);
  EXPECT_EQ(t_->num_ports, 4U);
  EXPECT_EQ(t_->ports[0].pvid, 10);
  EXPECT_EQ(t_->ports[1].pvid, 20);
  EXPECT_EQ(t_->ports[2].mode, shm::PortMode::kTrunk);
  EXPECT_EQ(t_->ports[2].pvid, 0);  // native VLAN 1 not allowed -> untagged frames dropped
  EXPECT_EQ(t_->vlans[10].members, 0b0101U);
  EXPECT_EQ(t_->vlans[10].untagged, 0b0001U);
  EXPECT_EQ(t_->vlans[20].members, 0b0110U);
  EXPECT_EQ(t_->vlans[20].untagged, 0b0010U);
  EXPECT_EQ(t_->vlans[1].members, 0b1000U);  // p4 left in VLAN 1
  EXPECT_EQ(std::string(t_->ports[0].name.data()), "p1");
  EXPECT_NE(t_->ports[0].flags & shm::port_flag::kAdminUp, 0);
}

TEST_F(L2ConfigTest, AccessPortInMissingVlanIsInactive) {
  auto c = sample();
  ASSERT_TRUE(run(c, "interface p1 switchport access vlan 77").ok);
  apply_l2_config(c, *t_);
  EXPECT_EQ(t_->ports[0].pvid, 0);
  EXPECT_EQ(t_->vlans[77].members, 0U);
}

TEST_F(L2ConfigTest, TrunkNativeVlanIsUntagged) {
  auto c = sample();
  ASSERT_TRUE(run(c, "vlan 10").ok);
  ASSERT_TRUE(run(c, "interface p3 switchport mode trunk").ok);
  apply_l2_config(c, *t_);
  EXPECT_EQ(t_->ports[2].pvid, 1);
  EXPECT_TRUE(has_port(t_->vlans[1].untagged, 2));
  EXPECT_TRUE(has_port(t_->vlans[10].members, 2));
  EXPECT_FALSE(has_port(t_->vlans[10].untagged, 2));
}

TEST_F(L2ConfigTest, ShutdownClearsAdminUp) {
  auto c = sample();
  ASSERT_TRUE(run(c, "interface p2 shutdown").ok);
  t_->ports[1].flags = shm::port_flag::kAdminUp | shm::port_flag::kOperUp;
  apply_l2_config(c, *t_);
  EXPECT_EQ(t_->ports[1].flags & shm::port_flag::kAdminUp, 0);
  EXPECT_NE(t_->ports[1].flags & shm::port_flag::kOperUp, 0);  // oper state untouched
}

}  // namespace
}  // namespace softswitch::cp
