// HaxWall: HaxBall firewall for Windows

#include "stdafx.h"

//#define BLOCK_DATA_CENTERS // uncomment flag when compiling flavors

#include "ban.h"
#include "PacketFilter.h"
#include <Winsock2.h>
#include <Mstcpip.h>
#include <Iphlpapi.h>
#include <Ws2tcpip.h>
#include <cstdint>
#include <iostream>
#include <list>
#include "haxball_whitelist.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")

#define VERIFICATION_PORT 1337 // Port for signature verification service

PacketFilter pktFilter;

void DisableQuickEditMode()
{
	DWORD mode;
	HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
	GetConsoleMode(handle, &mode);
	mode |= ENABLE_EXTENDED_FLAGS;
	SetConsoleMode(handle, mode);
	mode &= ~ENABLE_QUICK_EDIT_MODE;
	SetConsoleMode(handle, mode);
}

void ListIpAddresses(std::list<SOCKADDR_IN> &list)
{
	IP_ADAPTER_ADDRESSES adapter_addresses[0xFF];
	DWORD adapter_addresses_buffer_size = sizeof(adapter_addresses);

	DWORD error = ::GetAdaptersAddresses(
		AF_INET,
		GAA_FLAG_SKIP_ANYCAST |
		GAA_FLAG_SKIP_MULTICAST |
		GAA_FLAG_SKIP_DNS_SERVER |
		GAA_FLAG_SKIP_FRIENDLY_NAME,
		NULL,
		adapter_addresses,
		&adapter_addresses_buffer_size);
	
	if (error != ERROR_SUCCESS)
	{
		return;
	}

	// Iterate through all of the adapters
	for (IP_ADAPTER_ADDRESSES* adapter = adapter_addresses; NULL != adapter; adapter = adapter->Next)
	{
		// Skip loopback adapters
		if (IF_TYPE_SOFTWARE_LOOPBACK == adapter->IfType)
		{
			continue;
		}

		// Parse all IPv4 and IPv6 addresses
		for (
			IP_ADAPTER_UNICAST_ADDRESS* address = adapter->FirstUnicastAddress;
			NULL != address;
			address = address->Next)
		{
			auto family = address->Address.lpSockaddr->sa_family;
			if (AF_INET == family)
			{
				SOCKADDR_IN* ipv4 = reinterpret_cast<SOCKADDR_IN*>(address->Address.lpSockaddr);
				list.push_back(*ipv4);
			}
		}
	}
}

void ban(uint32_t saddr)
{
	char buf[INET_ADDRSTRLEN];
	snprintf(buf, sizeof(buf), "%d.%d.%d.%d", (saddr >> 24) & 0xFF, (saddr >> 16) & 0xFF,
		(saddr >> 8) & 0xFF, saddr & 0xFF);
	pktFilter.Block(buf);
}

void unban(uint32_t saddr)
{
	char buf[INET_ADDRSTRLEN];
	snprintf(buf, sizeof(buf), "%d.%d.%d.%d", (saddr >> 24) & 0xFF, (saddr >> 16) & 0xFF,
		(saddr >> 8) & 0xFF, saddr & 0xFF);
	pktFilter.Unblock(buf);
}

BOOL WINAPI ConsoleHandlerRoutine(DWORD dwCtrlType)
{
	switch (dwCtrlType)
	{
		case CTRL_CLOSE_EVENT:
		case CTRL_LOGOFF_EVENT:
		case CTRL_SHUTDOWN_EVENT:
		case CTRL_C_EVENT:
			std::cout << "Exiting..." << std::endl;
			pktFilter.StopFirewall();
			exit(0);
			return TRUE;
		default:
			break;
	}
	return FALSE;
}

