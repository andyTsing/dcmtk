/*
 *
 *  Copyright (C) 1998-2018, OFFIS e.V.
 *  All rights reserved.  See COPYRIGHT file for details.
 *
 *  This software and supporting documentation were developed by
 *
 *    OFFIS e.V.
 *    R&D Division Health
 *    Escherweg 2
 *    D-26121 Oldenburg, Germany
 *
 *
 *  Module: dcmnet
 *
 *  Author: Marco Eichelberg
 *
 *  Purpose:
 *    classes: DcmTransportConnection, DcmTCPConnection
 *
 */

#include "dcmtk/config/osconfig.h"    /* make sure OS specific configuration is included first */

#ifdef HAVE_WINDOWS_H
// on Windows, we need Winsock2 for network functions
#include <winsock2.h>
#endif

#include "dcmtk/dcmnet/dcmtrans.h"
#include "dcmtk/dcmnet/dcompat.h"     /* compatibility code for certain Unix dialects such as SunOS */
#include "dcmtk/dcmnet/diutil.h"

#define INCLUDE_CSTDLIB
#define INCLUDE_CSTDIO
#define INCLUDE_CSTRING
#define INCLUDE_CTIME
#define INCLUDE_CERRNO
#define INCLUDE_CSIGNAL
#include "dcmtk/ofstd/ofstdinc.h"
#include "dcmtk/ofstd/oftimer.h"

BEGIN_EXTERN_C
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
END_EXTERN_C

#ifdef DCMTK_HAVE_POLL
#include <poll.h>
#endif

/* platform independent definition of EINTR */
enum
{
#ifdef HAVE_WINSOCK_H
    DCMNET_EINTR = WSAEINTR
#else
    DCMNET_EINTR = EINTR
#endif
};

OFGlobal<Sint32> dcmSocketSendTimeout(60);
OFGlobal<Sint32> dcmSocketReceiveTimeout(60);

DcmTransportConnection::DcmTransportConnection(DcmNativeSocketType openSocket)
: theSocket(openSocket)
{
  if (theSocket >= 0)
  {
#ifdef DISABLE_SEND_TIMEOUT
#warning The macro DISABLE_SEND_TIMEOUT is not supported anymore. See "macros.txt" for details.
#endif
    /* get global timeout for the send() function */
    const Sint32 sendTimeout = dcmSocketSendTimeout.get();
    if (sendTimeout >= 0)
    {
      if (sendTimeout == 0)
        DCMNET_DEBUG("setting network send timeout to 0 (infinite)");
      else
        DCMNET_DEBUG("setting network send timeout to " << sendTimeout << " seconds");
#ifdef HAVE_WINSOCK_H
      // for Windows, specify send timeout in milliseconds
      int timeoutVal = sendTimeout * 1000;
      if (setsockopt(theSocket, SOL_SOCKET, SO_SNDTIMEO, (char *) &timeoutVal, sizeof(timeoutVal)) < 0)
#else
      // for other systems, specify send timeout as timeval struct
      struct timeval timeoutVal;
      timeoutVal.tv_sec = sendTimeout;
      timeoutVal.tv_usec = 0;
      if (setsockopt(theSocket, SOL_SOCKET, SO_SNDTIMEO, &timeoutVal, sizeof(timeoutVal)) < 0)
#endif
      {
        // according to MSDN: available in the Microsoft implementation of Windows Sockets 2,
        // so we are reporting a warning message but are not returning with an error code;
        // this also applies to all other systems where the call to this function might fail
        DCMNET_WARN("cannot set network send timeout to " << sendTimeout << " seconds");
      }
    }
#ifdef DISABLE_RECV_TIMEOUT
#warning The macro DISABLE_RECV_TIMEOUT is not supported anymore. See "macros.txt" for details.
#endif
    /* get global timeout for the recv() function */
    const Sint32 recvTimeout = dcmSocketReceiveTimeout.get();
    if (recvTimeout >= 0)
    {
      if (recvTimeout == 0)
        DCMNET_DEBUG("setting network receive timeout to 0 (infinite)");
      else
        DCMNET_DEBUG("setting network receive timeout to " << recvTimeout << " seconds");
#ifdef HAVE_WINSOCK_H
      // for Windows, specify receive timeout in milliseconds
      int timeoutVal = recvTimeout * 1000;
      if (setsockopt(theSocket, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeoutVal, sizeof(timeoutVal)) < 0)
#else
      // for other systems, specify receive timeout as timeval struct
      struct timeval timeoutVal;
      timeoutVal.tv_sec = recvTimeout;
      timeoutVal.tv_usec = 0;
      if (setsockopt(theSocket, SOL_SOCKET, SO_RCVTIMEO, &timeoutVal, sizeof(timeoutVal)) < 0)
#endif
      {
        // according to MSDN: available in the Microsoft implementation of Windows Sockets 2,
        // so we are reporting a warning message but are not returning with an error code;
        // this also applies to all other systems where the call to this function might fail
        DCMNET_WARN("cannot set network receive timeout to " << recvTimeout << " seconds");
      }
    }
  }
}

