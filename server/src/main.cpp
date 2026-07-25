#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <time.h>
#include <detect.h>
#include <system.h>
#include <argparse.h>
#include <json.h>
#include "server.h"
#include "main.h"

#if defined(CONF_FAMILY_UNIX)
	#include <signal.h>
#endif

#ifndef PRId64
	#define PRId64 "I64d"
#endif

static volatile int gs_Running = 1;
static volatile int gs_ReloadConfig = 0;

static void ExitFunc(int Signal)
{
	printf("[EXIT] Caught signal %d\n", Signal);
	gs_Running = 0;
}

static void ReloadFunc(int Signal)
{
	printf("[RELOAD] Caught signal %d\n", Signal);
	gs_ReloadConfig = 1;
}

CConfig::CConfig()
{
	// Initialize to default values
	m_Verbose = false; // -v, --verbose
	str_copy(m_aConfigFile, "config.json", sizeof(m_aConfigFile)); // -c, --config
	str_copy(m_aWebDir, "../status/", sizeof(m_aJSONFile)); // -d, --web-dir
	str_copy(m_aTemplateFile, "template.html", sizeof(m_aTemplateFile));
	str_copy(m_aJSONFile, "json/stats.json", sizeof(m_aJSONFile));
	str_copy(m_aBindAddr, "", sizeof(m_aBindAddr)); // -b, --bind
	m_Port = 35601; // -p, --port
}

CMain::CMain(CConfig Config) : m_Config(Config)
{
	mem_zero(m_aClients, sizeof(m_aClients));
	for(int i = 0; i < NET_MAX_CLIENTS; i++)
		m_aClients[i].m_ClientNetID = -1;
}

CMain::CClient *CMain::ClientNet(int ClientNetID)
{
	if(ClientNetID < 0 || ClientNetID >= NET_MAX_CLIENTS)
		return 0;

	for(int i = 0; i < NET_MAX_CLIENTS; i++)
	{
		if(Client(i)->m_ClientNetID == ClientNetID)
			return Client(i);
	}

	return 0;
}

int CMain::ClientNetToClient(int ClientNetID)
{
	if(ClientNetID < 0 || ClientNetID >= NET_MAX_CLIENTS)
		return -1;

	for(int i = 0; i < NET_MAX_CLIENTS; i++)
	{
		if(Client(i)->m_ClientNetID == ClientNetID)
			return i;
	}

	return -1;
}

void CMain::OnNewClient(int ClientNetID, int ClientID)
{
	dbg_msg("main", "OnNewClient(ncid=%d, cid=%d)", ClientNetID, ClientID);
	Client(ClientID)->m_ClientNetID = ClientNetID;
	Client(ClientID)->m_ClientNetType = m_Server.Network()->ClientAddr(ClientNetID)->type;
	Client(ClientID)->m_TimeConnected = time_get();
	Client(ClientID)->m_Connected = true;

	if(Client(ClientID)->m_ClientNetType == NETTYPE_IPV4)
		Client(ClientID)->m_Stats.m_Online4 = true;
	else if(Client(ClientID)->m_ClientNetType == NETTYPE_IPV6)
		Client(ClientID)->m_Stats.m_Online6 = true;
}

void CMain::OnDelClient(int ClientNetID)
{
	int ClientID = ClientNetToClient(ClientNetID);
	dbg_msg("main", "OnDelClient(ncid=%d, cid=%d)", ClientNetID, ClientID);
	if(ClientID >= 0 && ClientID < NET_MAX_CLIENTS)
	{
		Client(ClientID)->m_Connected = false;
		Client(ClientID)->m_ClientNetID = -1;
		Client(ClientID)->m_ClientNetType = NETTYPE_INVALID;
		mem_zero(&Client(ClientID)->m_Stats, sizeof(CClient::CStats));
	}
}

