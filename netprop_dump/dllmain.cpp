//https://wiki.alliedmods.net/Entity_properties

#include "icvar.h"
#include "eiface.h"
#include "cdll_int.h"
#include "server_class.h"
#include <time.h>

#include <Windows.h>
#ifdef GetProp
#undef GetProp
#endif
#ifdef CopyFile
#undef CopyFile
#endif

#define CSS_61

#if defined(_WIN32)
inline FILE* fopen_internal(const char* p_filename, const char* p_mode)
{
  FILE* fp;
  fopen_s(&fp, p_filename, p_mode);
  return fp;
}
#define fopen(f, m) fopen_internal(f, m)
#endif

void con_print(const char* p_format, ...)
{
  static char buf[1024];
  va_list argptr;
  va_start(argptr, p_format);
  vsnprintf_s(buf, sizeof(buf), p_format, argptr);
  
  Msg("%s\n", buf);
  printf("%s\n", buf);
}

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

HMODULE h_this_dll = nullptr;
FILE* p_conout = nullptr;
ICvar* p_cvar = nullptr;
IServerGameDLL* p_server_gamedll = nullptr;
IVEngineClient* p_engine_client = nullptr;
IVEngineServer* p_engine_server = nullptr;

/**
* wait_for_module_loading
*/
void wait_for_module_loading(LPCWSTR p_modname, DWORD recheck_ms = 100)
{
  while (!GetModuleHandleW(p_modname))
    Sleep(recheck_ms);
}

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

BOOL init()
{
  CreateInterfaceFn p_engine_factory;
  CreateInterfaceFn p_server_factory;
  CreateInterfaceFn p_client_factory;

  if (AllocConsole()) {
    freopen_s(&p_conout, "conout$", "w", stdout);
  }

  con_print("waiting for loading client.dll...");
  wait_for_module_loading(L"client.dll");

  /* query factories */
  GET_FACTORY_AND_CHECK(p_engine_factory, "engine.dll")
  GET_FACTORY_AND_CHECK(p_client_factory, "client.dll")
  GET_FACTORY_AND_CHECK(p_server_factory, "server.dll")

  /* init concommands */
  if (!init_cvars(p_engine_factory)) {
    con_print("init_cvars() failed");
    return FALSE;
  }

  /* get IServerGameDLL */
#ifdef CSS_61
  GET_INTERFACE_AND_CHECK(p_server_gamedll, p_server_factory, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL, FALSE)
  GET_INTERFACE_AND_CHECK(p_engine_server, p_engine_factory, IVEngineServer, INTERFACEVERSION_VENGINESERVER, FALSE)
  GET_INTERFACE_AND_CHECK(p_engine_client, p_engine_factory, IVEngineClient, "VEngineClient013", FALSE)
#else
  GET_INTERFACE_AND_CHECK(p_server_gamedll, p_server_factory, IServerGameDLL, "ServerGameDLL006", FALSE)
  GET_INTERFACE_AND_CHECK(p_engine_server, p_engine_factory, IVEngineServer, INTERFACEVERSION_VENGINESERVER, FALSE)
  GET_INTERFACE_AND_CHECK(p_engine_client, p_engine_factory, IVEngineClient, VENGINE_CLIENT_INTERFACE_VERSION, FALSE)
#endif

  con_print("dumper loaded");
  return TRUE;
}
#undef GET_FACTORY_AND_CHECK

void shutdown()
{
  con_print("unloading dumper");
  if (p_conout) {
    fclose(p_conout);
    FreeConsole();
  }
}

char* UTIL_SendFlagsToString(int flags, int type)
{
  static char str[1024];
  str[0] = 0;

  if (flags & SPROP_UNSIGNED)
  {
    strcat(str, "Unsigned|");
  }
  if (flags & SPROP_COORD)
  {
    strcat(str, "Coord|");
  }
  if (flags & SPROP_NOSCALE)
  {
    strcat(str, "NoScale|");
  }
  if (flags & SPROP_ROUNDDOWN)
  {
    strcat(str, "RoundDown|");
  }
  if (flags & SPROP_ROUNDUP)
  {
    strcat(str, "RoundUp|");
  }
  if (flags & SPROP_NORMAL)
  {
    if (type == DPT_Int)
    {
      strcat(str, "VarInt|");
    }
    else
    {
      strcat(str, "Normal|");
    }
  }
  if (flags & SPROP_EXCLUDE)
  {
    strcat(str, "Exclude|");
  }
  if (flags & SPROP_XYZE)
  {
    strcat(str, "XYZE|");
  }
  if (flags & SPROP_INSIDEARRAY)
  {
    strcat(str, "InsideArray|");
  }
  if (flags & SPROP_PROXY_ALWAYS_YES)
  {
    strcat(str, "AlwaysProxy|");
  }
  if (flags & SPROP_CHANGES_OFTEN)
  {
    strcat(str, "ChangesOften|");
  }
  if (flags & SPROP_IS_A_VECTOR_ELEM)
  {
    strcat(str, "VectorElem|");
  }
  if (flags & SPROP_COLLAPSIBLE)
  {
    strcat(str, "Collapsible|");
  }
#if 0
  if (flags & SPROP_COORD_MP)
  {
    strcat(str, "CoordMP|");
  }
  if (flags & SPROP_COORD_MP_LOWPRECISION)
  {
    strcat(str, "CoordMPLowPrec|");
  }
  if (flags & SPROP_COORD_MP_INTEGRAL)
  {
    strcat(str, "CoordMpIntegral|");
  }
#endif

  int len = strlen(str) - 1;
  if (len > 0)
  {
    str[len] = 0; // Strip the final '|'
  }
  return str;
}