DcmTransportConnection::~DcmTransportConnection()
{
}

OFBool DcmTransportConnection::safeSelectReadableAssociation(DcmTransportConnection *connections[], int connCount, int timeout)
{
  int numberOfRounds = timeout+1;
  if (numberOfRounds < 0) numberOfRounds = 0xFFFF; /* a long time */

  OFBool found = OFFalse;
  OFBool firstRound = OFTrue;
  int timeToWait=0;
  int i=0;
  while ((numberOfRounds > 0)&&(! found))
  {
    if (firstRound)
    {
      timeToWait = 0;
      firstRound = OFFalse;
    }
    else timeToWait = 1;
    for (i=0; i<connCount; i++)
    {
      if (connections[i])
      {
        if (connections[i]->networkDataAvailable(timeToWait))
        {
          i = connCount; /* break out of for loop */
          found = OFTrue; /* break out of while loop */
        }
        timeToWait = 0;
      }
    } /* for */
    if (timeToWait == 1) return OFFalse; /* all entries NULL */
    numberOfRounds--;
  } /* while */

  /* number of rounds == 0 (timeout over), do final check */
  found = OFFalse;
  for (i=0; i<connCount; i++)
  {
    if (connections[i])
    {
      if (connections[i]->networkDataAvailable(0)) found = OFTrue; else connections[i]=NULL;
    }
  }
  return found;
}

