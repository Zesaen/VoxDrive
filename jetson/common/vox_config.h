// vox_config.h — VoxDrive 统一配置读取（header-only, C++17）
//
// 配置文件为纯文本 key=value：
//   - '#' 开头为注释；首个 '=' 分割键值；值两端空白被裁剪
//   - 值支持 $HOME 前缀展开（板上模型/数据路径统一用 $HOME 表达）
// 查找顺序：显式路径 → 环境变量 VOX_CONF → 可执行文件向上定位
//  （services/<svc>/build/ → ../../../config/voxdrive.conf，即 jetson/config/）→ ./voxdrive.conf
// 用法：main() 启动时（多线程拉起前）调用一次 vox::config::load()，
//   之后各处 vox::config::get("port.tool_bus", "6669") 等读取。
#pragma once

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

namespace vox {
namespace config {

inline std::map<std::string, std::string>& store() {
  static std::map<std::string, std::string> s;
  return s;
}

inline std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

inline std::string expand_home(const std::string& v) {
  const char* home = std::getenv("HOME");
  if (home && v.rfind("$HOME", 0) == 0) return std::string(home) + v.substr(5);
  return v;
}

// 可执行文件所在目录（依赖 /proc/self/exe，仅 Linux——本服务族本就只在板上运行）
inline std::string exe_dir() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = '\0';
  std::string path(buf);
  size_t pos = path.find_last_of('/');
  return pos == std::string::npos ? "." : path.substr(0, pos);
}

inline std::vector<std::string> candidate_paths(const std::string& explicit_path) {
  std::vector<std::string> paths;
  if (!explicit_path.empty()) paths.push_back(explicit_path);
  if (const char* env = std::getenv("VOX_CONF")) paths.push_back(env);
  const std::string d = exe_dir();
  // services/<svc>/build/tool_bus → jetson/config/voxdrive.conf
  paths.push_back(d + "/../../../config/voxdrive.conf");
  paths.push_back(d + "/../../config/voxdrive.conf");
  paths.push_back(d + "/config/voxdrive.conf");
  paths.push_back(d + "/voxdrive.conf");
  paths.push_back("./config/voxdrive.conf");
  paths.push_back("./voxdrive.conf");
  return paths;
}

// 解析并装载；成功返回 true。多次调用以最后一次成功为准（便于测试）。
inline bool load(const std::string& explicit_path = "") {
  for (const std::string& p : candidate_paths(explicit_path)) {
    std::ifstream f(p);
    if (!f.is_open()) continue;
    std::map<std::string, std::string> parsed;
    std::string line;
    while (std::getline(f, line)) {
      size_t hash = line.find('#');
      if (hash != std::string::npos) line = line.substr(0, hash);
      line = trim(line);
      if (line.empty()) continue;
      size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      parsed[trim(line.substr(0, eq))] = expand_home(trim(line.substr(eq + 1)));
    }
    store() = std::move(parsed);
    std::fprintf(stderr, "vox_config: loaded %zu keys from %s\n", store().size(), p.c_str());
    return true;
  }
  std::fprintf(stderr, "vox_config: no voxdrive.conf found (VOX_CONF unset)\n");
  return false;
}

inline bool has(const std::string& key) {
  return store().count(key) > 0;
}

inline std::string get(const std::string& key, const std::string& def = "") {
  auto it = store().find(key);
  return it == store().end() ? def : it->second;
}

inline long get_int(const std::string& key, long def = 0) {
  auto it = store().find(key);
  if (it == store().end()) return def;
  char* end = nullptr;
  long v = std::strtol(it->second.c_str(), &end, 10);
  return (end && *end == '\0') ? v : def;
}

inline double get_double(const std::string& key, double def = 0.0) {
  auto it = store().find(key);
  if (it == store().end()) return def;
  char* end = nullptr;
  double v = std::strtod(it->second.c_str(), &end);
  return (end && *end == '\0') ? v : def;
}

// ZMQ 端点构造：bind 用通配主机（tcp://*:6669），connect 用 localhost
inline std::string bind_endpoint(const std::string& port_key, const std::string& def_port) {
  return "tcp://" + get("bind.host", "*") + ":" + get(port_key, def_port);
}

inline std::string connect_endpoint(const std::string& port_key, const std::string& def_port,
                                    const std::string& host = "localhost") {
  return "tcp://" + host + ":" + get(port_key, def_port);
}

}  // namespace config
}  // namespace vox
