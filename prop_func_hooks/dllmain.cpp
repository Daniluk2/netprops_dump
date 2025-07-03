#include <dbg.h>
#include <Windows.h>
#include <dt_recv.h>
#include <dt_send.h>
#include <bitbuf.h>
#include <strtools.h>
#include <vector.h>
#include "icvar.h"
#include "eiface.h"

#ifdef GetProp
#undef GetProp
#endif
#ifdef CopyFile
#undef CopyFile
#endif

#define GET_FACTORY_AND_CHECK(dst_var, mod_name_string)\
  do {\
    dst_var = Sys_GetFactory(mod_name_string);\
    if(!dst_var) {\
      con_print("failed to get " mod_name_string " factory");\
      return FALSE;\
    }\
  } while (0);

#define GET_INTERFACE_AND_CHECK(dst_var, factory, type, version, return_val)\
  do {\
    dst_var = (type *)factory(version, nullptr);\
    if(!dst_var) {\
      con_print("interface with version " version " not found!");\
      return return_val;\
    }\
  } while (0);

void con_print(const char* p_format, ...)
{
	static char buf[1024];
	va_list argptr;
	va_start(argptr, p_format);
	vsnprintf_s(buf, sizeof(buf), p_format, argptr);

	Msg("%s\n", buf);
	printf("%s\n", buf);
}

class DecodeInfo : public CRecvProxyData
{
public:
	void* m_pStruct;			// Points at the base structure
	void* m_pData;			// Points at where the variable should be encoded. 

	const SendProp* m_pProp;		// Provides the client's info on how to decode and its proxy.
	bf_read* m_pIn;			// The buffer to get the encoded data from.
	char			m_TempStr[DT_MAX_STRING_BUFFERSIZE];	// m_Value.m_pString is set to point to this.
};

ICvar* p_cvar = nullptr;
IVEngineServer* pengine_server = nullptr;

/**
* init_cvars
*/
bool init_cvars(CreateInterfaceFn p_engine_factory)
{
#ifdef CSS_61
	GET_INTERFACE_AND_CHECK(p_cvar, p_engine_factory, ICvar, "VEngineCvar004", false)
#else
	GET_INTERFACE_AND_CHECK(p_cvar, p_engine_factory, ICvar, VENGINE_CVAR_INTERFACE_VERSION, false)
#endif
		class convar_accessor : public IConCommandBaseAccessor
	{
	public:
		convar_accessor() {}
		~convar_accessor() {}

		virtual bool RegisterConCommandBase(ConCommandBase* pCommand) {
			pCommand->AddFlags(FCVAR_PLUGIN);
			pCommand->SetNext(0);
			p_cvar->RegisterConCommandBase(pCommand);
			return true;
		}
	} cvar_accessor;
	ConCommandBaseMgr::OneTimeInit(&cvar_accessor);
	return true;
}

typedef struct
{
	// Encode a value.
	// pStruct : points at the base structure
	// pVar    : holds data in the correct type (ie: PropVirtualsInt will have DVariant::m_Int set).
	// pProp   : describes the property to be encoded.
	// pOut    : the buffer to encode into.
	// objectID: for debug output.
	void			(*Encode)(const unsigned char* pStruct, DVariant* pVar, const SendProp* pProp, bf_write* pOut, int objectID);

	// Decode a value.
	// See the DecodeInfo class for a description of the parameters.
	void			(*Decode)(DecodeInfo* pInfo);

	// Compare the deltas in the two buffers. The property in both buffers must be fully decoded
	int				(*CompareDeltas)(const SendProp* pProp, bf_read* p1, bf_read* p2);

	// Used for the local single-player connection to copy the data straight from the server ent into the client ent.
	void			(*FastCopy)(
		const SendProp* pSendProp,
		const RecvProp* pRecvProp,
		const unsigned char* pSendData,
		unsigned char* pRecvData,
		int objectID);

	// Return a string with the name of the type ("DPT_Float", "DPT_Int", etc).
	const char* (*GetTypeNameString)();

	// Returns true if the property's value is zero.
	// NOTE: this does NOT strictly mean that it would encode to zeros. If it were a float with
	// min and max values, a value of zero could encode to some other integer value.
	bool			(*IsZero)(const unsigned char* pStruct, DVariant* pVar, const SendProp* pProp);

	// This writes a zero value in (ie: a value that would make IsZero return true).
	void			(*DecodeZero)(DecodeInfo* pInfo);

	// This reades this property from stream p and returns true, if it's a zero value
	bool			(*IsEncodedZero) (const SendProp* pProp, bf_read* p);
	void			(*SkipProp) (const SendProp* pProp, bf_read* p);
} PropTypeFns;

