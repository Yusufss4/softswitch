#include "softswitch/cp/text_table.hpp"

#include <algorithm>

namespace softswitch::cp {

std::string TextTable::render() const {
  std::vector<std::size_t> widths(headers_.size(), 0);
  auto measure = [&](const std::vector<std::string>& row) {
    for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].size());
    }
  };
  measure(headers_);
  for (const auto& row : rows_) {
    measure(row);
  }
  std::string out;
  auto emit = [&](const std::vector<std::string>& row) {
    std::string line;
    for (std::size_t i = 0; i < widths.size(); ++i) {
      const std::string cell = i < row.size() ? row[i] : std::string{};
      line += cell;
      if (i + 1 < widths.size()) {
        line.append(widths[i] - cell.size() + 2, ' ');
      }
    }
    while (!line.empty() && line.back() == ' ') {
      line.pop_back();
    }
    out += line + "\n";
  };
  emit(headers_);
  std::vector<std::string> rule;
  rule.reserve(widths.size());
  for (const auto w : widths) {
    rule.emplace_back(w, '-');
  }
  emit(rule);
  for (const auto& row : rows_) {
    emit(row);
  }
  return out;
}

}  // namespace softswitch::cp
