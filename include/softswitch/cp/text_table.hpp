#pragma once

/// @file
/// Column-aligned text tables for `show` output.

#include <string>
#include <vector>

namespace softswitch::cp {

class TextTable {
 public:
  explicit TextTable(std::vector<std::string> headers) : headers_(std::move(headers)) {}
  void add_row(std::vector<std::string> row) { rows_.push_back(std::move(row)); }
  [[nodiscard]] bool empty() const noexcept { return rows_.empty(); }
  /// Header, a dashed rule, then the rows; columns padded to their widest cell.
  [[nodiscard]] std::string render() const;

 private:
  std::vector<std::string> headers_;
  std::vector<std::vector<std::string>> rows_;
};

}  // namespace softswitch::cp
