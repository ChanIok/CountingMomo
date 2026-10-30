#include "cli.hpp"

#include "vendor/std.hpp"

#include "utils/file.hpp"
#include "utils/string.hpp"

namespace cli {
auto log(std::string_view message) -> void {
  std::osyncstream(std::cout) << message << '\n' << std::flush;
}
auto print_help() -> void {
  log("CountingMomo · 数数吧大喵\n"
      "  CountingMomo.exe scan --category 发型 [--expected-count 820]\n"
      "  CountingMomo.exe scan-all [--navigation-only]\n"
      "  CountingMomo.exe analyze --image 截图.jpg --category 发型\n"
      "  CountingMomo.exe capture [--hwnd 0x句柄]\n"
      "  CountingMomo.exe list-windows\n\n"
      "扫描前手动打开分类、清除筛选并滚到顶部。F8 / Ctrl+C 停止并保存。\n"
      "全分类扫描前收起饰品、妆容分组并将侧栏滚到顶部；全妆、肤色跳过，共28类。\n"
      "默认仅导出JSON/CSV及运行记录，不生成HTML报告或正常页面图片。\n"
      "  --report             生成HTML报告及页面/裁图证据，用于scan/scan-all/analyze\n"
      "  --navigation-only    scan-all仅切换分类、保存标题，不扫描物品\n"
      "  --categories PATH    scan-all的分类导航JSON及同目录图标\n"
      "  --font PATH           游戏TrueType字体\n"
      "  --names PATH          名称库TXT，一行一个名称\n"
      "  --validate-characters 开启单字低分核对及逐字证据，用于扫描/analyze；默认关闭\n"
      "  --layout PATH         布局JSON\n"
      "  --output PATH         新建结果目录，拒绝覆盖\n"
      "  --image PATH          analyze的原始客户区截图，不缩放、不操作游戏\n"
      "  --row-phase N         analyze的滚动截图行相位（行首y对行距取余）；首屏省略\n"
      "  --exe NAME            游戏进程名（含.exe后缀），默认 X6Game-Win64-Shipping.exe\n"
      "  --hwnd HANDLE         明确指定窗口句柄，优先于 --exe\n"
      "  --wheel-notches N     固定滚轮幅度1～273；省略时小步标定\n"
      "  --overlap-rows N      保留重叠行，默认6，至少2\n"
      "  --scroll-point X Y    布局客户区滚动点，默认自动选择\n"
      "  --frame-timeout SEC   单次事务超时，默认8秒\n"
      "  --stable-seconds SEC  名称与星星稳定时长，默认0.25秒\n"
      "  --max-scrolls N       每分类物品滚动次数上限，默认1000");
}
auto parse(int argc, wchar_t** argv) -> std::expected<Options, std::string> {
  Options o;
  auto root = utils::file::executable_directory();
  o.layout = root / L"resources/layout.json";
  o.font = root / L"resources/fonts/FZYASHJW_Zhong.ttf";
  o.names = root / L"resources/clothes_names.txt";
  o.categories = root / L"resources/categories/categories.json";
  if (argc == 1) {
    o.command = "help";
    return o;
  }
  o.command = utils::string::utf8(argv[1]);
  if (o.command == "--help" || o.command == "-h") o.command = "help";
  if (o.command != "scan" && o.command != "scan-all" && o.command != "analyze" &&
      o.command != "capture" && o.command != "list-windows" && o.command != "help")
    return std::unexpected("未知命令；请运行 --help。");
  auto integer = [](std::wstring_view text, bool allow_zero = false) -> std::optional<int> {
    std::wstring copy(text);
    wchar_t* end{};
    auto value = std::wcstol(copy.c_str(), &end, 10);
    if (end == copy.c_str() || *end || value < (allow_zero ? 0 : 1) || value > 1000000) return {};
    return static_cast<int>(value);
  };
  for (int i = 2; i < argc; ++i) {
    std::wstring_view key(argv[i]);
    if (key == L"--help") {
      o.command = "help";
      return o;
    }
    if (key == L"--validate-characters") {
      o.validate_characters = true;
      continue;
    }
    if (key == L"--navigation-only") {
      o.navigation_only = true;
      continue;
    }
    if (key == L"--report") {
      o.generate_report = true;
      continue;
    }
    if (i + 1 >= argc) return std::unexpected("参数缺少值：" + utils::string::utf8(key));
    std::wstring_view value(argv[++i]);
    if (key == L"--category")
      o.category = utils::string::utf8(value);
    else if (key == L"--font")
      o.font = value;
    else if (key == L"--names")
      o.names = value;
    else if (key == L"--categories")
      o.categories = value;
    else if (key == L"--layout")
      o.layout = value;
    else if (key == L"--output")
      o.output = value;
    else if (key == L"--image")
      o.image = value;
    else if (key == L"--row-phase") {
      auto phase = integer(value, true);
      if (!phase) return std::unexpected("行相位必须是非负整数。");
      o.row_phase = *phase;
    } else if (key == L"--exe")
      o.exe_name = value;
    else if (key == L"--hwnd") {
      std::wstring copy(value);
      wchar_t* end{};
      auto handle = std::wcstoull(copy.c_str(), &end, 0);
      if (!handle || end == copy.c_str() || *end || copy.front() == L'-')
        return std::unexpected("窗口句柄无效。");
      o.hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    } else if (key == L"--frame-timeout" || key == L"--stable-seconds") {
      std::wstring copy(value);
      wchar_t* end{};
      auto number = std::wcstod(copy.c_str(), &end);
      if (end == copy.c_str() || *end || !std::isfinite(number) || number <= 0 || number > 3600)
        return std::unexpected("时长必须是0～3600范围内的有限正数。");
      (key == L"--frame-timeout" ? o.frame_timeout : o.stable_seconds) = number;
    } else if (key == L"--scroll-point") {
      if (i + 1 >= argc) return std::unexpected("--scroll-point 需要 X Y。");
      auto x = integer(value, true), y = integer(argv[++i], true);
      if (!x || !y) return std::unexpected("滚动坐标无效。");
      o.scroll_point = std::array<int, 2>{*x, *y};
    } else {
      auto number = integer(value);
      if (!number) return std::unexpected("参数必须是正整数：" + utils::string::utf8(key));
      if (key == L"--expected-count")
        o.expected_count = *number;
      else if (key == L"--wheel-notches")
        o.wheel_notches = *number;
      else if (key == L"--overlap-rows")
        o.overlap_rows = *number;
      else if (key == L"--max-scrolls")
        o.max_scrolls = *number;
      else
        return std::unexpected("未知参数：" + utils::string::utf8(key));
    }
  }
  if ((o.command == "scan" || o.command == "analyze") && o.category.empty())
    return std::unexpected("扫描及离线分析需要 --category。");
  if (o.command == "analyze" && o.image.empty()) return std::unexpected("离线分析需要 --image。");
  if (o.command != "analyze" && !o.image.empty())
    return std::unexpected("--image 仅用于 analyze。");
  if (o.command != "analyze" && o.row_phase) return std::unexpected("--row-phase 仅用于 analyze。");
  if (o.navigation_only && o.command != "scan-all")
    return std::unexpected("--navigation-only 仅用于 scan-all。");
  if (o.generate_report && o.command != "scan" && o.command != "scan-all" && o.command != "analyze")
    return std::unexpected("--report 仅用于 scan / scan-all / analyze。");
  if (o.command == "scan-all" && (!o.category.empty() || o.expected_count))
    return std::unexpected("scan-all 使用分类清单，不接受 --category 或 --expected-count。");
  if (o.command == "scan-all" && o.frame_timeout <= 0.35)
    return std::unexpected("分类导航超时须大于0.35秒的稳定等待。");
  if (o.validate_characters && o.command != "scan" && o.command != "scan-all" &&
      o.command != "analyze")
    return std::unexpected("--validate-characters 仅用于 scan / scan-all / analyze。");
  if (o.navigation_only && o.validate_characters)
    return std::unexpected("仅导航不识别物品，无需 --validate-characters。");
  if (o.command == "analyze" && o.expected_count)
    return std::unexpected("单张截图不核对分类总数；--expected-count 仅用于 scan。");
  if (!o.hwnd) {
    if (o.exe_name.empty()) return std::unexpected("进程名不能为空。");
    if (o.exe_name.size() < 4 || o.exe_name.substr(o.exe_name.size() - 4) != L".exe")
      return std::unexpected("进程名须以.exe结尾，例如 X6Game-Win64-Shipping.exe。");
  }
  if (o.overlap_rows < 2 || o.wheel_notches > 273 || o.frame_timeout <= o.stable_seconds)
    return std::unexpected("重叠至少2行，滚轮不超过273格，超时须大于稳定时长。");
  return o;
}
}  // namespace cli
