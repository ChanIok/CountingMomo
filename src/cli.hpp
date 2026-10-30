#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

struct Options {
  std::string command{"scan"};
  std::string category;
  std::wstring exe_name{L"X6Game-Win64-Shipping.exe"};
  HWND hwnd{};
  std::filesystem::path layout;
  std::filesystem::path font;
  std::filesystem::path names;
  std::filesystem::path categories;
  std::filesystem::path output;
  std::filesystem::path image;
  std::optional<int> row_phase;
  bool validate_characters{};
  bool navigation_only{};
  bool generate_report{};
  int expected_count{};
  int wheel_notches{};
  int overlap_rows{6};
  int max_scrolls{1000};
  double frame_timeout{8.0};
  double stable_seconds{0.25};
  std::optional<std::array<int, 2>> scroll_point;
};
namespace cli {
auto parse(int argc, wchar_t** argv) -> std::expected<Options, std::string>;
auto print_help() -> void;
auto log(std::string_view message) -> void;
}  // namespace cli
