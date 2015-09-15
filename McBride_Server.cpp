#include <event2/listener.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctime>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>

#include "easylogging++.h"
INITIALIZE_EASYLOGGINGPP

/********************************
*
*
* Error codes
*
*
********************************/

const static std::string e500 = "HTTP/1.1 500 Internal Server Error: cannot allocate memory\n";
const static std::string e501 = "HTTP/1.1 501 Not Implemented: ";
const static std::string e400 = "HTTP/1.1 400 Bad Request: ";
const static std::string e404 = "HTTP/1.1 404 Not Found: ";


/********************************
*
*
* Structures
*
*
********************************/

const static timeval tenSeconds = {10, 0}; // ten second timeout

struct connection_info 
{
  int port;
  bufferevent *bev;
  event* timeout_event;

  std::string 
  port_s() const {
    std::ostringstream ss;
    ss << "[" << port << "]: ";
    return ss.str();
  }
};

class ServerConf {
public:
  ServerConf() :
    m_indices()
  {}

  
  bool
  ParseConfFile(const std::string& filename)
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
        m_indices.push_back(index);
        while ( ss >> index ) {
          m_indices.push_back(index);
        }

        VLOG(1) << "Using directory indices:";
        for (std::vector<std::string>::iterator it = m_indices.begin(); it != m_indices.end(); ++it)
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

  int
  port() const {
    return m_port;
  }

  std::vector<std::string>
  indicies() const {
    return m_indices;
  }

  std::string
  file_root() const {
    return m_root;
  }

  std::map<std::string, std::string> m_file_types;

private:
  int m_port;
  std::string m_root;
  std::vector<std::string> m_indices;
} sc;

class http_request {
public:
  http_request(std::string serverRoot) {
    m_root = serverRoot;
  }

  void
  uri_set(const std::string& uri)
  {
    m_uri = uri;
    m_full_uri = m_root + m_uri;
  }

  const std::string&
  uri() const {
    return m_uri;
  }

  const std::string&
  full_uri() const {
    return m_full_uri;
  }

  std::string method;
  std::string http_version;
  std::map<std::string, std::string> other_attrs;

private:
  std::string m_root;
  std::string m_uri;
  std::string m_full_uri;
};

/********************************
*
*
* Helper functions
*
*
********************************/

std::string
file_extension(const std::string& filename)
{
  std::string::size_type idx = filename.rfind('.');

  if (idx != std::string::npos)
  {
    return filename.substr(idx);
  }
  else
  {
    // No extension found
    return "";
  }
}

bool
file_exists(const std::string& name) {
  struct stat buffer;   
  return (stat (name.c_str(), &buffer) == 0); 
}

std::string
getCmdOption(const char ** begin, const char ** end, const std::string & option)
{
  const char ** itr = std::find(begin, end, option);
  if (itr != end && ++itr != end)
  {
      return std::string(*itr);
  }
  return std::string();
}

bool
cmdOptionExists(const char** begin, const char** end, const std::string& option)
{
  return std::find(begin, end, option) != end;
}

bool
splitHeaders(const std::string &s, std::pair<std::string, std::string>& pair)
{
  std::stringstream ss(s);
  std::string item;

  // get the key
  std::getline(ss, item, ':');
  if (item.compare("") == 0) return false; //blank key
  pair.first = item;

  // get the value, and strip the space
  std::getline(ss, item);
  if (item.at(0) == ' ') {
    item.erase(0,1);
  }
  pair.second = item;
  return true;
}

std::string
MakeSuccessHeader(
  const std::string& mime_type,
  const size_t length,
  std::map<std::string, std::string> other_attrs
  )
{
  std::string connection;
  if (other_attrs["Connection"].compare("keep-alive") == 0) {
    connection = "Connection: keep-alive\n";
  }
  else {
    connection = "Connection: close\n";
  }

  time_t rawtime;
  time(&rawtime);
  tm *gm = gmtime(&rawtime);

  char time_buffer[80];
  strftime(time_buffer, 80, "%a, %d %b %Y %H:%M:%S GMT\n", gm);

  std::ostringstream ss;
  ss << 
        "HTTP/1.1 200 OK\n" <<
        connection <<
        "Date: " << time_buffer <<
        "Content-Type: " << mime_type << "\n" <<
        "Content-Length: " << length << "\n"
  ;

  ss << "\n"; // have this last to separate the header from content
  return ss.str();
}

