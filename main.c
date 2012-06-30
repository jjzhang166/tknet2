/*
*      This file is part of the tknet project. 
*    which be used under the terms of the GNU General Public 
*    License version 3.0 as published by the Free Software
*    Foundation and appearing in the file LICENSE.GPL included 
*    in the packaging of this file.  Please review the following 
*    information to ensure the GNU General Public License 
*    version 3.0 requirements will be met: 
*    http://www.gnu.org/copyleft/gpl.html
*
*    Copyright  (C)   2012   Zhong Wei <clock126@126.com>  .
*/ 

#include "tknet.h"

BOOL             g_MainLoopFlag = 1;
BOOL             g_ifConfigAsFullCone = 0;
uchar            g_NATtype = NAT_T_UNKNOWN;
struct NetAddr   g_BdgPeerAddr;
char             g_TargetName[PEER_NAME_ID_LEN];
char             *g_pTargetName = NULL;
char             g_MyName[PEER_NAME_ID_LEN];

static
FLOW_CALLBK_FUNCTION( StdoutFlowCallbk )
{
	printf("%s",pa_pData);
}

TK_THREAD( StdinThread )
{
 	static char buff[BKGD_CMD_MAX_LEN];
	DEF_AND_CAST(pPipe,struct pipe,pa_else);
	
	while(g_MainLoopFlag)
	{
		fgets(buff,BKGD_CMD_MAX_LEN,stdin);
		PipeFlow(pPipe,buff,strlen(buff) + 1,NULL);
	}

	return NULL;
}

void
tkNetDefaultPipeInit()
{
	struct pipe *pPipe;
	
	pPipe = PipeMap("null");
	
	pPipe = PipeMap("stdout");
	pPipe->FlowCallbk = &StdoutFlowCallbk;
	
	pPipe = PipeMap("stdin");
	tkBeginThread( &StdinThread , pPipe );
}

void 
tkNetCommonInit()
{
	g_ConnectionNotify = &OnConnect;
	tkInitRandom();
	tkLogInit();
	SockInit();
	ProcessSetCondition(1);
	PipeModuleInit();
	tkNetDefaultPipeInit();

	g_TargetName[0]='\0';
	g_MyName[0]='\0';
}

void 
tkNetCommonUninit()
{
	PipeModuleUninit();
	SockDestory();
	tkLogClose();
}

void 
tkNetConnect(const char *pa_pName)
{
	if( pa_pName != NULL )
		strcpy( g_TargetName , pa_pName );
	
	g_pTargetName = g_TargetName;
}