OFBool DcmTransportConnection::fastSelectReadableAssociation(DcmTransportConnection *connections[], int connCount, int timeout)
{

#ifdef _WIN32
  SOCKET socketfd = INVALID_SOCKET;
  SOCKET maxsocketfd = INVALID_SOCKET;
#else
  int socketfd = -1;
  int maxsocketfd = -1;
#endif

  int i=0;
  struct timeval t;
#ifndef DCMTK_HAVE_POLL
  fd_set fdset;
  FD_ZERO(&fdset);
#endif
  OFTimer timer;
  int lTimeout = timeout;

  for (i=0; i<connCount; i++)
  {
    if (connections[i])
    {
      socketfd = connections[i]->getSocket();
#ifndef DCMTK_HAVE_POLL
#ifdef __MINGW32__
      /* on MinGW, FD_SET expects an unsigned first argument */
      FD_SET((unsigned int)socketfd, &fdset);
#else
      FD_SET(socketfd, &fdset);
#endif /* __MINGW32__ */
#endif /* DCMTK_HAVE_POLL */
      if (socketfd > maxsocketfd) maxsocketfd = socketfd;
    }
  }

#ifdef DCMTK_HAVE_POLL
  struct pollfd pfd[] = {
    { maxsocketfd, POLLIN, 0 }
  };
#endif

  OFBool done = OFFalse;
  while (!done)
  {
    // timeval can be undefined after the call to select, see
    // http://man7.org/linux/man-pages/man2/select.2.html
    t.tv_sec = lTimeout;
    t.tv_usec = 0;

#ifdef DCMTK_HAVE_POLL
    int nfound = poll(pfd, 1, t.tv_sec*1000+(t.tv_usec/1000));
#else /* DCMTK_HAVE_POLL */
#ifdef HAVE_INTP_SELECT
    int nfound = select(OFstatic_cast(int, maxsocketfd + 1), (int *)(&fdset), NULL, NULL, &t);
#else /* HAVE_INTP_SELECT */
    // This is safe because on Win32 the first parameter of select() is ignored anyway
    int nfound = select(OFstatic_cast(int, maxsocketfd + 1), &fdset, NULL, NULL, &t);
#endif /* HAVE_INTP_SELECT */
#endif /* DCMTK_HAVE_POLL */

    if (nfound == 0) return OFFalse; // a regular timeout
    else if (nfound > 0) done = OFTrue; // data available for reading
    else
    {
        // check for interrupt call
        if (OFStandard::getLastNetworkErrorCode().value() == DCMNET_EINTR)
        {
            int diff = OFstatic_cast(int, timer.getDiff());
            if (diff < timeout)
            {
                lTimeout = timeout - diff;
                continue; // retry
            }
        }
        else
        {
            DCMNET_ERROR("socket select returned with error: " << OFStandard::getLastNetworkErrorCode().message());
            return OFFalse;
        }
    }
  }

  for (i=0; i<connCount; i++)
  {
    if (connections[i])
    {
      socketfd = connections[i]->getSocket();
#ifdef DCMTK_HAVE_POLL
      pfd[0].fd = socketfd;
      pfd[0].events = POLLIN;
      pfd[0].revents = 0;
      poll(pfd, 1, 0);
      if(!(pfd[0].revents & POLLIN)) connections[i] = NULL;
#else
      /* if not available, set entry in array to NULL */
      if (!FD_ISSET(socketfd, &fdset)) connections[i] = NULL;
#endif
    }
  }
  return OFTrue;
}

OFBool DcmTransportConnection::selectReadableAssociation(DcmTransportConnection *connections[], int connCount, int timeout)
{
  OFBool canUseFastMode = OFTrue;
  for (int i=0; i<connCount; i++)
  {
    if ((connections[i]) && (! connections[i]->isTransparentConnection())) canUseFastMode = OFFalse;
  }
  if (canUseFastMode) return fastSelectReadableAssociation(connections, connCount, timeout);
  return safeSelectReadableAssociation(connections, connCount, timeout);
}

void DcmTransportConnection::dumpConnectionParameters(STD_NAMESPACE ostream& out)
{
    OFString str;
    out << dumpConnectionParameters(str) << OFendl;
}

/* ================================================ */

DcmTCPConnection::DcmTCPConnection(DcmNativeSocketType openSocket)
: DcmTransportConnection(openSocket)
{
}

DcmTCPConnection::~DcmTCPConnection()
{
  close();
}

DcmTransportLayerStatus DcmTCPConnection::serverSideHandshake()
{
  return TCS_ok;
}

DcmTransportLayerStatus DcmTCPConnection::clientSideHandshake()
{
  return TCS_ok;
}

DcmTransportLayerStatus DcmTCPConnection::renegotiate(const char * /* newSuite */ )
{
  return TCS_ok;
}

ssize_t DcmTCPConnection::read(void *buf, size_t nbyte)
{
#ifdef HAVE_WINSOCK_H
  return recv(getSocket(), (char *)buf, OFstatic_cast(int, nbyte), 0);
#else
  return ::read(getSocket(), (char *)buf, nbyte);
#endif
}

ssize_t DcmTCPConnection::write(void *buf, size_t nbyte)
{
#ifdef HAVE_WINSOCK_H
  return send(getSocket(), (char *)buf, OFstatic_cast(int, nbyte), 0);
#else
  return ::write(getSocket(), (char *)buf, nbyte);
#endif
}