/********************************
*
*
* Callback functions
*
*
********************************/

void
callback_event(bufferevent *event, short events, void *context)
{
  connection_info* ci = reinterpret_cast<connection_info*>(context);

  if ( events & BEV_EVENT_ERROR )
  {
    LOG(ERROR) << ci->port_s() << "Error in the bufferevent";
  }
  if ( events & ( BEV_EVENT_EOF | BEV_EVENT_ERROR ) )
  {
    VLOG(1) << ci->port_s() << "Closing (EOF)";
    bufferevent_free(event);
  }
}

void
callback_timeout(evutil_socket_t fd, short what, void* context)
{
  connection_info* ci = reinterpret_cast<connection_info*>(context);
  VLOG(1) << ci->port_s() << "Closing (TIMEOUT)";
  bufferevent_free( ci->bev );
}

void
callback_data_written(bufferevent *bev, void *context)
{
  connection_info *ci = reinterpret_cast<connection_info*>(context);
  VLOG(1) << ci->port_s() << "Closing (WRITEOUT)";
  bufferevent_free(bev);
}

void
callback_read(bufferevent *ev, void *context)
{
  connection_info* ci = reinterpret_cast<connection_info*>(context);
  // First reset the timer on the connection
  event_del( ci->timeout_event );

  std::string error_string;
  http_request req(sc.file_root());
  evbuffer *input = bufferevent_get_input(ev);
  evbuffer *output = bufferevent_get_output(ev);

  size_t n;
  int result = 0;
  const char* METHOD_GET = "GET";
  const char* URI_ROOT = "/";

  /*
    Enter a loop to construct the request
    and we will process a (valid) request
    after this loop
  */
  for (int i = 1; ; ++i)
  {
    char *line = evbuffer_readln(input, &n, EVBUFFER_EOL_CRLF);
    
    if ( !line ) {
      if ( i == 1 ) {
        LOG(ERROR) << "Opened connection but no input... CLOSING..";
        bufferevent_free(ev);
        return;
      }
      break;
    }
    if ( i == 1 ) {
      // First line of HTTP Request...
      // Should have METHOD URI HTTP_VERSION
      std::istringstream ss(line);

      // Parse out the method
      if ( !(ss>>req.method) ) {
        LOG(WARNING) << ci->port_s() << "Unable to parse HTTP Method.";
        LOG(WARNING) << ci->port_s() << "\t[" << line << "]";
        goto request_error;
      }
      VLOG(2) << ci->port_s() << "req.method= " << req.method;

      // Parse out the uri
      std::string uri;
      if ( !(ss>>uri) ) {
        LOG(WARNING) << ci->port_s() << "Unable to parse HTTP URI.";
        LOG(WARNING) << ci->port_s() << "\t[" << line << "]";
        goto request_error;
      }
      req.uri_set(uri);
      VLOG(2) << ci->port_s() << "req.uri= " << req.uri() << " [" << req.full_uri() << "]";

      // Parse out the http version
      if ( !(ss>>req.http_version) ) {
        LOG(WARNING) << ci->port_s() << "Unable to parse HTTP Version.";
        LOG(WARNING) << ci->port_s() << "\t[" << line << "]";
        goto request_error;
      }
      VLOG(2) << ci->port_s() << "req.http_version= " << req.http_version;
      if (!(req.http_version.compare("HTTP/1.0") == 0) && !(req.http_version.compare("HTTP/1.1") == 0)) {
        error_string = "Invalid HTTP-Version: " + req.http_version;
        goto bad_method;
      }

      if ( req.method.compare(METHOD_GET) == 0 ) {
        // try and open the file requested
        if ( req.uri().compare(URI_ROOT) == 0 )
        {
          VLOG(1) << ci->port_s() << "Client requested the root page";
          std::vector<std::string> indicies = sc.indicies();
          bool rootFound = false;
          for (std::vector<std::string>::iterator it = indicies.begin(); it != indicies.end(); ++it)
          {
            std::string f(sc.file_root());
            f += *it;
            VLOG(2) << ci->port_s() << "Root file exists? [" << f << "]";
            if ( file_exists(f) ) {
              VLOG(2) << ci->port_s() << ".....true";
              req.uri_set(*it);
              rootFound = true;
              break;
            }
            else {
              VLOG(2) << ci->port_s() << ".....false";
            }
          }
          if ( !rootFound )
          {
            error_string = req.uri();
            goto no_file;
          }
        }
        else
        {
          if ( !file_exists(req.full_uri()) ) {
            error_string = req.uri();
            goto no_file;
          }
          
        }
      }
      else {
        error_string = "Invalid Method: " + req.method;
        goto bad_method;
      }
    }
    else {
      /*
      This is any other content sent along with the request,
      such as Connection: keep-alive
      */

      std::pair<std::string, std::string> pair;
      bool b = splitHeaders(line, pair);
      if (b)
      {
        req.other_attrs.insert(pair);
        VLOG(3) << ci->port_s() << "<" << pair.first << "> , <" << pair.second << ">";
      } 
    }
  }

  /*
    Now we need to actually go through and process
    the request..
  */

  if (req.method.compare(METHOD_GET) == 0)
  {
    int fd = open( req.full_uri().c_str(), O_RDONLY );
    if ( fd < 0 ) {
      LOG(ERROR) << ci->port_s() << "Couldn't open file " << req.full_uri();
      goto request_error;
    }
    struct stat fd_stat;
    fstat(fd, &fd_stat); // get the file size that we're sending to the buffer

    std::string extension = file_extension(req.uri());
    VLOG(2) << ci->port_s() << "Requested extension: " << extension;
    std::map<std::string, std::string>::iterator it = sc.m_file_types.find(extension);
    if ( it == sc.m_file_types.end() )
    {
      // file type not allowed by config file
      error_string = req.uri();
      goto bad_file_type;
    }

    bool keepAlive;
    if (req.other_attrs["Connection"].compare("keep-alive") == 0) keepAlive = true;
    else keepAlive = false;
    std::string header = MakeSuccessHeader(it->second, fd_stat.st_size, req.other_attrs);

    evbuffer_add(output, header.c_str(), header.length() );
    result = evbuffer_add_file(output, fd, 0, fd_stat.st_size);
    if (req.other_attrs["Connection"].compare("keep-alive") == 0)
    {
      LOG(INFO) << ci->port_s() << "Serving file: " << req.uri() << " ~ (KEEP-ALIVE)";
      event_add(ci->timeout_event, &tenSeconds);
    }
    else
    {
      LOG(INFO) << ci->port_s() << "Serving file: " << req.uri() << " ~ (CLOSE)";
      bufferevent_setcb(ev, callback_read, callback_data_written, callback_event, (void*)ci);
    }
    return;
  } // GET method
  else
  {
    /*
      at this point, we only implement the GET method
      and the code should never get here, but who knows..
      crazier stuff has happened
    */

    LOG(ERROR) << ci->port_s() << "Something went wrong... shouldn't have accepted this non-GET parameter..";
    bufferevent_free(ev); // just close the connection
  }

bad_file_type:
  {
    std::string es;
    es += e501;
    es += error_string;
    es += "\n";
    
    LOG(WARNING) << ci->port_s() << "<501>: " << error_string;
    bufferevent_write( ev, es.c_str(), es.length() );
    bufferevent_setcb(ev, callback_read, callback_data_written, callback_event, (void*)ci);
    return;
  }

no_file:
  {
    std::string es;
    es += e404;
    es += error_string;
    es += "\n";
    
    LOG(WARNING) << ci->port_s() << "<404>: " << error_string;
    result = bufferevent_write( ev, es.c_str(), es.length() );
    bufferevent_setcb(ev, callback_read, callback_data_written, callback_event, (void*)ci);
    return;
  }
  
request_error:
  result = bufferevent_write( ev, e500.c_str(), e500.length() );
  bufferevent_setcb(ev, callback_read, callback_data_written, callback_event, (void*)ci);
  return;

bad_method:
  {
    std::string es;
    es += e400;
    es += error_string;
    es += "\n";
    LOG(WARNING) << ci->port_s() << "<400>: " << error_string;
    
    result = bufferevent_write( ev, es.c_str(), es.length() );
    bufferevent_setcb(ev, callback_read, callback_data_written, callback_event, (void*)ci);
    return;
  }
}


