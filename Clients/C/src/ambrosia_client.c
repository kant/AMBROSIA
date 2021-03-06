
// See client.h header for function-level documentation.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <string.h>
/* #include <sys/types.h> */
#include <time.h>
#include <assert.h>
#include <errno.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  // for SIO_LOOPBACK_FAST_PATH: 
  #include <Mstcpip.h> 
  #pragma comment(lib,"ws2_32.lib") //Winsock Library
#else
  #include <sys/socket.h>
  #include <arpa/inet.h> // inet_pton
  #include <netdb.h> // gethostbyname
  #include <sched.h>  // sched_yield
  #include <pthread.h> 
#endif

#include "ambrosia/client.h"
#include "ambrosia/internal/bits.h"

// For network progress thread only:
#include "ambrosia/internal/spsc_rring.h"

// Library-level (private) global variables:
// --------------------------------------------------

// FIXME: looks like we need a hashtable after all...
int g_attached = 0;  // For now, ONE destination.

// Global variables that should be initialized once for the library.
// We can ONLY ever have ONE reliability coordinator.
int g_to_immortal_coord, g_from_immortal_coord;


// An INTERNAL global representing whether the client is terminating
// this AMBROSIA instance/network-endpoint.
int g_amb_client_terminating = 0;

#ifdef IPV4
const char* coordinator_host = "127.0.0.1";
#elif defined IPV6
const char* coordinator_host = "::1";
#else
#error "Preprocessor: Expected IPV4 or IPV6 to be defined."
#endif

#ifdef AMBCLIENT_DEBUG
volatile int64_t amb_debug_lock = 0;
#endif


// Reusable code for interacting with AMBROSIA
// ==============================================================================

// General helper functions
// ------------------------

// This may leak, but we only use it when we're bailing out with an error anyway.
char* amb_get_error_string() {
#ifdef _WIN32
  // TODO: could use FormatMessage here...
  char* err = (char*)malloc(2048);
  sprintf(err, "%d", WSAGetLastError());
  return err;
#else  
  return strerror(errno);
#endif  
}

void amb_sleep_seconds(double n) {
#ifdef _WIN32
  Sleep((int)(n * 1000));
#else
  int64_t nanos = (int64_t)(10e9 * n);
  const struct timespec ts = {0, nanos};
  nanosleep(&ts, NULL);
#endif
}

void print_decimal_bytes(char* ptr, int len) {
  const int limit = 100; // Only print this many:
  int j;
  for (j=0; j < len && j < limit; j++) {
    printf("%02d", (unsigned char)ptr[j]);
    if (j % 2 == 1)  printf(" ");
    else printf(".");
  }
  if (j<len) printf("...");
}


void* write_zigzag_int(void* ptr, int32_t value) {
  char* bytes = (char*)ptr;
  uint32_t zigZagEncoded = (uint32_t)((value << 1) ^ (value >> 31));
  while ((zigZagEncoded & ~0x7F) != 0) {
    *bytes++ = (char)((zigZagEncoded | 0x80) & 0xFF);
    zigZagEncoded >>= 7;
  }
  *bytes++ = (char)zigZagEncoded;
  return bytes;
}

void* read_zigzag_int(void* ptr, int32_t* ret) {
  char* bytes = (char*)ptr;
  uint32_t currentByte = *bytes; bytes++;
  char read = 1;
  uint32_t result = currentByte & 0x7FU;
  int32_t  shift = 7;
  while ((currentByte & 0x80) != 0) {    
    currentByte = *bytes; bytes++;
    read++;
    result |= (currentByte & 0x7FU) << shift;
    shift += 7;
    if (read > 5) return NULL; // Invalid encoding.
  }
  *ret = (int32_t) ((-(result & 1)) ^ ((result >> 1) & 0x7FFFFFFFU));
  return (void*)bytes;
}

int zigzag_int_size(int32_t value) {
  int retVal = 0;
  uint32_t zigZagEncoded = ((value << 1) ^ (value >> 31));
  while ((zigZagEncoded & ~0x7F) != 0) {
      retVal++;
      zigZagEncoded >>= 7;
  }
  return retVal+1;
}


// AMBROSIA-specific messaging utilities
// -------------------------------------

