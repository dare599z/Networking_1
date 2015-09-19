#include <vector>
#include <string>
#include <map>
#include <thread>
#include "easylogging++.h"

typedef std::map<std::string, std::string> file_map;

class HTTP_Server {
public:
  HTTP_Server();

  bool ParseConfFile(const std::string& filename);

  int port() const;
  std::vector<std::string> index_pages() const;

  std::string file_root() const;
  const std::string get_mime(const std::string& ext) const;

  bool extAllowed(const std::string& ext) const;


private:
  int m_port;
  std::string m_root;
  std::vector<std::string> m_index_pages;
  file_map m_file_types;

  mutable std::mutex m_mutex_file_types;
};