int CMain::HandleMessage(int ClientNetID, char *pMessage)
{
	CClient *pClient = ClientNet(ClientNetID);
	if(!pClient)
		return true;

	if(str_comp_num(pMessage, "update", sizeof("update")-1) == 0)
	{
		char *pData = str_skip_whitespaces(&pMessage[sizeof("update")-1]);

		// parse json data
		json_settings JsonSettings;
		mem_zero(&JsonSettings, sizeof(JsonSettings));
		char aError[256];
		json_value *pJsonData = json_parse_ex(&JsonSettings, pData, strlen(pData), aError);
		if(!pJsonData)
		{
			dbg_msg("main", "JSON Error: %s", aError);
			if(pClient->m_Stats.m_Pong)
				m_Server.Network()->Send(ClientNetID, "1");
			return 1;
		}

		// extract data
		const json_value &rStart = (*pJsonData);
		if(rStart["uptime"].type)
			pClient->m_Stats.m_Uptime = rStart["uptime"].u.integer;
		if(rStart["load"].type)
			pClient->m_Stats.m_Load = rStart["load"].u.dbl;
		if(rStart["network_rx"].type)
			pClient->m_Stats.m_NetworkRx = rStart["network_rx"].u.integer;
		if(rStart["network_tx"].type)
			pClient->m_Stats.m_NetworkTx = rStart["network_tx"].u.integer;
		if(rStart["memory_total"].type)
			pClient->m_Stats.m_MemTotal = rStart["memory_total"].u.integer;
		if(rStart["memory_used"].type)
			pClient->m_Stats.m_MemUsed = rStart["memory_used"].u.integer;
		if(rStart["swap_total"].type)
			pClient->m_Stats.m_SwapTotal = rStart["swap_total"].u.integer;
		if(rStart["swap_used"].type)
			pClient->m_Stats.m_SwapUsed = rStart["swap_used"].u.integer;
		if(rStart["hdd_total"].type)
			pClient->m_Stats.m_HDDTotal = rStart["hdd_total"].u.integer;
		if(rStart["hdd_used"].type)
			pClient->m_Stats.m_HDDUsed = rStart["hdd_used"].u.integer;
		if(rStart["cpu"].type)
			pClient->m_Stats.m_CPU = rStart["cpu"].u.dbl;
		if(rStart["online4"].type && pClient->m_ClientNetType == NETTYPE_IPV6)
			pClient->m_Stats.m_Online4 = rStart["online4"].u.boolean;
		if(rStart["online6"].type && pClient->m_ClientNetType == NETTYPE_IPV4)
			pClient->m_Stats.m_Online6 = rStart["online6"].u.boolean;
		if(rStart["custom"].type == json_string)
			str_copy(pClient->m_Stats.m_aCustom, rStart["custom"].u.string.ptr, sizeof(pClient->m_Stats.m_aCustom));

		if(m_Config.m_Verbose)
		{
			if(rStart["online4"].type)
				dbg_msg("main", "Online4: %s\nUptime: %" PRId64 "\nLoad: %f\nNetworkRx: %" PRId64 "\nNetworkTx: %" PRId64 "\nMemTotal: %" PRId64 "\nMemUsed: %" PRId64 "\nSwapTotal: %" PRId64 "\nSwapUsed: %" PRId64 "\nHDDTotal: %" PRId64 "\nHDDUsed: %" PRId64 "\nCPU: %f\n",
					rStart["online4"].u.boolean ? "true" : "false",
					pClient->m_Stats.m_Uptime, pClient->m_Stats.m_Load, pClient->m_Stats.m_NetworkRx, pClient->m_Stats.m_NetworkTx, pClient->m_Stats.m_MemTotal, pClient->m_Stats.m_MemUsed, pClient->m_Stats.m_SwapTotal, pClient->m_Stats.m_SwapUsed, pClient->m_Stats.m_HDDTotal, pClient->m_Stats.m_HDDUsed, pClient->m_Stats.m_CPU);
			else if(rStart["online6"].type)
				dbg_msg("main", "Online6: %s\nUptime: %" PRId64 "\nLoad: %f\nNetworkRx: %" PRId64 "\nNetworkTx: %" PRId64 "\nMemTotal: %" PRId64 "\nMemUsed: %" PRId64 "\nSwapTotal: %" PRId64 "\nSwapUsed: %" PRId64 "\nHDDTotal: %" PRId64 "\nHDDUsed: %" PRId64 "\nCPU: %f\n",
					rStart["online6"].u.boolean ? "true" : "false",
					pClient->m_Stats.m_Uptime, pClient->m_Stats.m_Load, pClient->m_Stats.m_NetworkRx, pClient->m_Stats.m_NetworkTx, pClient->m_Stats.m_MemTotal, pClient->m_Stats.m_MemUsed, pClient->m_Stats.m_SwapTotal, pClient->m_Stats.m_SwapUsed, pClient->m_Stats.m_HDDTotal, pClient->m_Stats.m_HDDUsed, pClient->m_Stats.m_CPU);
			else
				dbg_msg("main", "Uptime: %" PRId64 "\nLoad: %f\nNetworkRx: %" PRId64 "\nNetworkTx: %" PRId64 "\nMemTotal: %" PRId64 "\nMemUsed: %" PRId64 "\nSwapTotal: %" PRId64 "\nSwapUsed: %" PRId64 "\nHDDTotal: %" PRId64 "\nHDDUsed: %" PRId64 "\nCPU: %f\n",
					pClient->m_Stats.m_Uptime, pClient->m_Stats.m_Load, pClient->m_Stats.m_NetworkRx, pClient->m_Stats.m_NetworkTx, pClient->m_Stats.m_MemTotal, pClient->m_Stats.m_MemUsed, pClient->m_Stats.m_SwapTotal, pClient->m_Stats.m_SwapUsed, pClient->m_Stats.m_HDDTotal, pClient->m_Stats.m_HDDUsed, pClient->m_Stats.m_CPU);
		}

		// clean up
		json_value_free(pJsonData);

		if(pClient->m_Stats.m_Pong)
			m_Server.Network()->Send(ClientNetID, "0");
		return 0;
	}
	else if(str_comp_num(pMessage, "pong", sizeof("pong")-1) == 0)
	{
		char *pData = str_skip_whitespaces(&pMessage[sizeof("pong")-1]);

		if(!str_comp(pData, "0") || !str_comp(pData, "off"))
			pClient->m_Stats.m_Pong = false;
		else if(!str_comp(pData, "1") || !str_comp(pData, "on"))
			pClient->m_Stats.m_Pong = true;

		return 0;
	}

	if(pClient->m_Stats.m_Pong)
		m_Server.Network()->Send(ClientNetID, "1");

	return 1;
}