// FIXME - need to match what's in the AMBROSIA code.
int32_t checksum(int32_t initial, char* buf, int n) {
  int32_t acc = initial;
  for(int i = 0; i<n; i++) {
    acc += (int32_t)buf[i];
  }
  return acc;
}

// CONVENTIONS:
//
// "linear cursors" - the functions that write to buffers here take a
//  pointer into the buffer, write a variable amount of data, and
//  return the advanced cursor in the buffer.

// Write-to-memory utilities
// ------------------------------

// FIXME -- all write_* functions need to take a BOUND to avoid buffer
// overflows, *OR* we instead need to create an "infinite" buffer and
// set up a guard page.


void* amb_write_incoming_rpc(void* buf, int32_t methodID, char fireForget, void* args, int argsLen) {
  char* cursor = (char*)buf;
  int methodIDSz = zigzag_int_size(methodID);
  int totalSize = 1/*type*/ + 1/*resrvd*/ + methodIDSz + 1/*fireforget*/ + argsLen; 
  // amb_debug_log(" ... encoding incoming RPC, writing varint size %d for argsLen %d (methodID takes up %d)\n", totalSize, argsLen, methodIDSz);
  cursor = write_zigzag_int(cursor, totalSize); // Size (message header)
  *cursor++ = RPC;                            // Type (message header)
  *cursor++ = 0;                              // Reserved zero byte. 
  cursor = write_zigzag_int(cursor, methodID);  // MethodID
  *cursor++ = 1;                              // Fire and forget = 1
  memcpy(cursor, args, argsLen);              // Arguments packed tightly.
  cursor += argsLen;
  return (void*)cursor;
}

void* amb_write_outgoing_rpc_hdr(void* buf, char* dest, int32_t destLen, char RPC_or_RetVal,
                             int32_t methodID, char fireForget, int argsLen) {
  char* cursor = (char*)buf;
  int totalSize = 1 // type tag
    + zigzag_int_size(destLen) + destLen + 1 // RPC_or_RetVal
    + zigzag_int_size(methodID) + 1 // fireForget
    + argsLen;  
  cursor = write_zigzag_int(cursor, totalSize); // Size (message header)
  *cursor++ = RPC;                            // Type (message header)
  cursor = write_zigzag_int(cursor, destLen);   // Destination string size 
  memcpy(cursor, dest, destLen); cursor += destLen; // Registered name of dest service
  *cursor++ = RPC_or_RetVal;                        // 1 byte 
  cursor = write_zigzag_int(cursor, methodID);        // 1-5 bytes
  *cursor++ = fireForget;                           // 1 byte
  return (void*)cursor;
}

void* amb_write_outgoing_rpc(void* buf, char* dest, int32_t destLen, char RPC_or_RetVal,
                          int32_t methodID, char fireForget, void* args, int argsLen) {
  char* cursor = amb_write_outgoing_rpc_hdr(buf, dest, destLen, RPC_or_RetVal, methodID, fireForget, argsLen);
  memcpy(cursor, args, argsLen);                    // N bytes - Arguments packed tightly.
  cursor += argsLen;
  return (void*)cursor;
}

// Direct socket sends/recvs
// ------------------------------

void amb_send_outgoing_rpc(void* tempbuf, char* dest, int32_t destLen, char RPC_or_RetVal,
                          int32_t methodID, char fireForget, void* args, int argsLen) {
  char* cursor0 = (char*)tempbuf;
  char* cursor = cursor0;
  int totalSize = 1 // type tag
    + zigzag_int_size(destLen) + destLen + 1 // RPC_or_RetVal
    + zigzag_int_size(methodID) + 1 // fireForget
    + argsLen;  
  cursor = write_zigzag_int(cursor, totalSize); // Size (message header)
  *cursor++ = RPC;                            // Type (message header)
  cursor = write_zigzag_int(cursor, destLen);   // Destination string size 
  memcpy(cursor, dest, destLen); cursor += destLen; // Registered name of dest service
  *cursor++ = RPC_or_RetVal;                        // 1 byte 
  cursor = write_zigzag_int(cursor, methodID);        // 1-5 bytes
  *cursor++ = fireForget;                           // 1 byte

  // This version makes even *more* syscalls, but it doesn't copy:
  amb_socket_send_all(g_to_immortal_coord, tempbuf, cursor-cursor0, 0);
  amb_socket_send_all(g_to_immortal_coord, args, argsLen, 0);  
  return;
}

