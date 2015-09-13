`
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>

#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define QLEN      32  /* maximum connection queue length  */
#define BUFSIZE   4096

extern int  errno;
int   errexit(const char *format, ...);
int   passivesock(const char *portnum, int qlen);
int   parseRequest(int fd);

/*------------------------------------------------------------------------
 * main - Concurrent TCP server for ECHO service
 *------------------------------------------------------------------------
 */
int
main(int argc, char *argv[])
{
  char  *portnum = "5004";  /* Standard server port number  */
  struct sockaddr_in fsin;  /* the from address of a client */
  int msock;      /* master server socket   */
  fd_set  rfds;     /* read file descriptor set */
  fd_set  afds;     /* active file descriptor set */
  unsigned int  alen;   /* from-address length    */
  int fd, nfds;
  
  switch (argc) {
  case  1:
    break;
  case  2:
    portnum = argv[1];
    break;
  default:
    errexit("usage: %s [port]\n",argv[0]);
  }

  msock = passivesock(portnum, QLEN);

  nfds = msock+1;
  FD_ZERO(&afds);
  FD_SET(msock, &afds);

  while (1) {
    memcpy(&rfds, &afds, sizeof(rfds));

    if (select(nfds, &rfds, (fd_set *)0, (fd_set *)0,
        (struct timeval *)0) < 0)
      errexit("select: %s\n", strerror(errno));
    if (FD_ISSET(msock, &rfds)) {
      int ssock;

      alen = sizeof(fsin);
      ssock = accept(msock, (struct sockaddr *)&fsin,
        &alen);
      if (ssock < 0)
        errexit("accept: %s\n",
          strerror(errno));
      FD_SET(ssock, &afds);
    }

    char* ok = "HTTP/1.1 200 OK";
    write(fd, ok, strlen(ok));

    for (fd=0; fd<nfds; ++fd)
      if (fd != msock && FD_ISSET(fd, &rfds))
        if (parseRequest(fd) == 0) {
          (void) close(fd);
          FD_CLR(fd, &afds);
        }
  }
}

/*------------------------------------------------------------------------
 * parseRequest - look at the request sent by the client and 
 * respond appropiately
 *------------------------------------------------------------------------
 */
int
parseRequest(int fd)
{
  char  buf[BUFSIZ];
  int cc;

  char method[15];
  char uri[256];
  char version[256];

  const char* METHOD_GET = "GET";
  const char* test_send = "HTTP/1.1 200 OK\r\n"
                          "Date: Tue, 16 Feb 2010 19:21:24 GMT\r\n"
                          "Content-Type: text/html;\r\n\r\n"
                          "<html><head><title>First Web Server!</title>\r\n";

  cc = read(fd, buf, sizeof buf);
  if (cc < 0) errexit("parseRequest read: %s\n", strerror(errno));

  if (cc) {
    // have data

    int readBytes = sscanf(buf, "%s %s %s", method, uri, version);

    if ( read != 3 ) {
      char* bad = "bad";
      write(fd, bad, strlen(bad));
      printf("bad args\n");
    }
    else {
      if ( strcmp( METHOD_GET, method) == 0) {
        printf("----------------------------------------------\n");
        printf("REQUEST FROM CLIENT : \n%s\n", buf);
        printf("----------------------------------------------\n");
        printf("SERVER RESPONSE:\n%s\n", test_send);
        printf("----------------------------------------------\n");
        write(fd, test_send, strlen(test_send));
        close(fd);
        return sizeof test_send;
      }
      else {
        printf( "got METHOD: [%s]\n", method );
        // printf( "buf = %s\n", buf );
      }

    }
  }

  else {
    // nothing useful to do
  }
  return cc;
}

/*------------------------------------------------------------------------
 * errexit - print an error message and exit
 *------------------------------------------------------------------------
 */
int
errexit(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  exit(1);
}

/*------------------------------------------------------------------------
 * passivesock - allocate & bind a server socket using TCP
 *------------------------------------------------------------------------
 */
int
passivesock(const char *portnum, int qlen)
/*
 * Arguments:
 *      portnum   - port number of the server
 *      qlen      - maximum server request queue length
 */
{
  struct sockaddr_in sin; /* an Internet endpoint address  */
  int     s;              /* socket descriptor             */

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET; 
  sin.sin_addr.s_addr = INADDR_ANY;

/* Map port number (char string) to port number (int) */
  if ((sin.sin_port=htons((unsigned short)atoi(portnum))) == 0)
          errexit("can't get \"%s\" port number\n", portnum);

/* Allocate a socket */
  s = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0)
      errexit("can't create socket: %s\n", strerror(errno));

/* Bind the socket */
  if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
      fprintf(stderr, "can't bind to %s port: %s; Trying other port\n",
          portnum, strerror(errno));
      sin.sin_port=htons(0); /* request a port number to be allocated
                             by bind */
      if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0)
          errexit("can't bind: %s\n", strerror(errno));
      else {
          int socklen = sizeof(sin);

          if (getsockname(s, (struct sockaddr *)&sin, &socklen) < 0)
                  errexit("getsockname: %s\n", strerror(errno));
          printf("New server port number is %d\n", ntohs(sin.sin_port));
      }
  }

  if (listen(s, qlen) < 0)
      errexit("can't listen on %s port: %s\n", portnum, strerror(errno));
  return s;
}

