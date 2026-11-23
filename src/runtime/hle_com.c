/* hle_com.c -- COM objects the game creates through CoCreateInstance, implemented in the runtime.
 *
 * An object is guest memory {vtable, id}; its vtable holds import thunks of the methods here (the names
 * "Interface::Method" in the "com" pseudo-DLL), so a guest call through the vtable lands in the HLE function like any
 * import. Methods are stdcall with `this` as argument 0. Reference counts are not kept (objects live forever).
 *
 * DxDiag (CLSID_DxDiagProvider): the game reads system information at boot (memory, CPU, DirectX version, the
 * display adapter) and refuses to start when it cannot. The tree is a fixed, plausible machine; XRC_DXDIAG_MEM_MB
 * overrides the physical memory it reports. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xrc_rt.h"
#include "guest_types.h"

#define A(i) xrc_arg(c, (i))
#define RET(v, n) do { c->eax = (u32)(v); xrc_stdcall_ret(c, (n)); return; } while (0)
#define W16(a) ((u16 *)(xrc_mem + (u32)(a)))

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_INVALIDARG 0x80070057u
#define REGDB_E_CLASSNOTREG 0x80040154u

#define VT_I4 3
#define VT_BSTR 8
#define VT_BOOL 11
#define VT_UI4 19

static void guid_str(u32 g, char *out, size_t n) {
	snprintf(out, n, "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", GF(Guid, g, data1), GF(Guid, g, data2), GF(Guid, g, data3), GFI(Guid, g, data4, 0), GFI(Guid, g, data4, 1),
	         GFI(Guid, g, data4, 2), GFI(Guid, g, data4, 3), GFI(Guid, g, data4, 4), GFI(Guid, g, data4, 5), GFI(Guid, g, data4, 6), GFI(Guid, g, data4, 7));
}

/* a guest UTF-16 string as ASCII (names here are ASCII) */
static void wstr(u32 p, char *out, size_t n) {
	size_t k = 0;
	if (p)
		for (; k + 1 < n && W16(p)[k]; k++) out[k] = (char)W16(p)[k];
	out[k] = 0;
}
/* copies an ASCII string into a guest UTF-16 buffer of cch characters */
static int put_wstr(u32 buf, u32 cch, const char *s) {
	u32 n = (u32)strlen(s), k;
	if (!buf || n + 1 > cch) return 0;
	for (k = 0; k <= n; k++) W16(buf)[k] = (u8)s[k];
	return 1;
}
u32 xrc_bstr_from(const char *s) {
	u32 n = (u32)strlen(s), p = xrc_heap_alloc(4 + 2 * n + 2, 0), k;
	if (!p) return 0;
	M32(p) = 2 * n;
	for (k = 0; k <= n; k++) M16(p + 4 + 2 * k) = (u8)s[k];
	return p + 4;
}

/* a new object: the vtable of the named methods (import thunks in the "com" DLL), and {vtable, id} */
static u32 com_new(const char *const *methods, int n, u32 id) {
	u32 vt = xrc_heap_alloc(4u * (u32)n, 1), o = xrc_heap_alloc(16, 1);
	int k;
	for (k = 0; k < n; k++) {
		u32 a = xrc_import_addr("com", methods[k]);
		if (!a) xrc_fatal(0, "hle_com: no method %s", methods[k]);
		M32(vt + 4u * (u32)k) = a;
	}
	GF(ComObject, o, vtable) = vt;
	GF(ComObject, o, id) = id;
	return o;
}

/* ============================================================================================ DxDiag */
typedef struct { const char *name; int vt; const char *s; u32 u; } DxProp;
typedef struct { const char *name; int parent; const DxProp *props; } DxNode;