void amb_recv_log_hdr(int sockfd, struct log_hdr* hdr) {
  // This version uses MSG_WAITALL to read in one go:
  int num = recv(sockfd, (char*)hdr, AMBROSIA_HEADERSIZE, MSG_WAITALL);
  if(num < AMBROSIA_HEADERSIZE) {
    char* err = amb_get_error_string();
    if (num >= 0) {
      fprintf(stderr,"\nERROR: connection interrupted. Did not receive all %d bytes of log header, only %d:\n  ",
             AMBROSIA_HEADERSIZE, num);
      print_hex_bytes(amb_dbg_fd,(char*)hdr, num); fprintf(amb_dbg_fd,"\n");
    }
    fprintf(stderr,"\nERROR: failed recv (logheader), which left errno = %s\n", err);
    abort();
  }
  amb_debug_log("Read log header: { commit %d, sz %d, checksum %lld, seqid %lld }\n",
                hdr->commitID, hdr->totalSize, hdr->checksum, hdr->seqID );
  // printf("Hex: "); print_hex_bytes((char*)hdr,AMBROSIA_HEADERSIZE); printf("\n");  
  return;
}



// ==============================================================================
// Manage the state of the client (networking/connections)
// ==============================================================================

void attach_if_needed(char* dest, int destLen) {
  // HACK: only working for one dest atm...
  if (!g_attached && destLen != 0) // If destName=="" we are sending to OURSELF and don't need attach.
  {
      amb_debug_log("Sending attach message re: dest = %s...\n", dest);
      char sendbuf[128];
      char* cur = sendbuf;
      int dest_len = strlen(dest);
      cur = (char*)write_zigzag_int(cur, dest_len + 1); // Size
      *cur++ = (char)AttachTo;                        // Type
      memcpy(cur, dest, dest_len); cur+=dest_len;
#ifdef AMBCLIENT_DEBUG
      amb_debug_log("  Attach message: ", dest);
      print_hex_bytes(amb_dbg_fd, sendbuf, cur-sendbuf);
      fprintf(amb_dbg_fd,"\n");
#endif
      amb_socket_send_all(g_to_immortal_coord, sendbuf, cur-sendbuf, 0);
      g_attached = 1;
      amb_debug_log("  attach message sent (%d bytes)\n", cur-sendbuf);
  }
}

// Hacky busy-wait by thread-yielding for now:
// FIXME: NEED BACKOFF!
static inline
void amb_yield_thread() {
#ifdef _WIN32
  SwitchToThread();
#else  
  sched_yield();
#endif
}


// Launch a background thread that progresses the network.
#ifdef _WIN32
DWORD WINAPI amb_network_progress_thread( LPVOID lpParam )
#else
void*        amb_network_progress_thread( void* lpParam )
#endif
{
  printf(" *** Network progress thread starting...\n");
  int hot_spin_amount = 1; // 100
  int spin_tries = hot_spin_amount;
  while(1) {
    int numbytes = -1;
    char* ptr = peek_buffer(&numbytes);    
    if (numbytes > 0) {
      amb_debug_log(" network thread: sending slice of %d bytes\n", numbytes);
      amb_socket_send_all(g_to_immortal_coord, ptr, numbytes, 0);
      pop_buffer(numbytes); // Must be at least this many.
      spin_tries = hot_spin_amount;
    } else if ( spin_tries == 0) {
      spin_tries = hot_spin_amount;
      // amb_debug_log(" network thread: yielding to wait...\n");
#ifdef AMBCLIENT_DEBUG      
      amb_sleep_seconds(0.5);
      amb_sleep_seconds(0.05);
#endif
      amb_yield_thread();
    } else spin_tries--;   
  }

  return 0;
}



