// Copyright 2018 The Outline Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// TODO: make import order irrelevant!
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <stdio.h>
// clang-format on

#define MALLOC(x) HeapAlloc(GetProcessHeap(), 0, (x))
#define FREE(x) HeapFree(GetProcessHeap(), 0, (x))

void usage(const char* path) {
  printf("usage: on <tun2socks> <proxy>|off <tun2socks> <proxy> <previous gateway>\n");
  exit(1);
}

// while the route command will figure out the best interface,
// these API calls do not.
DWORD getBest(DWORD ip) {
  DWORD best;
  DWORD dwStatus = GetBestInterface(ip, &best);
  if (dwStatus != NO_ERROR) {
    printf("could not figure best interface for IP: %d\n", dwStatus);
    exit(1);
  }
  return best;
}

DWORD getInterfaceMetric(DWORD interfaceIndex) {
  MIB_IPINTERFACE_ROW ipInterfaceRow = {0};
  ipInterfaceRow.Family = AF_INET;
  ipInterfaceRow.InterfaceIndex = interfaceIndex;
  DWORD dwStatus = GetIpInterfaceEntry(&ipInterfaceRow);
  if (dwStatus != NO_ERROR) {
    printf("could not call GetIpInterfaceEntry: %d\n", dwStatus);
    exit(1);
  }
  return ipInterfaceRow.Metric;
}

PMIB_IPFORWARDROW createRowForSingleIp() {
  PMIB_IPFORWARDROW row = (PMIB_IPFORWARDROW)malloc(sizeof(MIB_IPFORWARDROW));
  if (!row) {
    printf("Malloc failed. Out of memory.\n");
    exit(1);
  }

  // all fields:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/aa366850(v=vs.85).aspx
  row->dwForwardDest = 0;
  row->dwForwardMask = 0xFFFFFFFF;  // 255.255.255.255
  row->dwForwardPolicy = 0;
  row->dwForwardNextHop = 0;
  row->dwForwardIfIndex = 0;
  row->dwForwardType = 4;  /* the next hop is not the final dest */
  row->dwForwardProto = 3; /* PROTO_IP_NETMGMT */
  row->dwForwardAge = 0;
  row->dwForwardNextHopAS = 0;
  row->dwForwardMetric1 = 0;
  row->dwForwardMetric2 = 0;
  row->dwForwardMetric3 = 0;
  row->dwForwardMetric4 = 0;
  row->dwForwardMetric5 = 0;

  return row;
}

void createRoute(PMIB_IPFORWARDROW row) {
  DWORD dwStatus = CreateIpForwardEntry(row);
  if (dwStatus != ERROR_SUCCESS) {
    printf("could not delete route: %d\n", dwStatus);
    exit(1);
  }
}

void deleteRoute(PMIB_IPFORWARDROW row) {
  DWORD dwStatus = DeleteIpForwardEntry(row);
  if (dwStatus != ERROR_SUCCESS) {
    printf("could not delete route: %d\n", dwStatus);
    exit(1);
  }
}