void 
callback_accept_connection(
  evconnlistener *listener,
  evutil_socket_t newSocket,
  sockaddr *address,
  int socklen,
  void *context
  )
{
  event_base *base = evconnlistener_get_base(listener);
  bufferevent *bev = bufferevent_socket_new(base,
                                            newSocket,
                                            BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);

  connection_info *ci = new connection_info();
  event *e = event_new(base, -1, EV_TIMEOUT, callback_timeout, (void*)ci);

  ci->port = newSocket;
  ci->bev = bev;
  ci->timeout_event = e;

  VLOG(2) << ci->port_s() << "Opened connection.";
  bufferevent_setcb(bev, callback_read, NULL, callback_event, (void*)ci);
  bufferevent_setwatermark(bev, EV_WRITE, 0, 0);
  bufferevent_enable(bev, EV_READ|EV_WRITE);
}

void
callback_accept_error(struct evconnlistener *listener, void *ctx)
{
  struct event_base *base = evconnlistener_get_base(listener);
  int err = EVUTIL_SOCKET_ERROR();
  LOG(FATAL) << "Got error <" << err << ": " << evutil_socket_error_to_string(err) << "> on the connection listener. Shutting down.";

  event_base_loopexit(base, NULL);
}

int
main(const int argc, const char** argv)
{
  // start the easylogging++.h library
  START_EASYLOGGINGPP(argc, argv); 
  el::Configurations conf;
  conf.setToDefault();
  conf.setGlobally(el::ConfigurationType::Format, "<%datetime{%H:%m:%s}><%levshort>: %msg");
  el::Loggers::reconfigureAllLoggers(conf);
  conf.clear();
  el::Loggers::addFlag( el::LoggingFlag::ColoredTerminalOutput );
  el::Loggers::addFlag( el::LoggingFlag::DisableApplicationAbortOnFatalLog );


  std::string confFilePath; // config file can be passed in following "-c" cli option
  if ( cmdOptionExists(argv, argv+argc, "-c") ) confFilePath = getCmdOption( argv, argv+argc, "-c" );
  else confFilePath = "./ws.conf";

  if ( !sc.ParseConfFile(confFilePath) ) {
    LOG(FATAL) << "Errors while parsing the configuration file... Exiting";
    return -1;
  }

  event_base *base = event_base_new();
  if ( !base )
  {
    LOG(FATAL) << "Error creating an event loop.. Exiting";
    return -2;
  }

  sockaddr_in incomingSocket; 
  memset(&incomingSocket, 0, sizeof incomingSocket);

  incomingSocket.sin_family = AF_INET;
  incomingSocket.sin_addr.s_addr = 0; // local host
  incomingSocket.sin_port = htons(sc.port());

  evconnlistener *listener = evconnlistener_new_bind(
                                     base,
                                     callback_accept_connection,
                                     NULL,
                                     LEV_OPT_CLOSE_ON_FREE|LEV_OPT_REUSEABLE,
                                     -1,
                                     (sockaddr*)&incomingSocket,
                                     sizeof incomingSocket
                                     );

  if ( !listener )
  {
    LOG(FATAL) << "Error creating a TCP socket listener.. Exiting.";
    return -3;
  }

  evconnlistener_set_error_cb(listener, callback_accept_error);
  event_base_dispatch(base);

  return 0;
}