// Begin amb_connect_sockets:
// --------------------------------------------------
#ifdef _WIN32
void enable_fast_loopback(SOCKET sock) {
  int OptionValue = 1;
  DWORD NumberOfBytesReturned = 0;    
  int status = WSAIoctl(sock, SIO_LOOPBACK_FAST_PATH,
                        &OptionValue,
                        sizeof(OptionValue),
                        NULL,
                        0,
                        &NumberOfBytesReturned,
                        0,
                        0);
    
  if (SOCKET_ERROR == status) {
      DWORD LastError = WSAGetLastError();
      if (WSAEOPNOTSUPP == LastError) {
        printf("WARNING: this platform doesn't support the fast loopback (needs Windows Server >= 2012).\n");
      }
      else {
        fprintf(stderr,        "\nERROR: Loopback Fastpath WSAIoctl failed with code: %d", 
                LastError);
        abort();
      }
  }
}

void amb_connect_sockets(int upport, int downport, int* upptr, int* downptr) {
  WSADATA wsa;
  SOCKET sock;

#ifdef IPV4
  int af_inet = AF_INET;
  //  struct hostent* immortalCoord;
  //  struct sockaddr_in addr;
#else   
  int af_inet = AF_INET6;
  //  struct sockaddr_in6 addr;
#endif
   
  amb_debug_log("Initializing Winsock...\n");
  if (WSAStartup(MAKEWORD(2,2),&wsa) != 0) {
    fprintf(stderr,"\nERROR: Error Code : %d", WSAGetLastError());
    abort();
  }

  amb_debug_log("Creating to-AMBROSIA connection\n");  
  if((sock = socket(af_inet, SOCK_STREAM , 0 )) == INVALID_SOCKET) {
    fprintf(stderr, "ERROR: Could not create socket : %d" , WSAGetLastError());
    abort();
  }

  printf(" *** Configuring socket for Windows fast-loopback (pre-connect).\n");
  enable_fast_loopback(sock);
  
#ifdef IPV4     
  struct sockaddr_in addr;
  addr.sin_addr.s_addr = inet_addr(coordinator_host);
  addr.sin_family = AF_INET;
  addr.sin_port = htons( upport );
  
  if (connect(sock, (struct sockaddr *)&addr , sizeof(addr)) < 0) {
    fprintf(stderr, "\nERROR: Failed to connect to-socket: %s:%d\n", coordinator_host, upport); 
    abort();
  }
#else
  struct sockaddr_in6 addr;  
  inet_pton(AF_INET6, coordinator_host, &addr.sin6_addr);
  addr.sin6_family = af_inet;  
  addr.sin6_port = htons(upport);
  
  if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    fprintf(stderr, "\nERROR: Failed to connect to-socket (ipv6): %s:%d\n Error: %s",
            coordinator_host, upport, amb_get_error_string()); 
    abort();
  }
  /*  
    DWORD ipv6only = 0;
    if (SOCKET_ERROR == setsockopt(sock, IPPROTO_IPV6,
                                   IPV6_V6ONLY, (char*)&ipv6only, sizeof(ipv6only) )) {
      fprintf(stderr, "\nERROR: Failed to setsockopt.\n"); 
      closesocket(sock);
      abort();
    }
    // Output parameters:
    SOCKADDR_STORAGE LocalAddr = {0};
    SOCKADDR_STORAGE RemoteAddr = {0};
    DWORD dwLocalAddr = sizeof(LocalAddr);
    DWORD dwRemoteAddr = sizeof(RemoteAddr);
    char upportstr[16];
    sprintf(upportstr, "%d", upport);
    if (! WSAConnectByName(sock,
                           host, 
                           upportstr,
                           &dwLocalAddr,
                          (SOCKADDR*)&LocalAddr,
                          &dwRemoteAddr,
                          (SOCKADDR*)&RemoteAddr,
                          NULL,
                          NULL) ) {
      fprintf(stderr, "\nERROR: Failed to connect (IPV6) to-socket: %s:%d\n Error: %s\n",
              host, upport, amb_get_error_string());
      abort();
    }
  */
#endif
  // enable_fast_loopback(sock); // TEMP HACK  
  *upptr = sock;
  
  // Down link from the coordinator (recv channel)
  // --------------------------------------------------
  amb_debug_log("Creating from-AMBROSIA connection\n");
  SOCKET tempsock;
  if ((tempsock = socket(af_inet, SOCK_STREAM, 0)) == INVALID_SOCKET) {
    fprintf(stderr, "\nERROR: Failed to create (recv) socket: %d\n", WSAGetLastError());
    abort();
  }
