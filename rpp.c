/**
  * @brief This is a simple rpp tool to interact with RDE controllers
  *
  * @author Mateusz Viste
  * @copyright Copyright (C) 2016, Border 6 S.A.S, All rights reserved.
  *
  * Redistribution and use in source and binary forms, with or without
  * modification, are permitted provided that the following conditions are met:
  *
  * - Redistributions of source code must retain the above copyright notice,
  *   this list of conditions and the following disclaimer.
  *
  * - Redistributions in binary form must reproduce the above copyright notice,
  *   this list of conditions and the following disclaimer in the documentation
  *   and/or other materials provided with the distribution.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
  * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  * POSSIBILITY OF SUCH DAMAGE.
  */

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <errno.h>
#include <resolv.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PVER "20160429"
#define PDATE "2016"


/** @brief computes a revdns string for a given IPv4 or IPv6 address
  * @param *res a pointer to the string where result should be written
  * @param reslen the amount of space available in *res
  * @param *orig a pointer to the original string representing the IP address
  * @return 0 on success, non-zero otherwise
  */
static int ip2revdns(char *res, int reslen, char *orig) {
  int ipfamily;
  struct in6_addr ip6;
  /* input cannot be less than 4 bytes long */
  if (res == NULL) return(-1);
  if ((orig == NULL) || (strlen(orig) < 4)) {
    *res = 0;
    return(-1);
  }
  /* is it ip4 or ip6? */
  if ((orig[1] == '.') || (orig[2] == '.') || (orig[3] == '.')) {
    ipfamily = AF_INET;
  } else {
    ipfamily = AF_INET6;
  }
  /* convert str to binary */
  if (inet_pton(ipfamily, orig, &(ip6.s6_addr)) != 1) {
    *res = 0;
    return(-1);
  }
  /* compute the reverse string */
  if (ipfamily == AF_INET) {
    snprintf(res, reslen, "%d.%d.%d.%d.in-addr.arpa", ip6.s6_addr[3], ip6.s6_addr[2], ip6.s6_addr[1], ip6.s6_addr[0]);
  } else {
    int i, rlen = 0;
    for (i = 15; i >= 0; i--) {
      rlen += snprintf(res + rlen, reslen - rlen, "%x.%x.", ip6.s6_addr[i] & 0x0F, ip6.s6_addr[i] >> 4);
      if (rlen >= reslen) { /* error - res too short! */
        *res = 0;
        return(-1);
      }
    }
    rlen += snprintf(res + rlen, reslen - rlen, "ip6.arpa");
    if (rlen >= reslen) { /* error - res too short! */
      *res = 0;
      return(-1);
    }
  }
  /* all fine */
  return(0);
}


static void printhelp(void) {
  printf("rpp version " PVER " Copyright (C) " PDATE " Border 6 S.A.S\n"
         "\n"
         "rpp is a simple tool that allows to resolve and interact with remote RDE\n"
         "controllers.\n"
         "\n"
         "usage: rpp resolve|advertise remoteprefix [localprefixes preflist]\n"
         "\n"
         "where:\n"
         "'localprefixes' is the list of the prefixes advertised by the local AS.\n"
         "\n"
         "'preflist' is to be provided only for the 'advertise' action. it should\n"
         "be a single argument that contains the list of preffered ASes with weights to\n"
         "be advertised to the remote controller.\n"
         "\n");
  printf("examples:\n"
         "  rpp resolve 203.0.113.0/24\n"
         "  rpp advertise 203.0.113.0/24 '192.0.2.0/24 198.51.100.0/24' '64552:0 64900:255 65001:127'\n"
         "\n");
}


/** @brief resolves the routing controller for a given prefix' revDNS
  * @param *result the IP address of the routing controller is filled there
  * @param *revname the revDNS string we want to look at
  * @return 0 on success, negative value on resolution failure, 1 if resolution went fine but this prefix don't seem to have a RDE record
  */
static int rpp_getcontroller(char *result, int maxres, char *revname) {
  int i;
  unsigned char answer[512];
  ns_msg msg;
  int msg_count;

  i = res_query(revname, C_IN, T_TXT, answer, sizeof(answer));
  if (i < 0) return(1); /* res_query "fails" if no TXT record is found */

  /* parse the response */
  if (ns_initparse(answer, i, &msg) != 0) return(-3);

  msg_count = ns_msg_count(msg, ns_s_an);
  if (msg_count == 0) return(1); /* return 1 if all went fine but there is no RDE record */

  /* look at all received answers, until one looks fine enough */
  for (i = 0 ; i < msg_count ; i++ ) {
    ns_rr rr;
    if (ns_parserr(&msg, ns_s_an, i, &rr)) {
      return(-4);
    }
    /* if it doesn't start with 'RDE:', then ignore this entry */
    if ((ns_rr_rdata(rr)[1] != 'R') || (ns_rr_rdata(rr)[2] != 'D') || (ns_rr_rdata(rr)[3] != 'E') || (ns_rr_rdata(rr)[4] != ':')) continue;
    /* otherwise it's what we are looking for - copy it to result and quit */
    if (ns_rr_rdata(rr)[0] < maxres) maxres = ns_rr_rdata(rr)[0];
    maxres -= 4;
    memcpy(result, ns_rr_rdata(rr) + 5, maxres);
    result[maxres] = 0;
    return(0);
  }

  return(1);
}