static char mem_str[64];
static const DxProp sys_props[] = {
	{"szOSExEnglish", VT_BSTR, "Windows 7 Ultimate 64-bit (6.1, Build 7601) Service Pack 1 (7601.win7sp1_rtm.101119-1850)", 0},
	{"szOSEnglish", VT_BSTR, "Windows 7 Ultimate 64-bit (6.1, Build 7601) Service Pack 1", 0},
	{"szPhysicalMemoryEnglish", VT_BSTR, mem_str, 0},
	{"szPageFileEnglish", VT_BSTR, "2048MB used, 14336MB available", 0},
	{"szProcessorEnglish", VT_BSTR, "Intel(R) Core(TM) i7 CPU 920 @ 2.67GHz (8 CPUs), ~2.7GHz", 0},
	{"dwDirectXVersionMajor", VT_UI4, 0, 9},
	{"dwDirectXVersionMinor", VT_UI4, 0, 0},
	{"szDirectXVersionLetter", VT_BSTR, "c", 0},
	{"szDirectXVersionEnglish", VT_BSTR, "4.09.0000.0904", 0},
	{"szDirectXVersionLongEnglish", VT_BSTR, "DirectX 11", 0},
	{"bIsD3DDebugRuntime", VT_BOOL, 0, 0},
	{"szLanguagesEnglish", VT_BSTR, "English (Regional Setting: English)", 0},
	{"szSystemManufacturerEnglish", VT_BSTR, "System manufacturer", 0},
	{"szSystemModelEnglish", VT_BSTR, "System Product Name", 0},
	{0, 0, 0, 0},
};
static const DxProp disp_props[] = {
	{"szDescription", VT_BSTR, "NVIDIA GeForce GTX 560", 0},
	{"szChipType", VT_BSTR, "GeForce GTX 560", 0},
	{"szDisplayMemoryEnglish", VT_BSTR, "1024 MB", 0},
	{"szDisplayMemoryLocalized", VT_BSTR, "1024 MB", 0},
	{"szManufacturer", VT_BSTR, "NVIDIA", 0},
	{"szDriverVersion", VT_BSTR, "9.18.13.4052", 0},
	{"szDisplayModeEnglish", VT_BSTR, "1920 x 1080 (32 bit) (60Hz)", 0},
	{"szVendorId", VT_BSTR, "0x10DE", 0},
	{"szDeviceId", VT_BSTR, "0x1201", 0},
	{"dwWidth", VT_UI4, 0, 1920},
	{"dwHeight", VT_UI4, 0, 1080},
	{"dwBpp", VT_UI4, 0, 32},
	{"dwRefreshRate", VT_UI4, 0, 60},
	{0, 0, 0, 0},
};
static const DxProp no_props[] = {{0, 0, 0, 0}};
static const DxNode dx_nodes[] = {
	/* 0 */ {"", -1, no_props},
	/* 1 */ {"DxDiag_SystemInfo", 0, sys_props},
	/* 2 */ {"DxDiag_DisplayDevices", 0, no_props},
	/* 3 */ {"0", 2, disp_props},
	/* 4 */ {"DxDiag_SoundDevices", 0, no_props},
	/* 5 */ {"DxDiag_DirectInput", 0, no_props},
};
#define NDX (int)(sizeof dx_nodes / sizeof dx_nodes[0])

static const char *const dxprov_m[] = {"IDxDiagProvider::QueryInterface", "IDxDiagProvider::AddRef",
                                       "IDxDiagProvider::Release", "IDxDiagProvider::Initialize",
                                       "IDxDiagProvider::GetRootContainer"};
static const char *const dxcont_m[] = {
	"IDxDiagContainer::QueryInterface", "IDxDiagContainer::AddRef", "IDxDiagContainer::Release",
	"IDxDiagContainer::GetNumberOfChildContainers", "IDxDiagContainer::EnumChildContainerNames",
	"IDxDiagContainer::GetChildContainer", "IDxDiagContainer::GetNumberOfProps", "IDxDiagContainer::EnumPropNames",
	"IDxDiagContainer::GetProp"};
static u32 dx_cont_obj[NDX];
static u32 dx_container(int id) {
	if (!dx_cont_obj[id]) dx_cont_obj[id] = com_new(dxcont_m, 9, (u32)id);
	return dx_cont_obj[id];
}
static int dx_child(int parent, int idx) {
	int k, i = 0;
	for (k = 0; k < NDX; k++)
		if (dx_nodes[k].parent == parent && i++ == idx) return k;
	return -1;
}
static int dx_find(int parent, const char *name) {
	char part[64];
	const char *dot = strchr(name, '.');
	size_t n = dot ? (size_t)(dot - name) : strlen(name);
	int k;
	if (n >= sizeof part) return -1;
	memcpy(part, name, n);
	part[n] = 0;
	for (k = 0; k < NDX; k++)
		if (dx_nodes[k].parent == parent && strcmp(dx_nodes[k].name, part) == 0) return dot ? dx_find(k, dot + 1) : k;
	return -1;
}
static int dx_id(XCtx *c) { u32 id = GF(ComObject, A(0), id); return id < (u32)NDX ? (int)id : 0; }