// Escape a string for embedding in a JSON string literal ('"' and '\').
// Control characters are already stripped on receive (str_sanitize_cc) and
// config files are trusted, so those are the only two that can break the output.
static void json_escape(char *pDst, int DstSize, const char *pSrc)
{
	int d = 0;
	for(const char *p = pSrc; *p && d < DstSize-2; p++)
	{
		if(*p == '"' || *p == '\\')
			pDst[d++] = '\\';
		pDst[d++] = *p;
	}
	pDst[d] = 0;
}

void CMain::JSONUpdateThread(void *pUser)
{
	CJSONUpdateThreadData *m_pJSONUpdateThreadData = (CJSONUpdateThreadData *)pUser;
	CClient *pClients = m_pJSONUpdateThreadData->pClients;
	CConfig *pConfig = m_pJSONUpdateThreadData->pConfig;

	while(gs_Running)
	{
		char aFileBuf[2048*NET_MAX_CLIENTS];
		char *pBuf = aFileBuf;
		int Entries = 0;

		str_format(pBuf, sizeof(aFileBuf), "{\n\"servers\": [\n");
		pBuf += strlen(pBuf);

		for(int i = 0; i < NET_MAX_CLIENTS; i++)
		{
			char aName[2*128], aType[2*128], aHost[2*128], aLocation[2*128], aCustom[2*512];

			if(!pClients[i].m_Active || pClients[i].m_Disabled)
				continue;

			json_escape(aName, sizeof(aName), pClients[i].m_aName);
			json_escape(aType, sizeof(aType), pClients[i].m_aType);
			json_escape(aHost, sizeof(aHost), pClients[i].m_aHost);
			json_escape(aLocation, sizeof(aLocation), pClients[i].m_aLocation);
			json_escape(aCustom, sizeof(aCustom), pClients[i].m_Stats.m_aCustom);
			Entries++;

			if(pClients[i].m_Connected)
			{
				// Uptime
				char aUptime[16];
				int Days = pClients[i].m_Stats.m_Uptime/60.0/60.0/24.0;
				if(Days > 0)
				{
					if(Days > 1)
						str_format(aUptime, sizeof(aUptime), "%d days", Days);
					else
						str_format(aUptime, sizeof(aUptime), "%d day", Days);
				}
				else
					str_format(aUptime, sizeof(aUptime), "%02d:%02d:%02d", (int)(pClients[i].m_Stats.m_Uptime/60.0/60.0), (int)((pClients[i].m_Stats.m_Uptime/60)%60), (int)((pClients[i].m_Stats.m_Uptime)%60));

				str_format(pBuf, sizeof(aFileBuf) - (pBuf - aFileBuf), "{ \"name\": \"%s\", \"type\": \"%s\", \"host\": \"%s\", \"location\": \"%s\", \"online4\": %s, \"online6\": %s, \"uptime\": \"%s\", \"load\": %.2f, \"network_rx\": %" PRId64 ", \"network_tx\": %" PRId64 ", \"cpu\": %d, \"memory_total\": %" PRId64 ", \"memory_used\": %" PRId64 ", \"swap_total\": %" PRId64 ", \"swap_used\": %" PRId64 ", \"hdd_total\": %" PRId64 ", \"hdd_used\": %" PRId64 ", \"custom\": \"%s\" },\n",
					aName, aType, aHost, aLocation, pClients[i].m_Stats.m_Online4 ? "true" : "false", pClients[i].m_Stats.m_Online6 ? "true" : "false",	aUptime, pClients[i].m_Stats.m_Load, pClients[i].m_Stats.m_NetworkRx, pClients[i].m_Stats.m_NetworkTx, (int)pClients[i].m_Stats.m_CPU, pClients[i].m_Stats.m_MemTotal, pClients[i].m_Stats.m_MemUsed, pClients[i].m_Stats.m_SwapTotal, pClients[i].m_Stats.m_SwapUsed, pClients[i].m_Stats.m_HDDTotal, pClients[i].m_Stats.m_HDDUsed, aCustom);
				pBuf += strlen(pBuf);
			}
			else
			{
				str_format(pBuf, sizeof(aFileBuf) - (pBuf - aFileBuf), "{ \"name\": \"%s\", \"type\": \"%s\", \"host\": \"%s\", \"location\": \"%s\", \"online4\": false, \"online6\": false },\n",
					aName, aType, aHost, aLocation);
				pBuf += strlen(pBuf);
			}
		}
		if(Entries)
			pBuf -= 2; // rewind the trailing ",\n" — with zero entries there is none (the old unconditional -2 corrupted the '[' on an empty list)
		if(!m_pJSONUpdateThreadData->m_ReloadRequired)
			str_format(pBuf, sizeof(aFileBuf) - (pBuf - aFileBuf), "\n],\n\"updated\": \"%lld\"\n}", (long long)time(/*ago*/0));
		else
		{
			str_format(pBuf, sizeof(aFileBuf) - (pBuf - aFileBuf), "\n],\n\"updated\": \"%lld\",\n\"reload\": true\n}", (long long)time(/*ago*/0));
			m_pJSONUpdateThreadData->m_ReloadRequired--;
		}
		pBuf += strlen(pBuf);

		char aJSONFileTmp[1024];
		str_format(aJSONFileTmp, sizeof(aJSONFileTmp), "%s~", pConfig->m_aJSONFile);
		IOHANDLE File = io_open(aJSONFileTmp, IOFLAG_WRITE);
		if(!File)
		{
			dbg_msg("main", "Couldn't open %s", aJSONFileTmp);
			exit(1);
		}
		io_write(File, aFileBuf, (pBuf - aFileBuf));
		io_flush(File);
		io_close(File);
		fs_rename(aJSONFileTmp, pConfig->m_aJSONFile);
		// 2 s matches the frontend poll interval (serverstatus.js setInterval(uptime, 2000))
		// and halves the write load; there is no staleness cutoff in the frontend.
		thread_sleep(2000);
	}
	fs_remove(pConfig->m_aJSONFile);
}