#ifdef IPV4  
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons( downport );

  printf(" *** Enable fast-loopback EARLY (pre-bind):\n");
  enable_fast_loopback(tempsock); // TEMP HACK:
  
  if( bind(tempsock, (struct sockaddr *)&addr , sizeof(addr)) == SOCKET_ERROR) {
    fprintf(stderr,"\nERROR: bind returned error, addr:port is %s:%d\n Error was: %d\n",
            coordinator_host, downport, WSAGetLastError());
    abort();
  }

  // enable_fast_loopback(tempsock); // TEMP HACK:
  
  if ( listen(tempsock,5) == SOCKET_ERROR) {
    fprintf(stderr, "ERROR: listen() failed with error: %d\n", WSAGetLastError() );
    closesocket(tempsock);
    WSACleanup();
    abort();
  }
  struct sockaddr_in clientaddr;
  int addrlen = sizeof(struct sockaddr_in);

  // enable_fast_loopback(tempsock); // Apply to the socket before accepting requests.
  
  SOCKET new_socket = accept(tempsock, (struct sockaddr *)&clientaddr, &addrlen);
  if (new_socket == INVALID_SOCKET) {
    fprintf(stderr, "ERROR: accept failed with error code : %d" , WSAGetLastError());
    abort();
  }

  // enable_fast_loopback(new_socket); // TEMP HACK:
  
#else
  // struct sockaddr_in6 addr;
  addr.sin6_family       = af_inet;
  addr.sin6_addr         = in6addr_any;
  addr.sin6_port         = htons(downport);

  if ( bind(tempsock, (SOCKADDR *) &addr, sizeof(SOCKADDR)) == SOCKET_ERROR)
  // if ( bind(tempsock, &addr, sizeof(sockaddr_in6)) == SOCKET_ERROR)
  {
    fprintf(stderr,"\nERROR: bind() failed with error when connecting to addr:port %s:%d: %s\n",
            coordinator_host, downport, amb_get_error_string() );
    closesocket(tempsock);
    WSACleanup();
    abort();
  }
  if ( listen(tempsock, 5) == SOCKET_ERROR) {
    fprintf(stderr, "ERROR: listen() failed with error: %s\n", amb_get_error_string() );
    closesocket(tempsock);
    WSACleanup();
    abort();
  }
  SOCKET new_socket = WSAAccept(tempsock, NULL, NULL, NULL, (DWORD_PTR)NULL);
#endif
  amb_debug_log("Connection accepted from reliability coordinator\n");
  *downptr = new_socket;
  return;
}

#else
// Non-windows version:
// ------------------------------------------------------------

