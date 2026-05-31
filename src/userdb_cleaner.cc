// userdb_cleaner.cc — Linux-only full-GC userdb cleaner
//
// 通过 UserDictionaryComponent 的 Db 池复用已打开的 Db 句柄，
// 不做二次 Open，无 LevelDB 锁冲突。
//
// 触发：输入 trigger_input（默认 "/clean"）
//
// 做的事：
//   1. 全量扫描 LevelDB（共享 Db 句柄）→ Erase 超时条目
//   2. 清理 sync .userdb.txt
//   3. 删多平台残留 sync 子目录

#include <rime/common.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/deployer.h>
#include <rime/dict/db.h>
#include <rime/dict/user_db.h>
#include <rime/dict/user_dictionary.h>
#include <rime_api.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "lib/detached_thread_manager.hpp"
#include "userdb_cleaner.hpp"

namespace fs = std::filesystem;

namespace {

constexpr const char* kTxtSuffix = ".userdb.txt";
constexpr size_t kTxtSuffixLen = 11;

int parse_field(const std::string& line, const char* key) {
  auto pos = line.find(key);
  if (pos == std::string::npos) return -1;
  pos += strlen(key);
  auto end = line.find_first_of(" \t\r\n", pos);
  try {
    return std::stoi(end == std::string::npos ? line.substr(pos)
                                              : line.substr(pos, end - pos));
  } catch (...) { return -1; }
}

bool db_in_list(const std::string& name,
                const std::unordered_set<std::string>& filter) {
  return filter.empty() || filter.count(name) > 0;
}

fs::path get_sync_dir() {
  char buf[1024] = {0};
  rime_get_api()->get_sync_dir_s(buf, sizeof(buf));
  fs::path p(buf);
  if (fs::exists(p) && fs::is_directory(p)) return p;

  char ud[1024] = {0};
  rime_get_api()->get_user_data_dir_s(ud, sizeof(ud));
  fs::path up(ud);
  auto inst = up / "installation.yaml";
  if (fs::exists(inst)) {
    rime::Config cfg;
    if (cfg.LoadFromFile(inst)) {
      std::string sd;
      if (cfg.GetString("sync_dir", &sd)) { p = sd; }
    }
  }
  if (fs::exists(p)) return p;
  return up / "sync";
}

bool has_suffix(const std::string& s, const char* sfx, size_t len) {
  return s.size() > len && s.compare(s.size() - len, len, sfx) == 0;
}

std::string db_name_from_txt(const std::string& fname) {
  return fname.substr(0, fname.size() - kTxtSuffixLen);
}

std::string extract_text(const std::string& line) {
  auto t1 = line.find('\t');
  if (t1 == std::string::npos) return line;
  auto t2 = line.find('\t', t1 + 1);
  if (t2 == std::string::npos) return line.substr(t1 + 1);
  return line.substr(t1 + 1, t2 - t1 - 1);
}

void notify(const std::string& body) {
  std::string cmd =
      "notify-send '词典清理' '" + body + "' -t 5000 2>/dev/null";
  if (std::system(cmd.c_str()) != 0)
    LOG(INFO) << "cleaner: " << body;
}

}  // namespace