int main()
{
	DisableQuickEditMode(); // https://stackoverflow.com/q/30418886

	// Start firewall.
	if (pktFilter.StartFirewall())
	{
		std::cout << "Packet filter started successfully..." << std::endl;
	}
	else
	{
		std::cerr << "Error starting packet filter: " << GetLastError() << std::endl;
		return 1;
	}

	if (!SetConsoleCtrlHandler(ConsoleHandlerRoutine, TRUE))
	{
		std::cerr << "Failed to set exit handler." << std::endl;
	}

	WSAData wsa = { 0 };
	WSAStartup(MAKEWORD(2, 2), &wsa);

	std::list<SOCKADDR_IN> bind_addrs;
	ListIpAddresses(bind_addrs);
	if (bind_addrs.size() == 0)
	{
		std::cerr << "Failed to find interface addresses" << std::endl;
		return 1;
	}

	unsigned char data[0xFFFF];
	AttackFirewall fw(ban, unban);

	FD_SET socket_set;
	FD_SET backup_set;
	FD_ZERO(&socket_set);
	FD_SET* current_set = &socket_set;
	FD_SET* next_set = &backup_set;

	bool bound = false;
	std::list<SOCKET> sockets;

	SOCKET verification_socket = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in verification_addr;
	verification_addr.sin_family = AF_INET;
	verification_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	verification_addr.sin_port = htons(VERIFICATION_PORT);
	u_long nonblocking = 1;
	if (verification_socket == INVALID_SOCKET)
	{
		std::cerr << "Failed to start verification service." << std::endl;
	}
	else if (bind(verification_socket, (struct sockaddr*)&verification_addr, sizeof(verification_addr)) == SOCKET_ERROR)
	{
		std::cerr << "Failed to bind to verification service: " << WSAGetLastError() << std::endl;
	}
	else
	{
		FD_SET(verification_socket, &socket_set);
		sockets.push_back(verification_socket);
	}

	for (auto it = bind_addrs.begin(); it != bind_addrs.end(); it++)
	{
		SOCKET sock = socket(AF_INET, SOCK_RAW, IPPROTO_IP);
		if (sock != INVALID_SOCKET)
		{
			SOCKADDR_IN bind_addr = *it;
			if (bind(sock, (struct sockaddr*)&bind_addr, sizeof(SOCKADDR_IN)) != 0)
			{
				std::cerr << "Failed to bind socket: " << WSAGetLastError() << std::endl;
				continue;
			}
			unsigned int opt = RCVALL_IPLEVEL;
			DWORD ret;
			if (WSAIoctl(sock, SIO_RCVALL, &opt, sizeof(opt), 0, 0, &ret, 0, 0) != 0)
			{
				std::cerr << "Failed to enable promiscuous mode: " << WSAGetLastError() << std::endl;
				continue;
			}

			uint32_t address = ntohl(*((uint32_t*)&bind_addr.sin_addr));
			fw.AddWhitelist(address);
			fw.Log("Protecting", address);

			FD_SET(sock, &socket_set);
			sockets.push_back(sock);
			bound = true;
		}
	}

	if (!bound)
	{
		std::cerr << "Failed to listen on any interface." << std::endl;
		return 1;
	}

#ifdef BLOCK_DATA_CENTERS
	std::cout << "Data center blacklisting enabled." << std::endl;
	fw.SetBlacklist(&DataCenters, &HaxBallMatcher);
#else
	std::cout << "Data center blacklisting disabled." << std::endl;
	fw.SetBlacklist(NULL, &HaxBallMatcher);
#endif

	std::cout << "Firewall started. Keep this window open." << std::endl << std::endl;

	struct sockaddr_in receiver;
	int receiver_len = sizeof(receiver);

	while (1)
	{
		FD_ZERO(next_set);
		if (select(0, current_set, NULL, NULL, NULL) == SOCKET_ERROR)
		{
			std::cerr << "Error: Select failed. " << WSAGetLastError() << std::endl;
			return 1;
		}
		for (auto it = sockets.begin(); it != sockets.end(); it++)
		{
			FD_SET(*it, next_set);
			if (!FD_ISSET(*it, current_set))
			{
				continue;
			}
			int count = (*it != verification_socket) ? recvfrom(*it, (char *)data, sizeof(data), 0, NULL, NULL)
				: recvfrom(*it, (char *)data, sizeof(data), 0, (struct sockaddr*)&receiver, &receiver_len);
			if (count > 0)
			{
				if (*it == verification_socket)
				{
					if (count != 4)
					{
						continue;
					}
					uint32_t addr = ntohl(*((uint32_t*)data));
					data[0] = fw.IsActive(addr) ? 1 : 0;
					fw.Log("Query:", addr);
					sendto(*it, (char*)data, 1, 0, (struct sockaddr*)&receiver, receiver_len);
					continue;
				}

				if (count < 28 || data[9] != 0x11) // Must be IP header with UDP payload
				{
					continue;
				}

				uint32_t saddr = ntohl(*((uint32_t*)(data + 12)));
				uint32_t daddr = ntohl(*((uint32_t*)(data + 16)));
				uint16_t sport = ntohs(*((uint16_t*)(data + 20)));
				uint16_t dport = ntohs(*((uint16_t*)(data + 22)));

				if (sport < 1024 || dport < 1024 || dport == 3389) // Allow incoming and outgoing low port services like DNS and do not ban RDP packets.
				{
					// The source port check actually decreases the effectiveness of the firewall.
					// However, the usual skid will hardly be able to make it around this check.
					continue;
				}

				fw.ReceivePacket(saddr, sport);
				fw.ClearOldEntries();
			}
			else
			{
				std::cerr << "An error occured." << std::endl;
				return 1;
			}
		}
		if (current_set == &socket_set)
		{
			current_set = &backup_set;
			next_set = &socket_set;
		}
		else
		{
			current_set = &socket_set;
			next_set = &backup_set;
		}
	}
    return 0;
}