void DcmTCPConnection::close()
{
  if (getSocket() != -1)
  {
#ifdef HAVE_WINSOCK_H
    (void) shutdown(getSocket(), 1 /* SD_SEND */);
    (void) closesocket(getSocket());
#else
    (void) ::close(getSocket());
#endif
  /* forget about this socket (now closed) */
    setSocket(-1);
  }
}

unsigned long DcmTCPConnection::getPeerCertificateLength()
{
  return 0;
}

unsigned long DcmTCPConnection::getPeerCertificate(void * /* buf */ , unsigned long /* bufLen */ )
{
  return 0;
}

OFBool DcmTCPConnection::networkDataAvailable(int timeout)
{
  struct timeval t;
  int nfound;
  OFTimer timer;
  int lTimeout = timeout;

#ifndef DCMTK_HAVE_POLL
  fd_set fdset;
  FD_ZERO(&fdset);

#ifdef __MINGW32__
  /* on MinGW, FD_SET expects an unsigned first argument */
  FD_SET((unsigned int) getSocket(), &fdset);
#else
  FD_SET(getSocket(), &fdset);
#endif /* __MINGW32__ */
#endif /* DCMTK_HAVE_POLL */

  while (1)
  {
      // timeval can be undefined after the call to select, see
      // http://man7.org/linux/man-pages/man2/select.2.html
      t.tv_sec = lTimeout;
      t.tv_usec = 0;

#ifdef DCMTK_HAVE_POLL
  struct pollfd pfd[] = 
  {
    { getSocket(), POLLIN, 0 }
  };
  nfound = poll(pfd, 1, t.tv_sec*1000+(t.tv_usec/1000));
#else
#ifdef HAVE_INTP_SELECT
      nfound = select(OFstatic_cast(int, getSocket() + 1), (int *)(&fdset), NULL, NULL, &t);
#else
      // This is safe because on Win32 the first parameter of select() is ignored anyway
      nfound = select(OFstatic_cast(int, getSocket() + 1), &fdset, NULL, NULL, &t);
#endif /* HAVE_INTP_SELECT */
#endif /* DCMTK_HAVE_POLL */

      if (nfound < 0)
      {
        // check for interrupt call
        if (OFStandard::getLastNetworkErrorCode().value() == DCMNET_EINTR)
        {
            int diff = OFstatic_cast(int, timer.getDiff());
            if (diff < timeout)
            {
                lTimeout = timeout - diff;
                continue; // retry
            }
        }
        else
        {
            DCMNET_ERROR("socket select returned with error: " << OFStandard::getLastNetworkErrorCode().message());
            return OFFalse;
        }
      }
      if (nfound == 0)
      {
        return OFFalse; // a regular timeout
      }
      else
      {
#ifdef DCMTK_HAVE_POLL
        if (pfd[0].revents & POLLIN) return OFTrue;
#else
        if (FD_ISSET(getSocket(), &fdset)) return OFTrue;
#endif
        else return OFFalse;  /* This should not really happen */
      }
  }
  return OFFalse;
}

OFBool DcmTCPConnection::isTransparentConnection()
{
  return OFTrue;
}

OFString& DcmTCPConnection::dumpConnectionParameters(OFString& str)
{
  str = "Transport connection: TCP/IP, unencrypted.";
  return str;
}

const char *DcmTCPConnection::errorString(DcmTransportLayerStatus code)
{
  switch (code)
  {
    case TCS_ok:
      return "no error";
      /* break; */
    case TCS_noConnection:
      return "no secure connection in place";
      /* break; */
    case TCS_tlsError:
      return "TLS error";
      /* break; */
    case TCS_illegalCall:
      return "illegal call";
      /* break; */
    case TCS_unspecifiedError:
      return "unspecified error";
      /* break; */
  }
  return "unknown error code";
}