int CMain::ReadConfig()
{
	// read and parse config
	IOHANDLE File = io_open(m_Config.m_aConfigFile, IOFLAG_READ);
	if(!File)
	{
		dbg_msg("main", "Couldn't open %s", m_Config.m_aConfigFile);
		return 1;
	}
	int FileSize = (int)io_length(File);
	char *pFileData = (char *)mem_alloc(FileSize + 1, 1);

	io_read(File, pFileData, FileSize);
	pFileData[FileSize] = 0;
	io_close(File);

	// parse json data
	json_settings JsonSettings;
	mem_zero(&JsonSettings, sizeof(JsonSettings));
	char aError[256];
	json_value *pJsonData = json_parse_ex(&JsonSettings, pFileData, strlen(pFileData), aError);
	if(!pJsonData)
	{
		dbg_msg("main", "JSON Error in file %s: %s", m_Config.m_aConfigFile, aError);
		mem_free(pFileData);
		return 1;
	}

	// reset clients
	for(int i = 0; i < NET_MAX_CLIENTS; i++)
	{
		if(!Client(i)->m_Active || !Client(i)->m_Connected)
			continue;

		m_Server.Network()->Drop(Client(i)->m_ClientNetID, "Server reloading...");
	}
	mem_zero(m_aClients, sizeof(m_aClients));
	for(int i = 0; i < NET_MAX_CLIENTS; i++)
		m_aClients[i].m_ClientNetID = -1;

	// extract data
	int ID = 0;
	const json_value &rStart = (*pJsonData)["servers"];
	if(rStart.type == json_array)
	{
		for(unsigned i = 0; i < rStart.u.array.length; i++)
		{
			if(ID < 0 || ID >= NET_MAX_CLIENTS)
				continue;

			Client(ID)->m_Active = true;
			Client(ID)->m_Disabled = rStart[i]["disabled"].u.boolean;
			str_copy(Client(ID)->m_aName, rStart[i]["name"].u.string.ptr, sizeof(Client(ID)->m_aName));
			str_copy(Client(ID)->m_aUsername, rStart[i]["username"].u.string.ptr, sizeof(Client(ID)->m_aUsername));
			str_copy(Client(ID)->m_aType, rStart[i]["type"].u.string.ptr, sizeof(Client(ID)->m_aType));
			str_copy(Client(ID)->m_aHost, rStart[i]["host"].u.string.ptr, sizeof(Client(ID)->m_aHost));
			str_copy(Client(ID)->m_aLocation, rStart[i]["location"].u.string.ptr, sizeof(Client(ID)->m_aLocation));
			str_copy(Client(ID)->m_aPassword, rStart[i]["password"].u.string.ptr, sizeof(Client(ID)->m_aPassword));

			if(m_Config.m_Verbose)
			{
				// never log passwords — verbose output lands in the journal
				if(Client(ID)->m_Disabled)
					dbg_msg("main", "[#%d: Name: \"%s\", Username: \"%s\", Type: \"%s\", Host: \"%s\", Location: \"%s\"]",
						ID, Client(ID)->m_aName, Client(ID)->m_aUsername, Client(ID)->m_aType, Client(ID)->m_aHost, Client(ID)->m_aLocation);
				else
					dbg_msg("main", "#%d: Name: \"%s\", Username: \"%s\", Type: \"%s\", Host: \"%s\", Location: \"%s\"",
						ID, Client(ID)->m_aName, Client(ID)->m_aUsername, Client(ID)->m_aType, Client(ID)->m_aHost, Client(ID)->m_aLocation);
			}
			ID++;
		}
	}

	// clean up
	json_value_free(pJsonData);
	mem_free(pFileData);

	// tell clients to reload the page
	m_JSONUpdateThreadData.m_ReloadRequired = 2;

	return 0;
}

