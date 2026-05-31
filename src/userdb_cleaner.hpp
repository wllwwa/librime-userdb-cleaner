#ifndef USERDB_CLEANER_HPP_
#define USERDB_CLEANER_HPP_

#include <rime/common.h>
#include <rime/processor.h>
#include <string>
#include <vector>

namespace rime {

class UserdbCleaner : public Processor {
 public:
  explicit UserdbCleaner(const Ticket& ticket);
  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

 private:
  void LoadConfig();
  void ExecuteCleanup();

  std::string trigger_input_ = "/clean";
  int delete_threshold_ = 0;
  std::vector<std::string> db_list_;  // 空 = 清理所有 userdb
};

}  // namespace rime
#endif