uint8_t* wait_module(const char *pfilename) {
	uint8_t* pbase;
	while (!(pbase = (uint8_t*)GetModuleHandleA(pfilename)))
		Sleep(100);

	return pbase;
}

FILE* fp = nullptr;
PropTypeFns* g_PropTypeFns = nullptr;
FILE*  p_conout = nullptr;
size_t target_strlen=0;
char   target_name[128]="";

ConVar dt_decode_verbose("dt_decode_verbose", "0", 0, "verbose DT decode",
	[](ConVar *pcvar, const char *newvalue) {
		char buf[128];
		/* open new log file */
		if (pcvar->GetBool()) {
			if (fp)
				fclose(fp);

			Q_snprintf(buf, sizeof(buf), "dt_decode_verbose_%d.txt", Plat_MSTime());
			fopen_s(&fp, buf, "wb");
			if (!fp) {
				Warning("Failed to create \"%s\" log file!\n", buf);
				return;
			}
			Msg("Logh dt_decode_verbose ioeneod!\n");
			return;
		}
		else if (fp) {
			/* close log file */
			fclose(fp);
			fp = nullptr;
			Msg("Log dt_decode_verbose closed!Q\n");
		}
	}
);

CON_COMMAND(dt_decode_verbose_filter, "dt_decode_verbose_filter <propname>")
{
	if (pengine_server->Cmd_Argc() < 2) {
		Msg("Usage: dt_decode_verbose_filter <propname>\n");
		return;
	}

	const char *parg = pengine_server->Cmd_Argv(1);
	if (!parg) {
		Warning("empty arg!\n");
		return;
	}

	if (parg[0]) {
		Q_strncpy(target_name, parg, sizeof(target_name));
		target_strlen = Q_strlen(target_name);
		Msg("filter: \"%s\"\n", target_name);
	}
	else {
		target_name[0] = 0;
		target_strlen = 0;
		Msg("filter removed\n");
	}
}

bool is_prop_in_filter(const SendProp *pprop)
{
	const char* pname = pprop->GetName();
	/* checking filter */
	if (target_strlen && Q_strncmp(pname, target_name, target_strlen) != 0)
		return false;

	/* always logging */
	return true;
}

/* HOOK */
void (*pInt_Encode_original)(const unsigned char* pStruct, DVariant* pVar, const SendProp* pProp, bf_write* pOut, int objectID) = nullptr;
void (*pInt_Decode_original)(DecodeInfo* pInfo) = nullptr;

void Int_Encode_Proxy(const unsigned char* pStruct, DVariant* pVar, const SendProp* pProp, bf_write* pOut, int objectID)
{
	pInt_Encode_original(pStruct, pVar, pProp, pOut, objectID);
	//Msg("Int_Encode_Proxy(): called!\n");
}

void Int_Decode_Proxy(DecodeInfo* pInfo)
{
	const char* pname = pInfo->m_pProp->GetName();
	int last_value = *(int*)((char*)pInfo->m_pStruct + pInfo->m_pProp->GetOffset());
	pInt_Decode_original(pInfo);
	if (!is_prop_in_filter(pInfo->m_pProp))
		return;

	fprintf(fp, "int decode \"%s\" last: %d  new: %d  objid: %d\n",
		pname,
		last_value,
		pInfo->m_Value.m_Int,
		pInfo->m_ObjectID
		);
}

void (*pFloat_Decode_original)(DecodeInfo* pInfo) = nullptr;

void Float_Decode_Proxy(DecodeInfo* pInfo)
{
	const char* pname = pInfo->m_pProp->GetName();
	float last_value = *(float*)((char*)pInfo->m_pStruct + pInfo->m_pProp->GetOffset());
	pFloat_Decode_original(pInfo);
	if (!is_prop_in_filter(pInfo->m_pProp))
		return;

	fprintf(fp, "float decode \"%s\" last: %.3f  new: %.3f  objid: %d\n",
		pname,
		last_value,
		pInfo->m_Value.m_Float,
		pInfo->m_ObjectID
	);
}

void (*pVector_Decode_original)(DecodeInfo* pInfo) = nullptr;

void Vector_Decode_Proxy(DecodeInfo* pInfo)
{
	const char* pname = pInfo->m_pProp->GetName();
	Vector last_value = *(Vector*)((char*)pInfo->m_pStruct + pInfo->m_pProp->GetOffset());
	pVector_Decode_original(pInfo);
	Vector curr_value = *(Vector *)pInfo->m_Value.m_Vector;
	if (!is_prop_in_filter(pInfo->m_pProp))
		return;

	fprintf(fp, "vector decode \"%s\" last: ( %.3f %.3f %.3f )  new: ( %.3f %.3f %.3f )  objid: %d\n",
		pname,
		last_value.x, last_value.y, last_value.z,
		curr_value.x, curr_value.y, curr_value.z,
		pInfo->m_ObjectID
	);
}

