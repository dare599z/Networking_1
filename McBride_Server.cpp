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

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>

struct connection_info 
{
  int port;
  bufferevent *bev;
  event* timeout_event;
};

std::string file_extension(const std::string& filename)
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

inline bool file_exists (const std::string& name) {
  struct stat buffer;   
  return (stat (name.c_str(), &buffer) == 0); 
}

bool splitHeaders(const std::string &s, std::pair<std::string, std::string>& pair)
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

std::string MakeSuccessHeader(
  const std::string& mime_type,
  const size_t length,
  const std::map<std::string, std::string> other_attrs
  )
{
  std::ostringstream ss;
  ss << 
        "HTTP/1.1 200 OK\r\n" <<
        "Content-Type: " << mime_type << "\r\n" <<
        "Content-Length: " << length << "\r\n"
  ;

  for (std::map<std::string, std::string>::const_iterator it = other_attrs.begin(); it != other_attrs.end(); ++it)
  {
    ss << it->first << ": " << it->second << "\r\n";
  }
  ss << "\r\n"; // have this last to separate the header from content
  return ss.str();
}

class ServerConf {
public:
  ServerConf() :
    m_indices()
  {

  }
  bool ParseConfFile(const std::string& filename)
  {
    std::ifstream file;
    file.open(filename);
    if ( file.is_open() )
    {
      std::string line;
      // std::cout << "Config file:" << std::endl;
      while ( getline(file, line) )
      {

        if (line.at(0) == '#') continue;
        // std::cout << line << std::endl;
        std::istringstream ss(line);

        std::string first;
        if ( !( ss >> first ) ) {
          std::cerr << "  \t***Error reading line..." << std::endl;
        }

        if ( first.compare("Listen") == 0 )
        {
          if ( !(ss >> m_port) ) {
            std::cerr << "\tError.. Need Listen <int>" << std::endl;
            return false;
          }
          std::cout << "\tUsing port " << m_port << std::endl;
        }
        else if ( first.compare("DocumentRoot") == 0 )
        {
          if ( !(ss >> m_root) ) {
            std::cerr << "\tError.. Need DocumentRoot <string>" << std::endl;
            return false;
          }
          // sanitize the dir of quotes
          m_root.erase(std::remove(m_root.begin(), m_root.end(), '\"'), m_root.end());
          std::cout << "\tUsing root " << m_root << std::endl;
        }
        else if ( first.compare("DirectoryIndex") == 0 )
        {
          std::string index;
          if ( !(ss >> index) ) {
            std::cerr << "\tError.. Need DirectoryIndex <string> [<string ...]" << std::endl;
            return false;
          }
          m_indices.push_back(index);
          while ( ss >> index ) {
            m_indices.push_back(index);
          }

          std::cout << "\tUsing directory indices:" << std::endl;
          for (std::vector<std::string>::iterator it = m_indices.begin(); it != m_indices.end(); ++it)
          {
            std::cout << "\t\t" << *it << std::endl;
          }
          // std::cout << "\tUsing root " << m_root << std::endl;
        }
        else if ( first.at(0) == '.' )
        {
          std::string ct;
          if ( !(ss>> ct) )
          {
            std::cerr << "\tError.. Need \".extension Content-Type\"" << std::endl;
            continue;
          }
          m_file_types[first] = ct;
        }
      }

      for (std::map<std::string, std::string>::iterator it = m_file_types.begin(); it != m_file_types.end(); ++it)
      {
        std::cout << "Allowing file type " << it->first << "(" << it->second << ")" << std::endl;
      }
    }
    else
    {
      std::cerr << "Error opening file.. : " << filename << std::endl;
      return false;
    }
    return true;
  }

  int port() const {
    return m_port;
  }

  std::vector<std::string> indicies() const {
    return m_indices;
  }