namespace rime {

UserdbCleaner::UserdbCleaner(const Ticket& ticket) : Processor(ticket) {
  LoadConfig();
}

void UserdbCleaner::LoadConfig() {
  if (!engine_) return;
  auto* s = engine_->schema();
  if (!s) return;
  auto* c = s->config();
  if (!c) return;

  c->GetString("userdb_cleaner/trigger_input", &trigger_input_);
  c->GetInt("userdb_cleaner/delete_threshold", &delete_threshold_);

  if (auto list = c->GetList("userdb_cleaner/db_list")) {
    db_list_.clear();
    for (size_t i = 0; i < list->size(); ++i) {
      if (auto item = list->GetValueAt(i)) {
        std::string n;
        if (item->GetString(&n)) db_list_.push_back(n);
      }
    }
  }
}

ProcessResult UserdbCleaner::ProcessKeyEvent(const KeyEvent& key_event) {
  if (engine_->context()->input() != trigger_input_) return kNoop;
  engine_->context()->Clear();

  DetachedThreadManager mgr;
  if (!mgr.try_start([this]() { ExecuteCleanup(); })) {
    LOG(WARNING) << "cleaner: already running";
  }
  return kAccepted;
}

void UserdbCleaner::ExecuteCleanup() {
  if (delete_threshold_ <= 0) return;
  int total_erased = 0;
  int txt_filtered = 0;
  std::vector<std::string> words;

  std::unordered_set<std::string> filter(db_list_.begin(), db_list_.end());

  // ===== 1. 全量扫描 LevelDB（共享 Db 句柄，不加锁）=====
  auto* user_dict_comp = UserDictionary::Require("user_dictionary");
  if (!user_dict_comp) {
    LOG(ERROR) << "cleaner: cannot get user_dictionary component";
    notify("无法获取 user_dictionary 组件");
    return;
  }

  char user_dir[1024] = {0};
  rime_get_api()->get_user_data_dir_s(user_dir, sizeof(user_dir));
  fs::path user_path(user_dir);

  for (const auto& entry : fs::directory_iterator(user_path)) {
    if (!entry.is_directory()) continue;
    std::string fname = entry.path().filename().string();
    if (!has_suffix(fname, ".userdb", 7)) continue;

    std::string db_name = fname.substr(0, fname.size() - 7);
    if (!db_in_list(db_name, filter)) continue;

    // 通过 Db 池拿共享句柄（强转拿两参数版 Create）
    auto* udc = dynamic_cast<UserDictionaryComponent*>(user_dict_comp);
    if (!udc) continue;
    std::unique_ptr<UserDictionary> ud(udc->Create(db_name, "userdb"));
    if (!ud) continue;
    ud->Load();  // DB 已开则 no-op

    Db* db = ud->db();
    if (!db || !db->loaded()) continue;

    TickCount present_tick = ud->tick();
    if (present_tick == 0) continue;

    auto accessor = db->Query("");
    if (!accessor || accessor->exhausted()) continue;

    int erased = 0;
    std::string key, value;
    while (accessor->GetNextRecord(&key, &value)) {
      if (key.empty() || key[0] == '/' || key[0] == ' ') continue;

      UserDbValue v;
      if (!v.Unpack(value)) continue;

      bool drop = (v.commits < 0) ||  // 僵尸
                  (v.commits >= 0 && static_cast<int>(present_tick - v.tick) >
                                         delete_threshold_);  // 超时(含c=0)
      if (drop) {
        auto tab = key.find('\t');
        if (tab != std::string::npos)
          words.push_back(key.substr(tab + 1));
        db->Erase(key);
        erased++;
      }
    }
    total_erased += erased;
    if (erased > 0)
      LOG(INFO) << "cleaner: " << db_name << " erased " << erased;
  }

  // ===== 2. 清理 sync 目录 =====
  fs::path sync_dir = get_sync_dir();
  std::string user_id = Service::instance().deployer().user_id;

  // 2a. 删非当前用户子目录
  if (fs::exists(sync_dir)) {
    for (const auto& e : fs::directory_iterator(sync_dir)) {
      if (!e.is_directory()) continue;
      if (e.path().filename().string() != user_id) {
        std::error_code ec;
        fs::remove_all(e.path(), ec);
      }
    }
  }

  // 2b. 过滤当前用户的 .userdb.txt
  fs::path user_sync = sync_dir / user_id;
  if (fs::exists(user_sync)) {
    for (const auto& e : fs::directory_iterator(user_sync)) {
      if (!e.is_regular_file()) continue;
      std::string fname = e.path().filename().string();
      if (!has_suffix(fname, kTxtSuffix, kTxtSuffixLen)) continue;

      std::string db_name = db_name_from_txt(fname);
      if (!db_in_list(db_name, filter)) continue;

      fs::path fp = e.path();
      std::ifstream in(fp);
      if (!in.is_open()) continue;

      std::vector<std::string> lines;
      std::string line;
      int max_tick = 0;
      while (std::getline(in, line)) {
        lines.push_back(line);
        if (line.find("c=") != std::string::npos) {
          int t = parse_field(line, " t=");
          if (t > max_tick) max_tick = t;
        }
      }
      in.close();

      std::ofstream out(fp, std::ios::trunc);
      int dropped = 0;
      for (const auto& l : lines) {
        if (l.empty() || l.find("c=") == std::string::npos) {
          out << l << "\n";
          continue;
        }
        int c = parse_field(l, "c=");
        int t = parse_field(l, " t=");
        bool drop = (c < 0) || (c >= 0 && t >= 0 &&
                                (max_tick - t) > delete_threshold_);
        if (drop) {
          words.push_back(extract_text(l));
          dropped++;
        } else {
          out << l << "\n";
        }
      }
      txt_filtered += dropped;
      if (dropped > 0)
        LOG(INFO) << "cleaner: " << fname << " filtered " << dropped;
    }
  }

  // ===== 3. 通知 =====
  int total = total_erased + txt_filtered;
  std::ostringstream msg;
  if (total == 0) {
    msg << "没有需要清理的词条";
  } else {
    msg << "LevelDB 删除: " << total_erased << " 条\n"
        << "sync 文件过滤: " << txt_filtered << " 条";
    if (!words.empty()) {
      msg << "\n词条: ";
      int show = std::min((int)words.size(), 15);
      for (int i = 0; i < show; ++i) {
        if (i > 0) msg << ", ";
        msg << words[i];
      }
      if ((int)words.size() > show) msg << " +" << (words.size() - show);
    }
  }
  LOG(INFO) << "cleaner: done. " << msg.str();
  notify(msg.str());
}

}  // namespace rime