void (*pArray_Decode_original)(DecodeInfo* pInfo) = nullptr;

void Array_Decode_Proxy(DecodeInfo* pInfo)
{
	pArray_Decode_original(pInfo);
	//const char* pname = pInfo->m_pProp->GetName();
	//Msg("Float_Decode_Proxy(): \"%s\" changed\n");
}

BOOL initialize(HMODULE hModule)
{
	CreateInterfaceFn p_engine_factory;
	//CreateInterfaceFn p_server_factory;
	//CreateInterfaceFn p_client_factory;

	if (AllocConsole()) {
		freopen_s(&p_conout, "conout$", "w", stdout);
	}

	/* query factories */
	GET_FACTORY_AND_CHECK(p_engine_factory, "engine.dll")
	//GET_FACTORY_AND_CHECK(p_client_factory, "client.dll")
	//GET_FACTORY_AND_CHECK(p_server_factory, "server.dll")

#ifdef CSS_61
	GET_INTERFACE_AND_CHECK(pengine_server, p_engine_factory, IVEngineServer, INTERFACEVERSION_VENGINESERVER, FALSE)
#else
	GET_INTERFACE_AND_CHECK(pengine_server, p_engine_factory, IVEngineServer, INTERFACEVERSION_VENGINESERVER, FALSE)
#endif
	
	/* init concommands */
	if (!init_cvars(p_engine_factory)) {
		con_print("init_cvars() failed");
		return FALSE;
	}

	Msg("initialize(): initializing hooks\n");
	constexpr size_t engine_img_base = 0x20000000;
	constexpr size_t target_offset = 0x20368388 - engine_img_base; //.data:20368388 - imagebase:20000000 (address of g_PropTypeFns)
	constexpr size_t encoode_func_addr = 0x2009B2D0 - engine_img_base; //.text:2009B2D0 standard Encode function address rel image base in v34 4044 engine.dll
	uint8_t* penginebase = wait_module("engine.dll");
	g_PropTypeFns = (PropTypeFns*)(penginebase + target_offset);

	static char buf[512];
	Q_snprintf(buf, sizeof(buf), "decode_logs_%u.txt", Plat_MSTime());
	fopen_s(&fp, buf, "wb");
	if (!fp) {
		Warning("Log file creation failed!\n");
		return FALSE;
	}

	fprintf(fp, "Begin of DT decode log\n");

	Msg("change int decode address...\n");
	PropTypeFns* pintfns = &g_PropTypeFns[DPT_Int];
	assert(pintfns->Encode == encoode_func_addr && "pintfns->Encode not match function address! This is v34 4044 build?"); //K.D. CHECKING!
	pInt_Encode_original = pintfns->Encode;
	pintfns->Encode = Int_Encode_Proxy;
	pInt_Decode_original = pintfns->Decode;
	pintfns->Decode = Int_Decode_Proxy;

	PropTypeFns* pfloatfns = &g_PropTypeFns[DPT_Float];
	pFloat_Decode_original = pfloatfns->Decode;
	pfloatfns->Decode = Float_Decode_Proxy;

	PropTypeFns* pvectorfns = &g_PropTypeFns[DPT_Vector];
	pVector_Decode_original = pvectorfns->Decode;
	pvectorfns->Decode = Vector_Decode_Proxy;

	//PropTypeFns* pstringfns = &g_PropTypeFns[DPT_String];

	PropTypeFns* parrayfns = &g_PropTypeFns[DPT_Array];
	pArray_Decode_original = parrayfns->Decode;
	parrayfns->Decode = Array_Decode_Proxy;

	return TRUE;
}

BOOL deinitialize(HMODULE hModule)
{
	g_PropTypeFns[DPT_Int].Encode = pInt_Encode_original;
	g_PropTypeFns[DPT_Int].Decode = pInt_Decode_original;
	g_PropTypeFns[DPT_Float].Decode = pFloat_Decode_original;
	g_PropTypeFns[DPT_Vector].Decode = pVector_Decode_original;
	g_PropTypeFns[DPT_Array].Decode = pArray_Decode_original;
	if (fp) {
		fprintf(fp, "End of DT decode log\n");
		fclose(fp);
		fp = nullptr;
	}

	if (p_conout) {
		fclose(p_conout);
		FreeConsole();
	}
	return TRUE;
}

BOOL APIENTRY DllMain( HMODULE hModule, DWORD reason, LPVOID lpReserved )
{
	DisableThreadLibraryCalls(hModule);
  switch (reason) {
	case DLL_PROCESS_ATTACH:
		return initialize(hModule);
  case DLL_PROCESS_DETACH:
		return deinitialize(hModule);
  }
  return TRUE;
}