  std::string file_root() const {
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

  void uri_set(const std::string& uri)
  {
    m_uri = uri;
    m_full_uri = m_root + m_uri;
  }

  const std::string& uri() const {
    return m_uri;
  }

  const std::string& full_uri() const {
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

const static std::string e500 = "HTTP/1.1 500 Internal Server Error: cannot allocate memory\n";
const static std::string e501 = "HTTP/1.1 501 Not Implemented: ";
const static std::string e400 = "HTTP/1.1 400 Bad Request: ";
const static std::string e404 = "HTTP/1.1 404 Not Found: ";
const static timeval tenSeconds = {10, 0};

void callback_data_written(bufferevent *bev, void *context)
{
  connection_info *ci = reinterpret_cast<connection_info*>(context);
  std::cout << "[" << ci->port << "]: Closing (WRITEOUT)" << std::endl;
  bufferevent_free(bev);
}

static void callback_event(bufferevent *event, short events, void *context)
{
  connection_info* ci = reinterpret_cast<connection_info*>(context);

  if ( events & BEV_EVENT_ERROR )
  {
    std::cout << "[" << ci->port << "]: Error in the bufferevent" << std::endl;
    // error frmo bufferevent
  }
  if ( events & ( BEV_EVENT_EOF | BEV_EVENT_ERROR ) )
  {
    std::cout << "[" << ci->port << "]: Closing (EOF)" << std::endl;
    bufferevent_free(event);
  }
}

static void callback_timeout(evutil_socket_t fd, short what, void* context)
{
  connection_info* ci = reinterpret_cast<connection_info*>(context);
  std::cout << "[" << ci->port << "]: Closing (TIMEOUT)" << std::endl;
  bufferevent_free( ci->bev );
}

void callback_read(bufferevent *ev, void *context)
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
    bufferevent_lock(ev);
    char *line = evbuffer_readln(input, &n, EVBUFFER_EOL_CRLF);
    bufferevent_unlock(ev);
    if ( !line ) {
      if ( i == 1 ) {
        std::cout << "Opened connection but no input... CLOSING.." << std::endl;
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
        std::cerr << "Unable to parse HTTP Method." << std::endl
                  << "\t" << line << std::endl;
        goto request_error;
      }
      // std::cout << "parsed method = " << req.method << std::endl;

      // Parse out the uri
      std::string uri;
      if ( !(ss>>uri) ) {
        std::cerr << "Unable to parse HTTP URI." << std::endl
                  << "\t" << line << std::endl;
        goto request_error;
      }
      req.uri_set(uri);
      // std::cout << "parsed uri = " << req.uri() << std::endl;
      // std::cout << "parsed full_uri = " << req.full_uri() << std::endl;

      // Parse out the http version
      if ( !(ss>>req.http_version) ) {
        std::cerr << "Unable to parse HTTP Version." << std::endl
                  << "\t" << line << std::endl;
        goto request_error;
      }
      // std::cout << "parsed ver = " << req.http_version << std::endl;

      if ( req.method.compare(METHOD_GET) == 0 ) {
        // try and open the file requested
        if ( req.uri().compare(URI_ROOT) == 0 )
        {
          // std::cout << "Client requested root.." << std::endl;
          std::vector<std::string> indicies = sc.indicies();
          bool rootFound = false;
          for (std::vector<std::string>::iterator it = indicies.begin(); it != indicies.end(); ++it)
          {
            std::string f(sc.file_root());
            f += *it;
            // std::cout << "Checking if root file exists... " << f << std::endl;
            if ( file_exists(f) ) {
              // std::cout << "\ttrue" << std::endl;
              req.uri_set(*it);
              rootFound = true;
              break;
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
          // std::cout << "Checking if file exists... " << req.full_uri();
          bool f = file_exists(req.full_uri());
          // std::cout << ( f ? "\t[true]" : "\t[false]" ) << std::endl;
          if ( !f ) {
            error_string = req.uri();
            goto no_file;
          }
          
        }
      }
      else {
        std::cout << "Client requested method " << req.method << std::endl;
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
        // std::cout << "\tkey=" << pair.first << std::endl << "\tvalue=" << pair.second << std::endl;
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
      std::cerr << "<<ERROR>> Couldn't open file " << req.full_uri() << std::endl;
      return;
    }
    struct stat fd_stat;
    fstat(fd, &fd_stat); // get the file size that we're sending to the buffer

    std::string extension = file_extension(req.uri());
    // std::cout << "<REQUESTED EXTENSION>: " << extension << std::endl;
    std::map<std::string, std::string>::iterator it = sc.m_file_types.find(extension);
    if ( it == sc.m_file_types.end() )
    {
      // file type not allowed by config file
      error_string = req.uri();
      goto bad_file_type;
    }
    std::string header = MakeSuccessHeader(it->second, fd_stat.st_size, req.other_attrs);

    // std::cout << "Sending file...";
    evbuffer_lock(output);
    evbuffer_add(output, header.c_str(), header.length() );
    result = evbuffer_add_file(output, fd, 0, fd_stat.st_size);
    evbuffer_unlock(output);
    // std::cout << "done, result = " << result << std::endl;
    if (req.other_attrs["Connection"].compare("keep-alive") == 0)
    {
      event_add(ci->timeout_event, &tenSeconds);
    }
    else
    {
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

    std::cerr << "<<ERROR>> Something went wrong... shouldn't have accepted this non-GET parameter.." << std::endl;
    bufferevent_free(ev); // just close the connection
  } // non-GET method

bad_file_type:
  {
    std::string es;
    es += e501;
    es += error_string;
    es += "\n";
    bufferevent_lock(ev);
    result = bufferevent_write( ev, es.c_str(), es.length() );
    bufferevent_unlock(ev);
    bufferevent_setcb(ev, callback_read, callback_data_written, callback_event, (void*)ci);
    return;
  }

no_file:
  {
    std::string es;
    es += e404;
    es += error_string;
    es += "\n";
    bufferevent_lock(ev);
    result = bufferevent_write( ev, es.c_str(), es.length() );
    bufferevent_unlock(ev);
    bufferevent_setcb(ev, callback_read, callback_data_written, callback_event, (void*)ci);
    return;
  }
  
request_error:
  bufferevent_lock(ev);
  result = bufferevent_write( ev, e500.c_str(), e500.length() );
  bufferevent_unlock(ev);
  bufferevent_setcb(ev, callback_read, callback_data_written, callback_event, (void*)ci);
  return;

bad_method:
  {
    std::string es;
    es += e400;
    es += error_string;
    es += "\n";
    bufferevent_lock(ev);
    result = bufferevent_write( ev, es.c_str(), es.length() );
    bufferevent_unlock(ev);
    bufferevent_setcb(ev, callback_read, callback_data_written, callback_event, (void*)ci);
    return;
  }
}

static void callback_accept_connection(
  evconnlistener *listener,
  evutil_socket_t fd,
  sockaddr *address,
  int socklen,
  void *context
  )
{
  event_base *base = evconnlistener_get_base(listener);
  bufferevent *bev = bufferevent_socket_new(base,
                                            fd,
                                            BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);


  std::cout << "[" << fd << "]: Opened" << std::endl;

  connection_info* ci = new connection_info();
  event *e = event_new(base, -1, EV_TIMEOUT, callback_timeout, (void*)ci);

  ci->port = fd;
  ci->bev = bev;
  ci->timeout_event = e;

  bufferevent_setcb(bev, callback_read, NULL, callback_event, (void*)ci);
  bufferevent_setwatermark(bev, EV_WRITE, 0, 0);
  bufferevent_enable(bev, EV_READ|EV_WRITE);
}

static void
callback_accept_error(struct evconnlistener *listener, void *ctx)
{
  struct event_base *base = evconnlistener_get_base(listener);
  int err = EVUTIL_SOCKET_ERROR();
  fprintf(stderr, "Got an error %d (%s) on the listener. "
          "Shutting down.\n", err, evutil_socket_error_to_string(err));

  event_base_loopexit(base, NULL);
}

int main(int argc, char** argv)
{
  // ServerConf sc;
  if ( !sc.ParseConfFile("./ws.conf") ) {
    std::cerr << "Errors while parsing the configuration file... Exiting" << std::endl;
    return -1;
  }

  event_base *base = event_base_new();
  if ( !base )
  {
    std::cerr << "Error creating an event loop.. Exiting" << std::endl;
    return -2;
  }

  sockaddr_in sin; 
  memset(&sin, 0, sizeof sin);

  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = 0; // local host
  sin.sin_port = htons(sc.port());

  evconnlistener *listener = evconnlistener_new_bind(
                                     base,
                                     callback_accept_connection,
                                     NULL,
                                     LEV_OPT_CLOSE_ON_FREE|LEV_OPT_REUSEABLE,
                                     -1,
                                     (sockaddr*)&sin,
                                     sizeof sin
                                     );

  if ( !listener )
  {
    std::cerr << "Error creating a TCP socket listener.. Exiting" << std::endl;
    return -3;
  }

  evconnlistener_set_error_cb(listener, callback_accept_error);
  event_base_dispatch(base);

  return 0;
}