// Establish both connections with the reliability coordinator.
// Takes two output parameters where it will write the resulting sockets.
void amb_connect_sockets(int upport, int downport, int* upptr, int* downptr) {
#ifdef IPV4
  struct hostent* immortalCoord;
  struct sockaddr_in addr;
  int af_inet = AF_INET;
#else   
  struct sockaddr_in6 addr;
  int af_inet = AF_INET6;
#endif
  
  // Link up to the coordinator (send channel)
  // --------------------------------------------------
  memset((char*) &addr, 0, sizeof(addr));
  amb_debug_log("Creating to-AMBROSIA connection\n");  
  if ((*upptr = socket(af_inet, SOCK_STREAM, 0)) < 0) {
    fprintf(stderr, "\nERROR: Failed to create (send) socket.\n");
    abort();
  }
#ifdef IPV4  
  immortalCoord = gethostbyname(coordinator_host);
  if (immortalCoord == NULL) {
    amb_debug_log("\nERROR: could not resolve host: %s\n", coordinator_host);
    abort();
  }
  addr.sin_family = af_inet;  
  memcpy( (char*)&addr.sin_addr.s_addr,
          (char*)(immortalCoord->h_addr_list[0]),
          immortalCoord->h_length );
  //  inet_pton(AF_INET, coordinator_host, &addr.sin_addr);  
  addr.sin_port = htons(upport);
#else
  inet_pton(AF_INET6, coordinator_host, &addr.sin6_addr);
  addr.sin6_family = af_inet;  
  addr.sin6_port = htons(upport);
#endif

  if (connect(*upptr, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    fprintf(stderr, "\nERROR: Failed to connect to-socket: %s:%d\n", coordinator_host, upport); 
    abort();
  }

  // Down link from the coordinator (recv channel)
  // --------------------------------------------------
  amb_debug_log("Creating from-AMBROSIA connection\n");
  int tempfd;
  if ((tempfd = socket(af_inet, SOCK_STREAM, 0)) < 0) {
    fprintf(stderr, "\nERROR: Failed to create (recv) socket.\n");
    abort();
  }
  memset((char*) &addr, 0, sizeof(addr));
#ifdef IPV4
  addr.sin_family       = af_inet;
  addr.sin_addr.s_addr  = INADDR_ANY;    
  addr.sin_port         = htons(downport);
#else
  addr.sin6_family       = af_inet;
  addr.sin6_addr         = in6addr_any;
  addr.sin6_port         = htons(downport);
#endif
  if (bind(tempfd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    fprintf(stderr,"\nERROR: bind returned error, addr:port is %s:%d\n ERRNO was: %s\n",
            coordinator_host, downport, strerror(errno));
    abort();
  }

  if ( listen(tempfd,5) ) {
    fprintf(stderr,"\nERROR: listen returned error, addr:port is %s:%d\n ERRNO was: %s\n",
            coordinator_host, downport, strerror(errno));
    abort();
  }
#ifdef IPV4  
  struct sockaddr_in clientaddr;
#else
  struct sockaddr_in6 clientaddr;
#endif

  socklen_t addrlen = 0;
  if ((*downptr = accept(tempfd, (struct sockaddr*) &clientaddr, &addrlen)) < 0) {
    fprintf(stderr, "failed to accept connection, accept returned: %d", *downptr);
    abort();
  }
  return;
}
#endif
// End amb_connect_sockets


// (Runtime library) Startup.
//------------------------------------------------------------------------------

// Execute the startup messaging protocol.
void amb_startup_protocol(int upfd, int downfd) {
  struct log_hdr hdr; memset((void*) &hdr, 0, AMBROSIA_HEADERSIZE);
  assert(sizeof(struct log_hdr) == AMBROSIA_HEADERSIZE);

  amb_recv_log_hdr(downfd, &hdr);
  int payloadSz = hdr.totalSize - AMBROSIA_HEADERSIZE;
  char* buf = (char*)malloc(payloadSz);
  memset(buf, 0, payloadSz);

  amb_debug_log("  Log header received, now waiting on payload (%d bytes)...\n", payloadSz);
  if(recv(downfd, buf, payloadSz, MSG_WAITALL) < payloadSz) {
    fprintf(stderr,"\nERROR: connection interrupted. Did not receive all %d bytes of payload following header.",
            payloadSz);
    abort();
  }

#ifdef AMBCLIENT_DEBUG
  amb_debug_log("  Read %d byte payload following header: ", payloadSz);
  print_hex_bytes(amb_dbg_fd, buf, payloadSz); fprintf(amb_dbg_fd,"\n");
#endif

  int32_t msgsz = -1;
  char* buf2 = read_zigzag_int(buf, &msgsz);
  if (buf2 == NULL) {
    fprintf(stderr,"\nERROR: failed to parse zig-zag int for log record size.\n");
    abort();
  }
  char msgType = *buf2;
  amb_debug_log("  Read log record size: %d\n", msgsz);
  amb_debug_log("  Read message type: %d\n", msgType);

  switch(msgType) {
  case TakeBecomingPrimaryCheckpoint:
    amb_debug_log("Starting up for the first time (TakeBecomingPrimaryCheckpoint)\n");
    break;

  case Checkpoint:
    fprintf(stderr, "RECOVER mode ... not implemented yet.\n");
    
    abort();
    break;
  default:
    fprintf(stderr, "Protocol violation, did not expect this initial message type from server: %d", msgType);
    abort();
    break;
  }
  
  // int32_t c1 = checksum(0,(char*)&hdr, AMBROSIA_HEADERSIZE);
  int32_t c2 = checksum(0,buf,payloadSz);
  amb_debug_log("  (FINISHME) Per-byte checksum just of the payload bytes: %d\n", c2);

  // Now we write our initial message.
  char msgbuf[1024];
  char argsbuf[1024];  
  memset(msgbuf, 0, sizeof(msgbuf));
  memset(argsbuf, 0, sizeof(argsbuf));
  memset(buf, 0, sizeof(buf));  

  // Temp variables:
  int32_t msgsize;
  char *msgbufcur, *bufcur;

  

// FIXME!! Factor this out into the client application:
#define STARTUP_ID 32
  
  // Send InitialMessage
  // ----------------------------------------
  // Here zigZagEncoding is a disadvantage, because we can't write the
  // size until we have already serialized the message, which implies a copy.  
  // It would be nice to have an encoding that could OPTIONALLY take up 5 bytes, even if
  // its numeric value doesn't mandate it.
  argsbuf[0] = 5;
  argsbuf[1] = 4;
  argsbuf[2] = 3;
  msgbufcur = amb_write_incoming_rpc(msgbuf, STARTUP_ID, 1, argsbuf, 3);
  msgsize   = msgbufcur - msgbuf;  
  // msgsize = sprintf(msgbuf, "hi");

  // Here the "+ 1" accounts for the type byte as well as the message
  // itself (data payload):
  bufcur    = write_zigzag_int(buf, msgsize + 1); // Size (w/type)
  *bufcur++ = InitialMessage;                   // Type
  memcpy(bufcur, msgbuf, msgsize);              // Lame copy!

  int totalbytes = msgsize + (bufcur-buf);
  amb_debug_log("  Now will send InitialMessage to ImmortalCoordinator, %lld total bytes, %d in payload.\n",
         (int64_t)totalbytes, msgsize);
#ifdef AMBCLIENT_DEBUG
  amb_debug_log("  Message: ");
  print_hex_bytes(amb_dbg_fd, buf, msgsize + (bufcur-buf));
  fprintf(amb_dbg_fd,"\n");
#endif
  amb_socket_send_all(upfd, buf, totalbytes, 0);
  /* for(int i=0; i<totalbytes; i++) {
    printf("Sending byte[%d] = %x when you press enter...", i, buf[i]);
    getc(stdin);
    amb_socket_send_all(upfd, buf+i, 1, 0);
    } */ 
  
  // Send Checkpoint message
  // ----------------------------------------
  send_dummy_checkpoint(upfd);

  return;
}

void amb_initialize_client_runtime(int upport, int downport, int bufSz)
{
  int upfd, downfd;
  amb_connect_sockets(upport, downport, &upfd, &downfd);
  amb_debug_log("Connections established (%d,%d), beginning protocol.\n", upfd, downfd);
  amb_startup_protocol(upfd, downfd);

  // Initialize global state that other API entrypoints use:
  g_to_immortal_coord   = upfd;
  g_from_immortal_coord = downfd;

  // Set a default:
  if (bufSz <= 0) bufSz = 20 * 1024 * 1024;

  // Initialize the SPSC ring 
  new_buffer(bufSz);

#ifdef _WIN32
  DWORD lpThreadId;
  HANDLE th = CreateThread(NULL, 0,
                           amb_network_progress_thread,
                           NULL, 0,
                           & lpThreadId);
  if (th == NULL)
#else
  pthread_t th;
  int res = pthread_create(& th, NULL, amb_network_progress_thread, NULL);
  if (res != 0)
#endif
  {
    fprintf(stderr, "ERROR: failed to create network progress thread.\n");
    abort();
  }
}

void amb_shutdown_client_runtime()
{
  g_amb_client_terminating = 1;
}


// Application loop (FIXME: Move into the client library!)
//------------------------------------------------------------------------------

// Handle the serialized RPC after the (Size,MsgType) have been read
// off.
// 
// ARGUMENT len: The length argument is an exact bound on the bytes
// read by this function for this message, which is used in turn to
// compute the byte size of the arguments at the tail of the payload.
char* amb_handle_rpc(char* buf, int len) {
  if (len < 0) {
    fprintf(stderr, "ERROR: amb_handle_rpc, received negative length!: %d", len);
    abort();
  }
  char* bufstart = buf;
  char rpc_or_ret = *buf++;             // 1 Reserved byte.
  int32_t methodID;
  buf = read_zigzag_int(buf, &methodID);  // 1-5 bytes
  char fire_forget = *buf++;            // 1 byte
  int argsLen = len - (buf-bufstart);   // Everything left
  if (argsLen < 0) {
    fprintf(stderr, "ERROR: amb_handle_rpc, read past the end of the buffer: start %p, len %d", buf, len);
    abort();
  }
  amb_debug_log("  Dispatching method %d (rpc/ret %d, fireforget %d) with %d bytes of args...\n",
                methodID, rpc_or_ret, fire_forget, argsLen);
  amb_dispatch_method(methodID, buf, argsLen);
  return (buf+argsLen);
}

void amb_normal_processing_loop()
{
  int upfd   = g_to_immortal_coord;
  int downfd = g_from_immortal_coord;
  
  amb_debug_log("\n        .... Normal processing underway ....\n");
  struct log_hdr hdr;
  memset((void*) &hdr, 0, AMBROSIA_HEADERSIZE);

  int round = 0;
  while (!g_amb_client_terminating) {
    amb_debug_log("Normal processing (iter %d): receive next log header..\n", round++);
    amb_recv_log_hdr(downfd, &hdr);

    int payloadsize = hdr.totalSize - AMBROSIA_HEADERSIZE;
    char* buf = calloc(payloadsize, 1);
    recv(downfd, buf, payloadsize, MSG_WAITALL);
#ifdef AMBCLIENT_DEBUG  
    amb_debug_log("Entire Message Payload (%d bytes): ", payloadsize);
    print_hex_bytes(amb_dbg_fd,buf, payloadsize); fprintf(amb_dbg_fd,"\n");
#endif

    // Read a stream of messages from the log record:
    int rawsize = 0;
    char* bufcur = buf;
    char* limit = buf + payloadsize;
    int ind = 0;
    while (bufcur < limit) {
      amb_debug_log(" Processing message %d in log record, starting at offset %d (%p), remaining bytes %d\n",
                    ind++, bufcur-buf, bufcur, limit-bufcur);
      bufcur = read_zigzag_int(bufcur, &rawsize);  // Size
      char tag = *bufcur++;                      // Type
      rawsize--; // Discount type byte.
      switch(tag) {

      case RPC:
        amb_debug_log(" It's an incoming RPC.. size without len/tag bytes: %d\n", rawsize);
        // print_hex_bytes(bufcur,rawsize);printf("\n");
        bufcur = amb_handle_rpc(bufcur, rawsize);
        break;

      case InitialMessage:
        amb_debug_log(" Received InitialMessage back from server.  Processing..\n");
        // FIXME: InitialMessage should be an arbitrary blob...
        // but here we're following the convention that it's an actual message.
        break;

      case RPCBatch:
        { int32_t numMsgs = -1;
          bufcur = read_zigzag_int(bufcur, &numMsgs);
          amb_debug_log(" Receiving RPC batch of %d messages.\n", numMsgs);
          char* batchstart = bufcur;
          for (int i=0; i < numMsgs; i++) {
            amb_debug_log(" Reading off message %d/%d of batch, current offset %d, bytes left: %d.\n",
                          i+1, numMsgs, bufcur-batchstart, rawsize);
            char* lastbufcur = bufcur;
            int32_t msgsize = -100;
            bufcur = read_zigzag_int(bufcur, &msgsize);  // Size (unneeded)            
            char type = *bufcur++;                     // Type - IGNORED
            amb_debug_log(" --> Read message, type %d, payload size %d\n", type, msgsize-1);
            bufcur = amb_handle_rpc(bufcur, msgsize-1);
            amb_debug_log(" --> handling that message read %d bytes off the batch\n", (int)(bufcur - lastbufcur));
            rawsize -= (bufcur - lastbufcur);
          }
        }
        break;

      case TakeCheckpoint:
        send_dummy_checkpoint(upfd);
        break;
      default:
        fprintf(stderr, "ERROR: unexpected or unrecognized message type: %d", tag);
        abort();
        break;
      }
    }
  }
  amb_debug_log("Client signaled shutdown, normal_processing_loop exiting cleanly...\n");
  return;
}