static void com_AddRef(XCtx *c) { RET(1, 1); }
static void com_Release(XCtx *c) { RET(1, 1); }
static void dxprov_QueryInterface(XCtx *c) {
	char iid[64];
	guid_str(A(1), iid, sizeof iid);
	if (strcmp(iid, "{9c6b4cb0-23f8-49cc-a3ed-45a55000a6d2}") == 0 || strcmp(iid, "{00000000-0000-0000-c000-000000000046}") == 0) {
		M32(A(2)) = A(0);
		RET(S_OK, 3);
	}
	M32(A(2)) = 0;
	RET(E_NOINTERFACE, 3);
}
static void dxprov_Initialize(XCtx *c) { RET(S_OK, 2); }
static void dxprov_GetRootContainer(XCtx *c) {
	if (!A(1)) RET(E_POINTER, 2);
	M32(A(1)) = dx_container(0);
	RET(S_OK, 2);
}
static void dxcont_QueryInterface(XCtx *c) { M32(A(2)) = A(0); RET(S_OK, 3); }
static void dxcont_GetNumberOfChildContainers(XCtx *c) {
	int id = dx_id(c), n = 0;
	while (dx_child(id, n) >= 0) n++;
	M32(A(1)) = (u32)n;
	RET(S_OK, 2);
}
static void dxcont_EnumChildContainerNames(XCtx *c) {
	int k = dx_child(dx_id(c), (int)A(1));
	if (k < 0) RET(E_INVALIDARG, 4);
	RET(put_wstr(A(2), A(3), dx_nodes[k].name) ? S_OK : E_INVALIDARG, 4);
}
static void dxcont_GetChildContainer(XCtx *c) {
	char name[128];
	int k;
	wstr(A(1), name, sizeof name);
	k = dx_find(dx_id(c), name);
	if (k < 0) {
		fprintf(stderr, "xrc: DxDiag: no container %s under %s\n", name, dx_nodes[dx_id(c)].name);
		M32(A(2)) = 0;
		RET(E_INVALIDARG, 3);
	}
	M32(A(2)) = dx_container(k);
	RET(S_OK, 3);
}
static void dxcont_GetNumberOfProps(XCtx *c) {
	const DxProp *p = dx_nodes[dx_id(c)].props;
	u32 n = 0;
	while (p[n].name) n++;
	M32(A(1)) = n;
	RET(S_OK, 2);
}
static void dxcont_EnumPropNames(XCtx *c) {
	const DxProp *p = dx_nodes[dx_id(c)].props;
	u32 n = 0;
	while (p[n].name && n < A(1)) n++;
	if (!p[n].name) RET(E_INVALIDARG, 4);
	RET(put_wstr(A(2), A(3), p[n].name) ? S_OK : E_INVALIDARG, 4);
}
static void dxcont_GetProp(XCtx *c) {
	char name[128];
	const DxProp *p = dx_nodes[dx_id(c)].props;
	u32 v = A(2);
	wstr(A(1), name, sizeof name);
	for (; p->name; p++)
		if (strcmp(p->name, name) == 0) break;
	if (!p->name) {
		fprintf(stderr, "xrc: DxDiag: no property %s in %s\n", name, dx_nodes[dx_id(c)].name);
		RET(E_INVALIDARG, 3);
	}
	memset(xrc_mem + v, 0, 16);
	GF(Variant, v, vt) = (u16)p->vt;
	if (p->vt == VT_BSTR) GF(Variant, v, bstr_val) = xrc_bstr_from(p->s);
	else if (p->vt == VT_BOOL) GF(Variant, v, u16_val) = p->u ? 0xffff : 0;
	else GF(Variant, v, u32_val) = p->u;
	RET(S_OK, 3);
}

