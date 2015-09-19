#include "class_HTTP_Server.h"
#include <sstream>

HTTP_Server::HTTP_Server():
    m_index_pages(),
    m_file_types()
    // m_mutex_file_types(PTHREAD_MUTEX_INITIALIZER)
{
  pthread_mutex_init(&m_mutex_file_types,NULL);
}

int
HTTP_Server::port() const 
{
  return m_port;
}

std::vector<std::string>
HTTP_Server::index_pages() const
{
  return m_index_pages;
}

std::string
HTTP_Server::file_root() const
{
  return m_root;
}

const std::string
HTTP_Server::get_mime(const std::string& ext) const
{
  std::string mime;
  pthread_mutex_lock(&m_mutex_file_types);
  file_map::const_iterator f_it = m_file_types.find(ext);
  pthread_mutex_unlock(&m_mutex_file_types);
  return f_it->second;
}

bool
HTTP_Server::extAllowed(const std::string& ext) const
{
  pthread_mutex_lock(&m_mutex_file_types);
  file_map::const_iterator f_it = m_file_types.find(ext);
  if (f_it == m_file_types.end())
  {
    pthread_mutex_unlock(&m_mutex_file_types);
    return false;
  }
  else
  {
    pthread_mutex_unlock(&m_mutex_file_types);
    return true;
  }
}

bool  
HTTP_Server::ParseConfFile(const std::string& filename)
{
  std::ifstream file;
  file.open(filename);
  if ( !file.is_open() )
  {
    LOG(FATAL) << "Error opening file.. : " << filename;
    return false;
  }

  std::string line;
  while ( getline(file, line) )
  {

    if (line.at(0) == '#') continue;
    std::istringstream ss(line);

    std::string first;
    if ( !( ss >> first ) ) {
      LOG(ERROR) << "Config file error on line [" << line << "]";
      continue;
    }

    if ( first.compare("Listen") == 0 )
    {
      if ( !(ss >> m_port) ) {
        LOG(FATAL) << "Need Listen <int>";
        return false;
      }
      LOG(INFO) << "Server port: " << m_port;
    }
    else if ( first.compare("DocumentRoot") == 0 )
    {
      if ( !(ss >> m_root) ) {
        LOG(FATAL) << "Need DocumentRoot <string>";
        return false;
      }
      // sanitize the dir of quotes
      m_root.erase(std::remove(m_root.begin(), m_root.end(), '\"'), m_root.end());
      LOG(INFO) << "Document root: " << m_root;
    }
    else if ( first.compare("DirectoryIndex") == 0 )
    {
      std::string index;
      if ( !(ss >> index) ) {
        LOG(FATAL) << "Need DirectoryIndex <string> [<string ...]";
        return false;
      }
      m_index_pages.push_back(index);
      while ( ss >> index ) {
        m_index_pages.push_back(index);
      }

      VLOG(1) << "Using directory indices:";
      for (std::vector<std::string>::iterator it = m_index_pages.begin(); it != m_index_pages.end(); ++it)
      {
        VLOG(1) << "\t" << *it;
      }
    }
    else if ( first.at(0) == '.' )
    {
      std::string ct;
      if ( !(ss>> ct) )
      {
        LOG(ERROR) << "Need \".extension Content-Type\"";
        LOG(ERROR) << "\t\t[" << line << "]";
        continue;
      }
      m_file_types[first] = ct;
    }
  }

  for (std::map<std::string, std::string>::iterator it = m_file_types.begin(); it != m_file_types.end(); ++it)
  {
    VLOG(1) << "Allowed: " << it->first << " (" << it->second << ")";
  }
  
  return true;
}