int CMain::Run()
{
	if(m_Server.Init(this, m_Config.m_aBindAddr, m_Config.m_Port))
		return 1;

	if(ReadConfig())
		return 1;

	// Start JSON Update Thread
	m_JSONUpdateThreadData.m_ReloadRequired = 2;
	m_JSONUpdateThreadData.pClients = m_aClients;
	m_JSONUpdateThreadData.pConfig = &m_Config;
	void *LoadThread = thread_create(JSONUpdateThread, &m_JSONUpdateThreadData);
	//thread_detach(LoadThread);

	while(gs_Running)
	{
		if(gs_ReloadConfig)
		{
			if(ReadConfig())
				return 1;
			m_Server.NetBan()->UnbanAll();
			gs_ReloadConfig = 0;
		}

		m_Server.Update();

		// wait for incomming data
		// NOTE: the select() inside only watches the LISTEN socket — established
		// client sockets are drained by polling each pass, so this timeout is the
		// effective poll interval for client data. Clients report once per second;
		// 250 ms adds no observable latency and cuts idle wakeups 25x vs the old 10 ms.
		net_socket_read_wait(*m_Server.Network()->Socket(), 250);
	}

	dbg_msg("server", "Closing.");
	m_Server.Network()->Close();
	thread_wait(LoadThread);

	return 0;
}