// TODO: handle host names
int main(int argc, char* argv[]) {
  if (argc < 2) {
    usage(argv[0]);
  }

  int connecting = strcmp(argv[1], "on") == 0;

  if (argc != (connecting ? 4 : 5)) {
    usage(argv[0]);
  }

  DWORD NewGateway = INADDR_NONE;
  NewGateway = inet_addr(argv[2]);
  if (NewGateway == INADDR_NONE) {
    printf("could not parse tun2socks virtual router IP\n");
    return 1;
  }

  DWORD proxyServerIp = INADDR_NONE;
  proxyServerIp = inet_addr(argv[3]);
  if (proxyServerIp == INADDR_NONE) {
    printf("could not parse proxy server IP\n");
    return 1;
  }

  DWORD systemGatewayIp = INADDR_NONE;
  if (!connecting) {
    systemGatewayIp = inet_addr(argv[4]);
    if (systemGatewayIp == INADDR_NONE) {
      printf("could not parse system gateway IP\n");
      return 1;
    }
  }

  // TODO: remove this once tun2socks supports UDP
  DWORD dnsIp = inet_addr("8.8.8.8");

  // Fetch the system's routing table.
  PMIB_IPFORWARDTABLE pIpForwardTable = (MIB_IPFORWARDTABLE*)MALLOC(sizeof(MIB_IPFORWARDTABLE));
  if (pIpForwardTable == NULL) {
    printf("Error allocating memory\n");
    return 1;
  }

  DWORD dwSize = 0;
  if (GetIpForwardTable(pIpForwardTable, &dwSize, 0) == ERROR_INSUFFICIENT_BUFFER) {
    FREE(pIpForwardTable);
    pIpForwardTable = (MIB_IPFORWARDTABLE*)MALLOC(dwSize);
    if (pIpForwardTable == NULL) {
      printf("Error allocating memory\n");
      return 1;
    }
  }

  if (GetIpForwardTable(pIpForwardTable, &dwSize, 0) != NO_ERROR) {
    printf("could not query routing table\n");
    FREE(pIpForwardTable);
    return 1;
  }

  // default gateway.
  PMIB_IPFORWARDROW pRow = NULL;
  // tun2socks gateway.
  PMIB_IPFORWARDROW gwRow = NULL;
  // proxy server.
  PMIB_IPFORWARDROW proxyRow = NULL;
  // DNS server.
  PMIB_IPFORWARDROW dnsRow = NULL;

  // If tun2socks crashes, a kind of "shadow" route is left behind
  // which is invisible until tun2socks is *restarted*.
  // Because of this, if we find a gateway via the tun2socks device
  // we leave it alone.

  for (int i = 0; i < pIpForwardTable->dwNumEntries; i++) {
    PMIB_IPFORWARDROW row = &(pIpForwardTable->table[i]);

    if (row->dwForwardDest == 0) {
      // Gateway.
      if (row->dwForwardNextHop == NewGateway) {
        gwRow = row;
      } else if (row->dwForwardNextHop == systemGatewayIp) {
        printf("the previous gateway already exists\n");
        pRow = row;
      } else {
        if (pRow) {
          printf("cannot handle multiple gateways\n");
          exit(1);
        }
        pRow = row;
      }
    } else if (row->dwForwardDest == proxyServerIp) {
      if (proxyRow) {
        printf("found multiple routes to proxy server, cannot handle\n");
        exit(1);
      }
      proxyRow = row;
    } else if (row->dwForwardDest == dnsIp) {
      if (dnsRow) {
        printf("found multiple routes to DNS server, cannot handle\n");
        exit(1);
      }
      dnsRow = row;
    }
  }

  if (connecting) {
    if (!pRow) {
      printf("found no other gateway - cannot handle this\n");
      exit(1);
    }

    // Route via the tun2socks virtual router.
    // Ideally, we would *modify* the gateway rather than adding one and deleting
    // the old. That way, we could be almost certain of not leaving the user's
    // computer in an unuseable state. However, that does *not* work - as the
    // documentation for SetIpForwardEntry explicitly states:
    //   xxx
    //
    // Instead, we first add the new gateway before deleting the old.
    //
    // And what about just keeping the old one around? There are enough posts and
    // questions on the web around the topic of multiple gateways to suggest
    // this is dangerous thinking: and in Outline's case, the TAP interface seems to
    // always have a higher priority than any existing ethernet device - messing
    // with that is probably not going to end well.
    if (!gwRow) {
      // NOTE: tun2socks *must be active* before this returns the correct result.
      int newGatewayInterfaceIndex = getBest(NewGateway);

      // This turns out to be a crucial, undocumented step.
      // TODO: more comments!
      DWORD newGatewayInterfaceMetric = getInterfaceMetric(newGatewayInterfaceIndex);

      PMIB_IPFORWARDROW gwRow = createRowForSingleIp();
      gwRow->dwForwardMask = 0;
      gwRow->dwForwardNextHop = NewGateway;
      gwRow->dwForwardIfIndex = newGatewayInterfaceIndex;
      gwRow->dwForwardMetric1 = newGatewayInterfaceMetric;

      createRoute(gwRow);
      printf("added new gateway\n");
    }

    // Delete the previous gateway and print its IP so Outline can restore it.
    DWORD oldGateway = pRow->dwForwardNextHop;
    char oldGatewayIp[128];
    struct in_addr IpAddr;
    IpAddr.S_un.S_addr = (u_long)pRow->dwForwardNextHop;
    strcpy(oldGatewayIp, inet_ntoa(IpAddr));
    printf("current gateway: %s\n", oldGatewayIp);

    deleteRoute(pRow);
    printf("removed old gateway\n");

    int oldGatewayInterfaceIndex = getBest(oldGateway);
    DWORD oldGatewayInterfaceMetric = getInterfaceMetric(oldGatewayInterfaceIndex);

    // Add a route to the proxy server.
    if (proxyRow) {
      deleteRoute(proxyRow);
      printf("removed old route to proxy server\n");
    }
    proxyRow = createRowForSingleIp();
    proxyRow->dwForwardDest = proxyServerIp;
    proxyRow->dwForwardNextHop = oldGateway;
    proxyRow->dwForwardMetric1 = oldGatewayInterfaceMetric;
    proxyRow->dwForwardIfIndex = oldGatewayInterfaceIndex;
    createRoute(proxyRow);
    printf("added route to proxy server\n");

    // Add a route to the DNS server.
    if (dnsRow) {
      deleteRoute(dnsRow);
      printf("deleted old route to DNS server\n");
    }
    dnsRow = createRowForSingleIp();
    dnsRow->dwForwardDest = dnsIp;
    dnsRow->dwForwardNextHop = oldGateway;
    dnsRow->dwForwardMetric1 = oldGatewayInterfaceMetric;
    dnsRow->dwForwardIfIndex = oldGatewayInterfaceIndex;
    createRoute(dnsRow);
    printf("added new route to DNS server\n");
  } else {
    // TODO: When we aren't told the previous gateway, make a guess.
    if (gwRow) {
      deleteRoute(gwRow);
      printf("removed tun2socks gateway\n");
    }

    if (!pRow) {
      int oldGatewayInterfaceIndex = getBest(systemGatewayIp);
      DWORD oldGatewayInterfaceMetric = getInterfaceMetric(oldGatewayInterfaceIndex);

      PMIB_IPFORWARDROW gwRow = createRowForSingleIp();
      gwRow->dwForwardMask = 0;
      gwRow->dwForwardNextHop = systemGatewayIp;
      gwRow->dwForwardIfIndex = oldGatewayInterfaceIndex;
      gwRow->dwForwardMetric1 = oldGatewayInterfaceMetric;

      createRoute(gwRow);
      printf("restored gateway\n");
    }

    if (proxyRow) {
      deleteRoute(proxyRow);
      printf("removed route to proxy server\n");
    }

    if (dnsRow) {
      deleteRoute(dnsRow);
      printf("removed route to DNS server\n");
    }
  }

  exit(0);
}