/* ============================================================================================ CoCreateInstance */
/* 0 and *out set: created; REGDB_E_CLASSNOTREG: not a class the runtime has */
u32 xrc_com_create(u32 clsid, u32 iid, u32 out) {
	char cls[64];
	(void)iid;
	guid_str(clsid, cls, sizeof cls);
	if (strcmp(cls, "{a65b8071-3bfe-4213-9a5b-491da4461ca7}") == 0) { /* CLSID_DxDiagProvider */
		static u32 prov;
		if (!mem_str[0]) {
			const char *m = getenv("XRC_DXDIAG_MEM_MB");
			snprintf(mem_str, sizeof mem_str, "%sMB RAM", m && *m ? m : "8192");
		}
		if (!prov) prov = com_new(dxprov_m, 5, 0);
		M32(out) = prov;
		return S_OK;
	}
	return xrc_xaudio2_create(cls, out);
}

/* CoGetClassObject: an IClassFactory {vtable, the CLSID's address} whose CreateInstance is CoCreateInstance of it */
static const char *const factory_m[] = {"IClassFactory::QueryInterface", "IClassFactory::AddRef", "IClassFactory::Release",
                                        "IClassFactory::CreateInstance", "IClassFactory::LockServer"};
static int com_known(u32 clsid) {
	char cls[64];
	guid_str(clsid, cls, sizeof cls);
	return strcmp(cls, "{a65b8071-3bfe-4213-9a5b-491da4461ca7}") == 0 || xrc_xaudio2_create(cls, 0) == S_OK;
}
u32 xrc_com_class_object(u32 clsid, u32 out) {
	u32 o, g;
	if (!com_known(clsid)) return REGDB_E_CLASSNOTREG;
	g = xrc_heap_alloc(16, 0);
	memcpy(xrc_mem + g, xrc_mem + clsid, 16);
	o = com_new(factory_m, 5, g);
	M32(out) = o;
	return S_OK;
}
static void cf_QueryInterface(XCtx *c) { M32(A(2)) = A(0); RET(S_OK, 3); }
/* CreateInstance(this, outer, iid, out) */
static void cf_CreateInstance(XCtx *c) {
	if (A(1)) RET(0x80040110u /* CLASS_E_NOAGGREGATION */, 4);
	RET(xrc_com_create(GF(ComObject, A(0), id), A(2), A(3)), 4);
}
static void cf_LockServer(XCtx *c) { RET(S_OK, 2); }

#define M(i, f) {"com", i, f}
const XrcImport xrc_hle_com[] = {
	M("IClassFactory::QueryInterface", cf_QueryInterface), M("IClassFactory::AddRef", com_AddRef),
	M("IClassFactory::Release", com_Release), M("IClassFactory::CreateInstance", cf_CreateInstance),
	M("IClassFactory::LockServer", cf_LockServer),
	M("IDxDiagProvider::QueryInterface", dxprov_QueryInterface), M("IDxDiagProvider::AddRef", com_AddRef),
	M("IDxDiagProvider::Release", com_Release), M("IDxDiagProvider::Initialize", dxprov_Initialize),
	M("IDxDiagProvider::GetRootContainer", dxprov_GetRootContainer),
	M("IDxDiagContainer::QueryInterface", dxcont_QueryInterface), M("IDxDiagContainer::AddRef", com_AddRef),
	M("IDxDiagContainer::Release", com_Release),
	M("IDxDiagContainer::GetNumberOfChildContainers", dxcont_GetNumberOfChildContainers),
	M("IDxDiagContainer::EnumChildContainerNames", dxcont_EnumChildContainerNames),
	M("IDxDiagContainer::GetChildContainer", dxcont_GetChildContainer),
	M("IDxDiagContainer::GetNumberOfProps", dxcont_GetNumberOfProps),
	M("IDxDiagContainer::EnumPropNames", dxcont_EnumPropNames), M("IDxDiagContainer::GetProp", dxcont_GetProp),
};
const unsigned xrc_hle_com_n = sizeof xrc_hle_com / sizeof xrc_hle_com[0];