/** @brief advertises our inbound preferences to a remote prefix
  * @return returns 0 on success, non-zero otherwise */
static int advertise_inpref_to_remote_dst(char *locpreflist, int ttl, char *servstringaddr, char *preflist) {
  char buff[64];
  int bufflen;
  int sock;
  struct sockaddr_in servaddr;

  /* construct the server address structure */
  memset(&servaddr, 0, sizeof(servaddr));  /* zero out structure */
  servaddr.sin_family = AF_INET;  /* internet address family */
  servaddr.sin_addr.s_addr = inet_addr(servstringaddr);  /* server IP address */
  servaddr.sin_port = htons(4343);  /* server port */

  sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    printf("ERROR: socket() call failed (%s)\n", strerror(errno));
    return(-1);
  }

  /* establish the connection to the server */
  if (connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
    printf("ERROR: connection to the remote controller failed (%s)\n", strerror(errno));
    return(-2);
  }

  /* send our inbound routing preferences to the remote controller */
  bufflen = sprintf(buff, "SETINPREF %d\t", ttl);
  if (send(sock, buff, bufflen, MSG_MORE) != bufflen) {
    printf("ERROR: failed to send routing prefs to %s (%s)\n", servstringaddr, strerror(errno));
    close(sock);
    return(-3);
  }

  /* send the list of our local prefixes */
  if (send(sock, locpreflist, strlen(locpreflist), MSG_MORE) != (unsigned)strlen(locpreflist)) {
    printf("ERROR: failed to send routing prefs to %s (%s)\n", servstringaddr, strerror(errno));
    close(sock);
    return(-4);
  }
  if (send(sock, "\t", 1, MSG_MORE) != 1) {
    printf("ERROR: failed to send routing prefs to %s (%s)\n", servstringaddr, strerror(errno));
    close(sock);
    return(-5);
  }

  if (send(sock, preflist, strlen(preflist), MSG_MORE) != (unsigned)strlen(preflist)) {
    printf("ERROR: failed to send routing prefs to %s (%s)\n", servstringaddr, strerror(errno));
    close(sock);
    return(-6);
  }

  /* terminate with a \r\n and close the connection */
  if (send(sock, "\r\n", 2, 0) != 2) {
    printf("ERROR: failed to send routing prefs to %s (%s)\n", servstringaddr, strerror(errno));
    close(sock);
    return(-7);
  }

  close(sock);
  return(0);
}


#define RESOLVE 0
#define ADVERTISE 1

int main(int argc, char **argv) {
  int action;
  int i;
  unsigned x;
  char *prefixorg;
  char prefix[128];
  char revdns[128];
  char rdeaddr[128];
  char *locpreflist = NULL, *preflist = NULL;

  /* validate command and number of CLI arguments */
  if ((argc == 3) && (strcmp(argv[1], "resolve") == 0)) {
    action = RESOLVE;
  } else if ((argc == 5) && (strcmp(argv[1], "advertise") == 0)) {
    action = ADVERTISE;
    locpreflist = argv[3];
    preflist = argv[4];
  } else {
    printhelp();
    return(1);
  }
  prefixorg = argv[2];

  /* look at the prefix - if a '/' is found, cut it out */
  for (x = 0; (prefixorg[x] != 0) && (prefixorg[x] != '/') && (x < sizeof(prefix) - 1); x++) prefix[x] = prefixorg[x];
  prefix[x] = 0;

  /* compute the revDNS representation of the given prefix */
  if (ip2revdns(revdns, sizeof(revdns), prefix) != 0) {
    printf("ERROR: failed to compute a reverse DNS for '%s'\n", prefixorg);
    return(1);
  }

  /* resolve RDE controller's address for the given prefix */
  i = rpp_getcontroller(rdeaddr, sizeof(rdeaddr), revdns);
  if (i == 0) {
    printf("RDE controller for %s is %s\n", prefixorg, rdeaddr);
  } else if (i > 0) {
    printf("No RDE entry found for %s\n", prefixorg);
  } else {
    printf("ERROR: DNS failure (%d)\n", i);
  }

  /* if action is 'resolve', then stop here */
  if (action == RESOLVE) return(0);

  puts("Sending preferences...");

  /* send a SETINPREF query to the remote controller */
  if (advertise_inpref_to_remote_dst(locpreflist, 3600, rdeaddr, preflist) == 0) {
    puts("Done.");
  }

  return(0);
}