int 
tkNetMain(int pa_argn,char **in_args)
{
	struct KeyInfoCache        KeyInfoCache;
	struct ProcessingList      ProcList;
	struct BackGroundArgs      BkgdArgs;
	struct PeerData            PeerDataRoot;
	struct Iterator            ISeedPeer;
	struct Sock                MainSock;
	struct BridgeProc          BdgServerProc;
	struct BridgeProc          BdgClientProc;
	char                       BdgPeerAddrStr[32];
	BOOL                       ifClientSkipRegister = 1;
	int                        TestPurposeNatType;
	struct BridgeClientProcPa  *pBCPPa = NULL;

	printf("tknet \n build: " TKNET_VER "\n");

	tkNetCommonInit();
	MutexInit(&g_BkgdMutex);

	ISeedPeer = GetIterator(NULL);

	PeerDataCons(&PeerDataRoot);
	PeerDataRoot.tpnd.RanPriority = 0;
	PeerDataRoot.addr.port = 0;
	PeerDataRoot.addr.IPv4 = 0;

	ProcessingListCons( &ProcList );

	RelayModuleInit();

	KeyInfoCacheCons(&KeyInfoCache);
	if(!KeyInfoReadFile(&KeyInfoCache,"tknet.info"))
	{
		printf("config file lost.\n");
		goto exit;
	}

	if(!KeyInfoTry(&KeyInfoCache,KEY_INFO_TYPE_CONFIG,&MainSock))
	{
		printf("bad config format.\n");
		goto exit;
	}
	
	if( g_TargetName[0] != '\0' )
	{
		printf("target name: %s \n", g_TargetName);
		tkNetConnect(NULL);
	}
	else
	{
		printf("target name unset. \n");
	}

	if(g_ifConfigAsFullCone)
	{
		g_NATtype = NAT_T_FULL_CONE;
		printf("config NAT type as fullcone.\n");
	}
	else
	{
		while(!KeyInfoTry(&KeyInfoCache,KEY_INFO_TYPE_STUNSERVER,&MainSock))
		{
			if(!KeyInfoTry(&KeyInfoCache,KEY_INFO_TYPE_MAILSERVER,&MainSock))
			{
				printf("No way to get NAT type.\n");
				goto exit;
			}
		}
		
		printf("NAT type got from STUN.\n");
	}

	if(pa_argn == 2)
	{
		sscanf(in_args[1],"%d",&TestPurposeNatType);
		g_NATtype = (uchar)TestPurposeNatType;
		
		printf("NAT type assigned by argument.\n");
	}
		
	NatTypePrint(g_NATtype);

	while(!KeyInfoTry(&KeyInfoCache,KEY_INFO_TYPE_BRIDGEPEER,&MainSock))
	{
		if(!KeyInfoTry(&KeyInfoCache,KEY_INFO_TYPE_MAILSERVER,&MainSock))
		{
			printf("no avalible Bridge peer.\n");
			goto no_bdg_peer;
		}
	}

	GetAddrText(&g_BdgPeerAddr,BdgPeerAddrStr);
	printf("using Bridge peer: %s\n",BdgPeerAddrStr);
	ifClientSkipRegister = 0;

no_bdg_peer:

	pBCPPa = BridgeMakeClientProc(&BdgClientProc,&MainSock,&ProcList,&g_BdgPeerAddr,
			g_MyName,g_NATtype,&g_pTargetName,ifClientSkipRegister);
	ProcessStart(&BdgClientProc.proc,&ProcList);

	if(g_ifBkgdEnable)
		printf("back ground enabled.\n");
	else
		printf("back ground disabled.\n");

	BkgdArgs.pPeerDataRoot = &PeerDataRoot;
	BkgdArgs.pInfoCache = &KeyInfoCache;
	BkgdArgs.pProcList = &ProcList;
	BkgdArgs.pBdgClientProc = &BdgClientProc;
	BkgdArgs.pMainSock = &MainSock;
	tkBeginThread( &BackGround , &BkgdArgs );

	ConsAndStartBridgeServer(&BdgServerProc,&PeerDataRoot,&ProcList,&MainSock,&ISeedPeer);

	while( g_MainLoopFlag )
	{
		MutexLock(&g_BkgdMutex);

		if(!ifBkgdStunProc())
			SockRead(&MainSock);

		DoProcessing( &ProcList );

		if(!ifBkgdStunProc())
			MainSock.RecvLen = 0;

		MutexUnlock(&g_BkgdMutex);

		tkMsSleep(50);
	}

	ProcessDisattach(&BdgClientProc.proc,&ProcList);
	FreeBdgClientProc(&BdgClientProc);

	ProcessDisattach(&BdgServerProc.proc,&ProcList);
	FreeBridgeServer(&BdgServerProc);

	ProcessListFree(&ProcList);
	FreeSubBridgeServerTemplate();

	SockClose(&MainSock);

exit:

	PeerDataDestroy(&PeerDataRoot,&ISeedPeer);
	KeyInfoUpdate( &KeyInfoCache );
	KeyInfoWriteFile(&KeyInfoCache,"tknet.info");
	KeyInfoFree(&KeyInfoCache);
	RelayMuduleDestruction();
	MutexDelete(&g_BkgdMutex);
	tkNetCommonUninit();

	printf("unfree memory:%d \n",g_allocs);
	tkMsSleep(200);//waiting for threads

	return 0;
}