int main(int argc, const char *argv[])
{
	int RetVal;
	dbg_logger_stdout();

	#if defined(CONF_FAMILY_UNIX)
		signal(SIGINT, ExitFunc);
		signal(SIGTERM, ExitFunc);
		signal(SIGQUIT, ExitFunc);
		signal(SIGHUP, ReloadFunc);
	#endif

	char aUsage[128];
	CConfig Config;
	str_format(aUsage, sizeof(aUsage), "%s [options]", argv[0]);
	const char *pConfigFile = 0;
	const char *pWebDir = 0;
	const char *pBindAddr = 0;

	struct argparse_option aOptions[] = {
		OPT_HELP(),
		OPT_BOOLEAN('v', "verbose", &Config.m_Verbose, "Verbose output", 0),
		OPT_STRING('c', "config", &pConfigFile, "Config file to use", 0),
		OPT_STRING('d', "web-dir", &pWebDir, "Location of the web directory", 0),
		OPT_STRING('b', "bind", &pBindAddr, "Bind to address", 0),
		OPT_INTEGER('p', "port", &Config.m_Port, "Listen on port", 0),
		OPT_END(),
	};
	struct argparse Argparse;
	argparse_init(&Argparse, aOptions, aUsage, 0);
	argc = argparse_parse(&Argparse, argc, argv);

	if(pConfigFile)
		str_copy(Config.m_aConfigFile, pConfigFile, sizeof(Config.m_aConfigFile));
	if(pWebDir)
		str_copy(Config.m_aWebDir, pWebDir, sizeof(Config.m_aWebDir));
	if(pBindAddr)
		str_copy(Config.m_aBindAddr, pBindAddr, sizeof(Config.m_aBindAddr));

	if(Config.m_aWebDir[strlen(Config.m_aWebDir)-1] != '/')
		str_append(Config.m_aWebDir, "/", sizeof(Config.m_aWebDir));
	if(!fs_is_dir(Config.m_aWebDir))
	{
		dbg_msg("main", "ERROR: Can't find web directory: %s", Config.m_aWebDir);
		return 1;
	}

	char aTmp[1024];
	str_format(aTmp, sizeof(aTmp), "%s%s", Config.m_aWebDir, Config.m_aJSONFile);
	str_copy(Config.m_aJSONFile, aTmp, sizeof(Config.m_aJSONFile));

	CMain Main(Config);
	RetVal = Main.Run();

	return RetVal;
}