const char* GetDTTypeName(int type)
{
  switch (type)
  {
  case DPT_Int:
  {
    return "integer";
  }
  case DPT_Float:
  {
    return "float";
  }
  case DPT_Vector:
  {
    return "vector";
  }
  case DPT_String:
  {
    return "string";
  }
  case DPT_Array:
  {
    return "array";
  }
  case DPT_DataTable:
  {
    return "datatable";
  }
#if 0
  case DPT_Int64:
  {
    return "int64";
  }
#endif
  default:
  {
    return nullptr;
  }
  }
  return nullptr;
}

void UTIL_DrawSendTable_XML(FILE* fp, SendTable* pTable, int space_count)
{
  char spaces[255];

  for (int i = 0; i < space_count; i++)
  {
    spaces[i] = ' ';
  }
  spaces[space_count] = '\0';

  const char* type_name;
  SendTable* pOtherTable;
  SendProp* pProp;

  fprintf(fp, " %s<sendtable name='%s'>\n", spaces, pTable->GetName());
  for (int i = 0; i < pTable->GetNumProps(); i++)
  {
    pProp = pTable->GetProp(i);

    fprintf(fp, "  %s<property name='%s'>\n", spaces, pProp->GetName());

    if ((type_name = GetDTTypeName(pProp->GetType())) != NULL)
    {
      fprintf(fp, "   %s<type>%s</type>\n", spaces, type_name);
    }
    else
    {
      fprintf(fp, "   %s<type>%d</type>\n", spaces, pProp->GetType());
    }

    fprintf(fp, "   %s<offset>%d</offset>\n", spaces, pProp->GetOffset());
    fprintf(fp, "   %s<bits>%d</bits>\n", spaces, pProp->m_nBits);
    fprintf(fp, "   %s<flags>%s</flags>\n", spaces, UTIL_SendFlagsToString(pProp->GetFlags(), pProp->GetType()));

    if ((pOtherTable = pTable->GetProp(i)->GetDataTable()) != NULL)
    {
      UTIL_DrawSendTable_XML(fp, pOtherTable, space_count + 3);
    }

    fprintf(fp, "  %s</property>\n", spaces);
  }
  fprintf(fp, " %s</sendtable>\n", spaces);
}

void UTIL_DrawServerClass_XML(FILE* fp, ServerClass* sc)
{
  fprintf(fp, " <serverclass name='%s'>\n", sc->GetName());
  UTIL_DrawSendTable_XML(fp, sc->m_pTable, 1);
  fprintf(fp, " </serverclass>\n");
}

CON_COMMAND(netprops_dump_xml, "")
{
  const char *p_filename;
  FILE       *fp = nullptr;
  if (p_engine_server->Cmd_Argc() < 1) {
    con_print("use: netprops_dump_xml <filename.xml>\n");
    return;
  }

  /* perform dumping */
  p_filename = p_engine_server->Cmd_Argv(1);
  fp = fopen(p_filename, "wt"); 
  if (!fp) {
    con_print("Could not open file \"%s\"\n", p_filename);
    return;
  }

  char buffer[80];
  buffer[0] = 0;
  time_t nowtime = time(nullptr);
  strftime(buffer, sizeof(buffer), "%Y/%m/%d", localtime(&nowtime));
  fprintf(fp, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n");
  fprintf(fp, "<!-- Dump of all network properties for \"%s\" as at %s -->\n\n", p_engine_client->GetGameDirectory(), buffer);
  fprintf(fp, "<netprops>\n");

  ServerClass* pBase = p_server_gamedll->GetAllServerClasses();
  while (pBase != nullptr) {
    UTIL_DrawServerClass_XML(fp, pBase);
    pBase = pBase->m_pNext;
  }
  fprintf(fp, "</netprops>\n");
  fclose(fp);
}

//CON_COMMAND(netprops_dump_unload, "")
//{
//  if (h_this_dll) {
//    FreeLibrary(h_this_dll);
//  }
//}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
  DisableThreadLibraryCalls(hModule);
  switch (reason)
  {
  case DLL_PROCESS_ATTACH:
    h_this_dll = hModule;
    return init();

  case DLL_PROCESS_DETACH:
      break;
  }
  return TRUE;
}

