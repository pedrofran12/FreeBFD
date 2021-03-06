#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "bfdInt.h"
#include "bfdLog.h"
#include "tp-timers.h"

typedef struct _bfdSockRec {
  int                 sock;
  uint32_t            refCnt;
  uint16_t            port;
  struct _bfdSockRec *next;
} bfdSockRec;

/* Only the receive sockets are potentially shared */
static bfdSockRec *sRxSocks = NULL;

/* Buffer and msghdr for received packets */
static uint8_t msgbuf[BFD_MINPKTLEN];
static struct iovec msgiov = {
  &(msgbuf[0]),
  sizeof(msgbuf)
};
static uint8_t cmsgbuf[sizeof(struct cmsghdr) + 4];
static struct sockaddr_in msgaddr;
static struct msghdr msghdr = {
  (void *)&msgaddr,
  sizeof(msgaddr),
  &msgiov,
  1,
  (void *)&cmsgbuf,
  sizeof(cmsgbuf),
  0
};

static bfdSockRec* findSock(bfdSessionInt *bfd)
{
  bfdSockRec *sockRec = sRxSocks;

  while (sockRec) {
    if (sockRec->port == bfd->Sn.LocalPort) {
      return sockRec;
    }

    sockRec = sockRec->next;
  }

  return NULL;
}

/*
 * Create and Register socket to receive control messages
 */
static bool setupRxSocket(bfdSessionInt *bfd)
{
  struct sockaddr_in sin;
  int rcvttl = 1;
  int sock;
  bfdSockRec *sockRec;

  if ((sockRec = findSock(bfd)) == NULL) {
    sockRec = calloc(1, sizeof(bfdSockRec));
    if (sockRec == NULL) {
      bfdLog(LOG_ERR, "Unable to allocate socket record: %m\n");
      return false;
    }

    if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
      bfdLog(LOG_WARNING, "[%x] Can't create Rx socket [*:%d]: %m\n",
             bfd->LocalDiscr, bfd->Sn.LocalPort);

      free(sockRec);
      return false;
    }

    if (setsockopt(sock, SOL_IP, IP_RECVTTL, &rcvttl, sizeof(rcvttl)) < 0) {
      bfdLog(LOG_WARNING,
             "[%x] Can't configure Rx socket [*:%d] to receive TTL: %m\n",
             bfd->LocalDiscr, bfd->Sn.LocalPort);

      close(sock);
      free(sockRec);
      return false;
    }

    sin.sin_family      = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port        = htons(bfd->Sn.LocalPort);
    
    if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
      bfdLog(LOG_WARNING,
             "[%x] Can't bind Rx socket to port %d: %m\n",
             bfd->LocalDiscr, bfd->Sn.LocalPort);

      free(sockRec);
      close(sock);
      return false;
    }

    sockRec->sock = sock;
    sockRec->port = bfd->Sn.LocalPort;
    sockRec->refCnt = 1;
    sockRec->next = sRxSocks;
    sRxSocks = sockRec;

    bfd->RxSock = sock;

    /* Add socket to select poll */
    tpSetSktActor(sock, bfdRcvPkt, (void *)&msghdr, NULL);

    bfdLog(LOG_DEBUG, "[%x] Created new Rx socket %d [*:%d]\n",
           bfd->LocalDiscr, sock, bfd->Sn.LocalPort);
  } else {
    bfd->RxSock = sockRec->sock;
    sockRec->refCnt++;

    bfdLog(LOG_DEBUG, "[%x] Reusing Rx socket %d [*:%d]\n",
           bfd->LocalDiscr, sockRec->sock, bfd->Sn.LocalPort);
  }

  return true;
}

static bool setupTxSocket(bfdSessionInt *bfd)
{
  static uint16_t srcPort = BFD_SRCPORTINIT;
  struct sockaddr_in sin;
  int ttlval = BFD_1HOPTTLVALUE;
  int pcount;
  int sock;

  /* Get socket for transmitting control packets */
  if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    bfdLog(LOG_WARNING, "[%x] Can't create socket for %s: %m\n",
           bfd->LocalDiscr, bfd->Sn.SnIdStr);

    return false;
  }

  /* Set TTL to 255 for all transmitted packets */
  if (setsockopt(sock, SOL_IP, IP_TTL, &ttlval, sizeof(ttlval)) < 0) {
    bfdLog(LOG_WARNING, "[%x] Can't set TTL for pkts to %s: %m\n",
           bfd->LocalDiscr, bfd->Sn.SnIdStr);

    close(sock);

    return false;
  }

  /* Find an available source port in the proper range */
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = INADDR_ANY;

  pcount = 0;
  do {
    if ((++pcount) > (BFD_SRCPORTMAX - BFD_SRCPORTINIT)) {
      /* Searched all ports, none available */
      bfdLog(LOG_WARNING, "[%x] Can't find source port for %s\n",
             bfd->LocalDiscr, bfd->Sn.SnIdStr);

      close(sock);

      return false;
    }

    if (srcPort >= BFD_SRCPORTMAX) { srcPort = BFD_SRCPORTINIT; }

    sin.sin_port = htons(srcPort++);
  } while (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0);

  bfd->TxSock = sock;

  bfdLog(LOG_DEBUG, "[%x] Opened socket %d to %s\n",
         bfd->LocalDiscr, sock, bfd->Sn.SnIdStr);

  return true;
}

bool bfdSocketSetup(bfdSessionInt *bfd)
{
  if (!setupTxSocket(bfd)) {
    return false;
  }

  if (!setupRxSocket(bfd)) {
    bfdSocketClose(bfd);
    return false;
  }

  return true;
}

bool bfdSocketClose(bfdSessionInt *bfd)
{
  bfdSockRec *sockRec;

  if (bfd->TxSock > 0) {
    close(bfd->TxSock);

    bfdLog(LOG_DEBUG, "[%x] Closed socket %d to %s\n", bfd->LocalDiscr,
           bfd->TxSock, bfd->Sn.SnIdStr);
  }

  if (bfd->RxSock > 0 && bfd->TxSock != bfd->RxSock) {
    sockRec = findSock(bfd);

    if (sockRec != NULL) {
      sockRec->refCnt--;
      if (sockRec->refCnt <= 0) {
        bfdSockRec *cur = sRxSocks;
        bfdSockRec *prev = NULL;

        while (cur != sockRec) {
          prev = cur;
          cur = cur->next;
        }

        if (prev == NULL) {
          sRxSocks = sRxSocks->next;
        } else {
          prev->next = cur->next;
        }

        close(sockRec->sock);
        tpRmSktActor(sockRec->sock);

        bfdLog(LOG_DEBUG, "[%x] Closed lonely Rx socket %d [*:%d]\n",
               bfd->LocalDiscr, sockRec->sock, bfd->Sn.LocalPort);

        free(sockRec);
      }
    } else {
      close(bfd->RxSock);

      bfdLog(LOG_DEBUG, "[%x] Closed Rx socket %d\n",
             bfd->LocalDiscr, bfd->RxSock);
    }
  }

  bfd->TxSock = -1;
  bfd->RxSock = -1;

  return true;
}
