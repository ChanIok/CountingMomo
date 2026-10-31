#include "features/result/result.hpp"

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"
#include "vendor/windows.hpp"

#include "core/message/message.hpp"
#include "utils/file.hpp"

namespace features::result {

// 序列化扫描结果：将分类与 0/1 拥有数组映射转为格式化 JSON 字符串
auto serialize(const Categories& categories) -> std::expected<std::string, core::Error> {
  try {
    // 调用反射库生成 JSON 文本
    return rfl::json::write(categories);
  } catch (const std::exception& error) {
    // 捕获序列化异常并包装为领域错误
    return std::unexpected(
        core::with_detail(core::error("error.result.serialize_failed"), error.what()));
  }
}

// 原子写入结果文件：序列化 JSON → 写临时文件 → 强制刷新落盘替换原文件
auto write(const std::filesystem::path& path, const Categories& categories)
    -> std::expected<void, core::Error> {
  // 构造同目录的临时交换文件路径，避免写中断损坏已有成果
  auto temporary = path;
  temporary += L".tmp";
  // 将拥有状态映射转换为 JSON 文本
  auto json = serialize(categories);
  if (!json) {
    return std::unexpected(json.error());
  }
  // 先把全量内容写入临时文件
  if (auto ok = utils::file::write(temporary, *json); !ok) {
    return ok;
  }
  // 原子替换目标文件并要求硬件落盘，防止断电或进程强退造成半写
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return std::unexpected(core::error("error.result.replace_failed", GetLastError()));
  }
  return {};
}

}  // namespace features::result
