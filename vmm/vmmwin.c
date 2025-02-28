// vmmwin.c : implementation related to operating system and process
// parsing of virtual memory. Windows related features only.
//
// (c) Ulf Frisk, 2018-2022
// Author: Ulf Frisk, pcileech@frizk.net
//

#include "vmmwin.h"
#include "vmmwindef.h"
#include "vmmwinreg.h"
#include "vmmproc.h"
#include "charutil.h"
#include "util.h"
#include "pdb.h"
#include "pe.h"
#include "mm.h"
#include "infodb.h"
#ifdef _WIN32
#include <sddl.h>
#include <shlwapi.h>
#endif /* _WIN32 */

// ----------------------------------------------------------------------------
// WINDOWS SPECIFIC PROCESS RELATED FUNCTIONALITY BELOW:
//    GENERAL FUNCTIONALITY
// ----------------------------------------------------------------------------

PIMAGE_NT_HEADERS VmmWin_GetVerifyHeaderPE(_In_ PVMM_PROCESS pProcess, _In_opt_ QWORD vaModule, _Inout_ PBYTE pbModuleHeader, _Out_ PBOOL pfHdr32)
{
    PIMAGE_DOS_HEADER dosHeader;
    PIMAGE_NT_HEADERS ntHeader;
    *pfHdr32 = FALSE;
    if(vaModule) {
        if(!VmmReadPage(pProcess, vaModule, pbModuleHeader)) { return NULL; }
    }
    dosHeader = (PIMAGE_DOS_HEADER)pbModuleHeader; // dos header.
    if(!dosHeader || dosHeader->e_magic != IMAGE_DOS_SIGNATURE) { return NULL; }
    if((dosHeader->e_lfanew < 0) || (dosHeader->e_lfanew > 0x800)) { return NULL; }
    ntHeader = (PIMAGE_NT_HEADERS)(pbModuleHeader + dosHeader->e_lfanew); // nt header
    if(!ntHeader || ntHeader->Signature != IMAGE_NT_SIGNATURE) { return NULL; }
    if((ntHeader->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) && (ntHeader->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)) { return NULL; }
    *pfHdr32 = (ntHeader->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC);
    return ntHeader;
}

int VmmWin_HashTableLookup_CmpSort(PDWORD pdw1, PDWORD pdw2)
{
    return (*pdw1 < *pdw2) ? -1 : ((*pdw1 > *pdw2) ? 1 : 0);
}

// ----------------------------------------------------------------------------
// WINDOWS SPECIFIC PROCESS RELATED FUNCTIONALITY BELOW:
//    IMPORT/EXPORT DIRECTORY PARSING
// ----------------------------------------------------------------------------

/*
* Callback function for cache map entry validity - an entry is valid
* if it's in the same medium refresh tickcount.
*/
BOOL VmmWinEATIAT_Callback_ValidEntry(_Inout_ PQWORD qwContext, _In_ QWORD qwKey, _In_ PVOID pvObject)
{
    return *qwContext == ctxVmm->tcRefreshMedium;
}

VOID VmmWinEAT_ObCloseCallback(_In_ PVMMOB_MAP_EAT pObEAT)
{
    LocalFree(pObEAT->pbMultiText);
}

/*
* Helper function for EAT initialization.
* CALLER DECREF: return
* -- pProcess
* -- pModule
* -- return
*/
PVMMOB_MAP_EAT VmmWinEAT_Initialize_DoWork(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_MODULEENTRY pModule)
{
    BYTE pbModuleHeader[0x1000] = { 0 };
    PIMAGE_NT_HEADERS64 ntHeader64;
    PIMAGE_NT_HEADERS32 ntHeader32;
    QWORD vaExpDir, vaAddressOfNames, vaAddressOfNameOrdinals, vaAddressOfFunctions;
    DWORD i, oExpDir, cbExpDir;
    PWORD pwNameOrdinals;
    PDWORD pdwRvaNames, pdwRvaFunctions;
    PBYTE pbExpDir = NULL;
    PIMAGE_EXPORT_DIRECTORY pExpDir;
    BOOL fHdr32;
    POB_STRMAP pObStrMap = NULL;
    PVMMOB_MAP_EAT pObEAT = NULL;
    PVMM_MAP_EATENTRY pe;
    // load both 32/64 bit ntHeader (only one will be valid)
    if(!(ntHeader64 = (PIMAGE_NT_HEADERS64)VmmWin_GetVerifyHeaderPE(pProcess, pModule->vaBase, pbModuleHeader, &fHdr32))) { goto fail; }
    ntHeader32 = (PIMAGE_NT_HEADERS32)ntHeader64;
    // load Export Address Table (EAT)
    oExpDir = fHdr32 ?
        ntHeader32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress :
        ntHeader64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    cbExpDir = fHdr32 ?
        ntHeader32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size :
        ntHeader64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    vaExpDir = pModule->vaBase + oExpDir;
    if(!oExpDir || !cbExpDir || cbExpDir > 0x01000000) { goto fail; }
    if(!(pbExpDir = LocalAlloc(0, cbExpDir + 1ULL))) { goto fail; }
    if(!VmmRead(pProcess, vaExpDir, pbExpDir, cbExpDir)) { goto fail; }
    pbExpDir[cbExpDir] = 0;
    // sanity check EAT
    pExpDir = (PIMAGE_EXPORT_DIRECTORY)pbExpDir;
    if(!pExpDir->NumberOfFunctions || (pExpDir->NumberOfFunctions > 0xffff)) { goto fail; }
    if(pExpDir->NumberOfNames > pExpDir->NumberOfFunctions) { goto fail; }
    vaAddressOfNames = pModule->vaBase + pExpDir->AddressOfNames;
    vaAddressOfNameOrdinals = pModule->vaBase + pExpDir->AddressOfNameOrdinals;
    vaAddressOfFunctions = pModule->vaBase + pExpDir->AddressOfFunctions;
    if((vaAddressOfNames < vaExpDir) || (vaAddressOfNames > vaExpDir + cbExpDir - pExpDir->NumberOfNames * sizeof(DWORD))) { goto fail; }
    if((vaAddressOfNameOrdinals < vaExpDir) || (vaAddressOfNameOrdinals > vaExpDir + cbExpDir - pExpDir->NumberOfNames * sizeof(WORD))) { goto fail; }
    if((vaAddressOfFunctions < vaExpDir) || (vaAddressOfFunctions > vaExpDir + cbExpDir - pExpDir->NumberOfNames * sizeof(DWORD))) { goto fail; }
    pdwRvaNames = (PDWORD)(pbExpDir + pExpDir->AddressOfNames - oExpDir);
    pwNameOrdinals = (PWORD)(pbExpDir + pExpDir->AddressOfNameOrdinals - oExpDir);
    pdwRvaFunctions = (PDWORD)(pbExpDir + pExpDir->AddressOfFunctions - oExpDir);
    // allocate EAT-MAP
    if(!(pObStrMap = ObStrMap_New(OB_STRMAP_FLAGS_CASE_SENSITIVE))) { goto fail; }
    if(!(pObEAT = Ob_Alloc(OB_TAG_MAP_EAT, LMEM_ZEROINIT, sizeof(VMMOB_MAP_EAT) + pExpDir->NumberOfFunctions * (sizeof(VMM_MAP_EATENTRY) + sizeof(QWORD)), (OB_CLEANUP_CB)VmmWinEAT_ObCloseCallback, NULL))) { goto fail; }
    pObEAT->pHashTableLookup = (PQWORD)((QWORD)pObEAT + sizeof(VMMOB_MAP_EAT) + pExpDir->NumberOfFunctions * sizeof(VMM_MAP_EATENTRY));
    pObEAT->cMap = pExpDir->NumberOfFunctions;
    pObEAT->vaModuleBase = pModule->vaBase;
    pObEAT->dwOrdinalBase = pExpDir->Base;
    pObEAT->vaAddressOfFunctions = vaAddressOfFunctions;
    pObEAT->vaAddressOfNames = vaAddressOfNames;
    pObEAT->cNumberOfFunctions = pExpDir->NumberOfFunctions;
    pObEAT->cNumberOfNames = pExpDir->NumberOfNames;
    // walk exported function names
    for(i = 0; i < pExpDir->NumberOfNames && i < pObEAT->cMap; i++) {
        if(pwNameOrdinals[i] >= pExpDir->NumberOfFunctions) { continue; }                     // name ordinal >= number of functions -> "fail"
        if(pdwRvaNames[i] < oExpDir || (pdwRvaNames[i] >= oExpDir + cbExpDir)) { continue; }  // name outside export directory -> "fail"
        pe = pObEAT->pMap + pwNameOrdinals[i];
        pe->vaFunction = pModule->vaBase + pdwRvaFunctions[pwNameOrdinals[i]];
        pe->dwOrdinal = pExpDir->Base + pwNameOrdinals[i];
        pe->oFunctionsArray = pwNameOrdinals[i];
        pe->oNamesArray = i;
        ObStrMap_PushPtrAU(pObStrMap, (LPSTR)(pbExpDir - oExpDir + pdwRvaNames[i]), &pe->uszFunction, &pe->cbuFunction);
    }
    ObStrMap_FinalizeAllocU_DECREF_NULL(&pObStrMap, &pObEAT->pbMultiText, &pObEAT->cbMultiText);
    // walk exported functions
    for(i = 0; i < pObEAT->cMap; i++) {
        pe = pObEAT->pMap + i;
        if(pe->vaFunction) {    // function has name
            pObEAT->pHashTableLookup[i] = ((QWORD)i << 32) | (DWORD)CharUtil_Hash64U(pe->uszFunction, TRUE);
            continue;
        }
        pe->vaFunction = pModule->vaBase + pdwRvaFunctions[i];
        pe->dwOrdinal = pExpDir->Base + i;
        pe->oFunctionsArray = i;
        pe->oNamesArray = -1;
        pe->cbuFunction = 1;
        pe->uszFunction = (LPSTR)pObEAT->pbMultiText;
    }
    // sort hashtable, cleanup, return
    qsort(pObEAT->pHashTableLookup, pObEAT->cMap, sizeof(QWORD), (int(*)(const void *, const void *))VmmWin_HashTableLookup_CmpSort);
    LocalFree(pbExpDir);
    return pObEAT;
fail:
    Ob_DECREF(pObStrMap);
    LocalFree(pbExpDir);
    return Ob_Alloc(OB_TAG_MAP_EAT, LMEM_ZEROINIT, sizeof(VMMOB_MAP_EAT), NULL, NULL);
}

/*
* Initialize EAT (exported functions) for a specific module.
* CALLER DECREF: return
* -- pProcess
* -- pModule
* -- return
*/
PVMMOB_MAP_EAT VmmWinEAT_Initialize(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_MODULEENTRY pModule)
{
    BOOL f;
    PVMMOB_MAP_EAT pObMap = NULL;
    QWORD qwKey = (pProcess->dwPID ^ ((QWORD)pProcess->dwPID << 48) ^ pModule->vaBase);
    f = ctxVmm->pObCacheMapEAT ||
        (ctxVmm->pObCacheMapEAT = ObCacheMap_New(0x20, VmmWinEATIAT_Callback_ValidEntry, OB_CACHEMAP_FLAGS_OBJECT_OB));
    if(!f) { return NULL; }
    if((pObMap = ObCacheMap_GetByKey(ctxVmm->pObCacheMapEAT, qwKey))) { return pObMap; }
    EnterCriticalSection(&pProcess->LockUpdate);
    pObMap = ObCacheMap_GetByKey(ctxVmm->pObCacheMapEAT, qwKey);
    if(!pObMap && (pObMap = VmmWinEAT_Initialize_DoWork(pProcess, pModule))) {
        ObCacheMap_Push(ctxVmm->pObCacheMapEAT, qwKey, pObMap, ctxVmm->tcRefreshMedium);
    }
    LeaveCriticalSection(&pProcess->LockUpdate);
    return pObMap;
}

VOID VmmWinIAT_ObCloseCallback(_In_ PVMMOB_MAP_IAT pObIAT)
{
    LocalFree(pObIAT->pbMultiText);
}

/*
* Helper function for IAT initialization.
* CALLER DECREF: return
* -- pProcess
* -- pModule
* -- return
*/
PVMMOB_MAP_IAT VmmWinIAT_Initialize_DoWork(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_MODULEENTRY pModule)
{
    BYTE pbModuleHeader[0x1000] = { 0 };
    PIMAGE_NT_HEADERS64 ntHeader64;
    PIMAGE_NT_HEADERS32 ntHeader32;
    QWORD i, oImportDirectory;
    PIMAGE_IMPORT_DESCRIPTOR pIID;
    PQWORD pIAT64, pHNA64;
    PDWORD pIAT32, pHNA32;
    PBYTE pbModule = NULL;
    DWORD c, j, cbModule, cbRead;
    BOOL fHdr32, fNameFn, fNameMod;
    POB_STRMAP pObStrMap = NULL;
    PVMMOB_MAP_IAT pObIAT = NULL;
    PVMM_MAP_IATENTRY pe;
    // Load the module
    if(pModule->cbImageSize > 0x02000000) { goto fail; }
    cbModule = pModule->cbImageSize;
    if(!(pbModule = LocalAlloc(LMEM_ZEROINIT, cbModule))) { goto fail; }
    VmmReadEx(pProcess, pModule->vaBase, pbModule, cbModule, &cbRead, 0);
    if(cbRead <= 0x2000) { goto fail; }
    pbModule[cbModule - 1] = 0;
    // load both 32/64 bit ntHeader (only one will be valid)
    if(!(ntHeader64 = (PIMAGE_NT_HEADERS64)VmmWin_GetVerifyHeaderPE(pProcess, pModule->vaBase, pbModuleHeader, &fHdr32))) { goto fail; }
    ntHeader32 = (PIMAGE_NT_HEADERS32)ntHeader64;
    oImportDirectory = fHdr32 ?
        ntHeader32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress :
        ntHeader64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if(!oImportDirectory || (oImportDirectory >= cbModule)) { goto fail; }
    // Allocate IAT-MAP
    if(!(pObStrMap = ObStrMap_New(OB_STRMAP_FLAGS_CASE_SENSITIVE))) { goto fail; }
    if(!(pObIAT = Ob_Alloc(OB_TAG_MAP_IAT, LMEM_ZEROINIT, sizeof(VMMOB_MAP_IAT) + pModule->cIAT * sizeof(VMM_MAP_IATENTRY), (OB_CLEANUP_CB)VmmWinIAT_ObCloseCallback, NULL))) { goto fail; }
    pObIAT->cMap = pModule->cIAT;
    pObIAT->vaModuleBase = pModule->vaBase;
    // Walk imported modules / functions
    pIID = (PIMAGE_IMPORT_DESCRIPTOR)(pbModule + oImportDirectory);
    i = 0, c = 0;
    while((oImportDirectory + (i + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR) < cbModule) && pIID[i].FirstThunk) {
        if(c >= pObIAT->cMap) { break; }
        if(pIID[i].Name > cbModule - 64) { i++; continue; }
        if(fHdr32) {
            // 32-bit PE
            j = 0;
            pIAT32 = (PDWORD)(pbModule + pIID[i].FirstThunk);
            pHNA32 = (PDWORD)(pbModule + pIID[i].OriginalFirstThunk);
            while(TRUE) {
                if(c >= pObIAT->cMap) { break; }
                if((QWORD)(pIAT32 + j) + sizeof(DWORD) - (QWORD)pbModule > cbModule) { break; }
                if((QWORD)(pHNA32 + j) + sizeof(DWORD) - (QWORD)pbModule > cbModule) { break; }
                if(!pIAT32[j]) { break; }
                if(!pHNA32[j]) { break; }
                fNameFn = (pHNA32[j] < cbModule);
                fNameMod = (pIID[i].Name < cbModule);
                // store
                pe = pObIAT->pMap + c;
                pe->vaFunction = pIAT32[j];
                ObStrMap_PushPtrAU(pObStrMap, (fNameFn ? (LPSTR)(pbModule + pHNA32[j] + 2) : NULL), &pe->uszFunction, &pe->cbuFunction);
                ObStrMap_PushPtrAU(pObStrMap, (fNameMod ? (LPSTR)(pbModule + pIID[i].Name) : NULL), &pe->uszModule, &pe->cbuModule);
                pe->Thunk.f32 = TRUE;
                pe->Thunk.rvaFirstThunk = pIID[i].FirstThunk + j * sizeof(DWORD);
                pe->Thunk.rvaOriginalFirstThunk = pIID[i].OriginalFirstThunk + j * sizeof(DWORD);
                pe->Thunk.wHint = fNameFn ? *(PWORD)(pbModule + pHNA32[j]) : 0;
                pe->Thunk.rvaNameFunction = pHNA32[j];
                pe->Thunk.rvaNameModule = pIID[i].Name;
                c++;
                j++;
            }
        } else {
            // 64-bit PE
            j = 0;
            pIAT64 = (PQWORD)(pbModule + pIID[i].FirstThunk);
            pHNA64 = (PQWORD)(pbModule + pIID[i].OriginalFirstThunk);
            while(TRUE) {
                if(c >= pObIAT->cMap) { break; }
                if((QWORD)(pIAT64 + j) + sizeof(QWORD) - (QWORD)pbModule > cbModule) { break; }
                if((QWORD)(pHNA64 + j) + sizeof(QWORD) - (QWORD)pbModule > cbModule) { break; }
                if(!pIAT64[j] || (!VMM_UADDR64(pIAT64[j]) && !VMM_KADDR64(pIAT64[j]))) { break; }
                if(!pHNA64[j]) { break; }
                fNameFn = (pHNA64[j] < cbModule);
                fNameMod = (pIID[i].Name < cbModule);
                // store
                pe = pObIAT->pMap + c;
                pe->vaFunction = pIAT64[j];
                ObStrMap_PushPtrAU(pObStrMap, (fNameFn ? (LPSTR)(pbModule + pHNA64[j] + 2) : NULL), &pe->uszFunction, &pe->cbuFunction);
                ObStrMap_PushPtrAU(pObStrMap, (fNameMod ? (LPSTR)(pbModule + pIID[i].Name) : NULL), &pe->uszModule, &pe->cbuModule);
                pe->Thunk.f32 = FALSE;
                pe->Thunk.rvaFirstThunk = pIID[i].FirstThunk + j * sizeof(QWORD);
                pe->Thunk.rvaOriginalFirstThunk = pIID[i].OriginalFirstThunk + j * sizeof(QWORD);
                pe->Thunk.wHint = fNameFn ? *(PWORD)(pbModule + pHNA64[j]) : 0;
                pe->Thunk.rvaNameFunction = (DWORD)pHNA64[j];
                pe->Thunk.rvaNameModule = pIID[i].Name;
                c++;
                j++;
            }
        }
        i++;
    }
    // fixups
    ObStrMap_FinalizeAllocU_DECREF_NULL(&pObStrMap, &pObIAT->pbMultiText, &pObIAT->cbMultiText);
    for(i = 0; i < pObIAT->cMap; i++) {
        pe = pObIAT->pMap + i;
        if(!pe->uszModule) {
            pe->cbuModule = 1;
            pe->uszModule = (LPSTR)pObIAT->pbMultiText;
        }
        if(!pe->uszFunction) {
            pe->cbuFunction = 1;
            pe->uszFunction = (LPSTR)pObIAT->pbMultiText;
        }
    }
    LocalFree(pbModule);
    return pObIAT;
fail:
    LocalFree(pbModule);
    Ob_DECREF(pObStrMap);
    return Ob_Alloc(OB_TAG_MAP_IAT, LMEM_ZEROINIT, sizeof(VMMOB_MAP_IAT), NULL, NULL);
}

/*
* Initialize IAT (imported functions) for a specific module.
* CALLER DECREF: return
* -- pProcess
* -- pModule
* -- return
*/
PVMMOB_MAP_IAT VmmWinIAT_Initialize(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_MODULEENTRY pModule)
{
    BOOL f;
    PVMMOB_MAP_IAT pObMap = NULL;
    QWORD qwKey = (pProcess->dwPID ^ ((QWORD)pProcess->dwPID << 48) ^ pModule->vaBase);
    f = ctxVmm->pObCacheMapIAT ||
        (ctxVmm->pObCacheMapIAT = ObCacheMap_New(0x20, VmmWinEATIAT_Callback_ValidEntry, OB_CACHEMAP_FLAGS_OBJECT_OB));
    if(!f) { return NULL; }
    if((pObMap = ObCacheMap_GetByKey(ctxVmm->pObCacheMapIAT, qwKey))) { return pObMap; }
    EnterCriticalSection(&pProcess->LockUpdate);
    pObMap = ObCacheMap_GetByKey(ctxVmm->pObCacheMapIAT, qwKey);
    if(!pObMap && (pObMap = VmmWinIAT_Initialize_DoWork(pProcess, pModule))) {
        ObCacheMap_Push(ctxVmm->pObCacheMapIAT, qwKey, pObMap, ctxVmm->tcRefreshMedium);
    }
    LeaveCriticalSection(&pProcess->LockUpdate);
    return pObMap;
}



// ----------------------------------------------------------------------------
// WINDOWS SPECIFIC PROCESS RELATED FUNCTIONALITY BELOW:
//    PEB/LDR USER MODE PARSING CODE (64-bit and 32-bit)
// ----------------------------------------------------------------------------

#define VMMPROCWINDOWS_MAX_MODULES      512

// more extensive definition of the Windows LDR_DATA_TABLE_ENTRY struct.
typedef struct _LDR_MODULE64 {
    LIST_ENTRY64        InLoadOrderModuleList;
    LIST_ENTRY64        InMemoryOrderModuleList;
    LIST_ENTRY64        InInitializationOrderModuleList;
    QWORD               BaseAddress;
    QWORD               EntryPoint;
    ULONG               SizeOfImage;
    ULONG               _Filler1;
    UNICODE_STRING64    FullDllName;
    UNICODE_STRING64    BaseDllName;
    ULONG               Flags;
    SHORT               LoadCount;
    SHORT               TlsIndex;
    LIST_ENTRY64        HashTableEntry;
    ULONG               TimeDateStamp;
    ULONG               _Filler2;
} LDR_MODULE64, *PLDR_MODULE64;

typedef struct _LDR_MODULE32 {
    LIST_ENTRY32        InLoadOrderModuleList;
    LIST_ENTRY32        InMemoryOrderModuleList;
    LIST_ENTRY32        InInitializationOrderModuleList;
    DWORD               BaseAddress;
    DWORD               EntryPoint;
    ULONG               SizeOfImage;
    UNICODE_STRING32    FullDllName;
    UNICODE_STRING32    BaseDllName;
    ULONG               Flags;
    SHORT               LoadCount;
    SHORT               TlsIndex;
    LIST_ENTRY32        HashTableEntry;
    ULONG               TimeDateStamp;
} LDR_MODULE32, *PLDR_MODULE32;

typedef struct _PEB_LDR_DATA32 {
    BYTE Reserved1[8];
    DWORD Reserved2;
    LIST_ENTRY32 InLoadOrderModuleList;
    LIST_ENTRY32 InMemoryOrderModuleList;
    LIST_ENTRY32 InInitializationOrderModuleList;
} PEB_LDR_DATA32, *PPEB_LDR_DATA32;

typedef struct _PEB_LDR_DATA64 {
    BYTE Reserved1[8];
    QWORD Reserved2;
    LIST_ENTRY64 InLoadOrderModuleList;
    LIST_ENTRY64 InMemoryOrderModuleList;
    LIST_ENTRY64 InInitializationOrderModuleList;
} PEB_LDR_DATA64, *PPEB_LDR_DATA64;

typedef struct _PEB32 {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[1];
    DWORD Reserved3[2];
    DWORD Ldr;
    DWORD ProcessParameters;
    DWORD SubSystemData;
    DWORD ProcessHeap;
    DWORD Unknown1[27];
    DWORD NumberOfHeaps;
    DWORD MaximumNumberOfHeaps;
    DWORD ProcessHeaps;
    // ...
} PEB32, *PPEB32;

typedef struct _PEB64 {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[1];
    DWORD _Filler;
    QWORD Reserved3[2];
    QWORD Ldr;
    QWORD ProcessParameters;
    QWORD SubSystemData;
    QWORD ProcessHeap;
    QWORD Unknown1[22];
    DWORD NumberOfHeaps;
    DWORD MaximumNumberOfHeaps;
    QWORD ProcessHeaps;
    // ...
} PEB64, *PPEB64;

typedef struct tdVMMWIN_LDRMODULES_CONTEXT {
    DWORD cwszTextTotal;
    DWORD cModules;
    DWORD cModulesMax;
    PVMM_MAP_MODULEENTRY pModules;
    POB_SET psVaName;
} VMMWIN_LDRMODULES_CONTEXT, *PVMMWIN_LDRMODULES_CONTEXT;

VOID VmmWinLdrModule_Initialize_VSetPutVA(_In_ POB_SET pObSet_vaAll, _In_ POB_SET pObSet_vaTry1, _In_ QWORD va)
{
    if(!ObSet_Exists(pObSet_vaAll, va)) {
        ObSet_Push(pObSet_vaAll, va);
        ObSet_Push(pObSet_vaTry1, va);
    }
}

VOID VmmWinLdrModule_Initialize64(_In_ PVMM_PROCESS pProcess, _Inout_ POB_MAP pmModules, _In_ BOOL fUserOnly)
{
    QWORD vaModuleLdrFirst64, vaModuleLdr64 = 0;
    BYTE pbPEB64[sizeof(PEB64)], pbPEBLdrData64[sizeof(PEB_LDR_DATA64)], pbLdrModule64[sizeof(LDR_MODULE64)];
    PPEB64 pPEB64 = (PPEB64)pbPEB64;
    PPEB_LDR_DATA64 pPEBLdrData64 = (PPEB_LDR_DATA64)pbPEBLdrData64;
    PLDR_MODULE64 pLdrModule64 = (PLDR_MODULE64)pbLdrModule64;
    VMM_MAP_MODULEENTRY oModule;
    POB_SET pObSet_vaAll = NULL, pObSet_vaTry1 = NULL, pObSet_vaTry2 = NULL;
    BOOL fTry1;
    DWORD i, cbReadData;
    // prefetch existing addresses (if any) & allocate new vaModuleLdr VSet
    pObSet_vaAll = ObContainer_GetOb(pProcess->pObPersistent->pObCLdrModulesPrefetch64);
    VmmCachePrefetchPages3(pProcess, pObSet_vaAll, sizeof(LDR_MODULE64), 0);
    Ob_DECREF_NULL(&pObSet_vaAll);
    if(!(pObSet_vaAll = ObSet_New())) { goto fail; }
    if(!(pObSet_vaTry1 = ObSet_New())) { goto fail; }
    if(!(pObSet_vaTry2 = ObSet_New())) { goto fail; }
    // set up initial entry in vaModuleLdr DataSet
    if(fUserOnly) {
        // User mode process -> walk PEB LDR list to enumerate modules / .dlls.
        if(!pProcess->win.vaPEB) { goto fail; }
        if(!VmmRead(pProcess, pProcess->win.vaPEB, pbPEB64, sizeof(PEB64))) { goto fail; }
        if(!VmmRead(pProcess, (QWORD)pPEB64->Ldr, pbPEBLdrData64, sizeof(PEB_LDR_DATA64))) { goto fail; }
        for(i = 0; i < 6; i++) {
            vaModuleLdrFirst64 = *(PQWORD)((PBYTE)&pPEBLdrData64->InLoadOrderModuleList + i * sizeof(QWORD));
            if(VMM_UADDR64_8(vaModuleLdrFirst64)) {
                ObSet_Push(pObSet_vaAll, vaModuleLdrFirst64);
                ObSet_Push(pObSet_vaTry1, vaModuleLdrFirst64);
            }
        }
    } else {
        // Kernel mode process -> walk PsLoadedModuleList to enumerate drivers / .sys and .dlls.
        if(!ctxVmm->kernel.vaPsLoadedModuleListPtr) { goto fail; }
        if(!VmmRead(pProcess, ctxVmm->kernel.vaPsLoadedModuleListPtr, (PBYTE)&vaModuleLdrFirst64, sizeof(QWORD)) || !vaModuleLdrFirst64) { goto fail; }
        if(!VmmRead(pProcess, ctxVmm->kernel.vaPsLoadedModuleListPtr, pbPEBLdrData64, sizeof(PEB_LDR_DATA64))) { goto fail; }
        ObSet_Push(pObSet_vaAll, vaModuleLdrFirst64);
        ObSet_Push(pObSet_vaTry1, vaModuleLdrFirst64);
    }
    // iterate over modules using all available linked lists in an efficient way.
    fTry1 = TRUE;
    vaModuleLdr64 = 0;
    while(ObMap_Size(pmModules) < VMMPROCWINDOWS_MAX_MODULES) {
        if(fTry1) {
            vaModuleLdr64 = ObSet_Pop(pObSet_vaTry1);
            if(!vaModuleLdr64 && (0 == ObSet_Size(pObSet_vaTry2))) { break; }
            if(!vaModuleLdr64) {
                VmmCachePrefetchPages3(pProcess, pObSet_vaAll, sizeof(PEB_LDR_DATA64), 0);
                fTry1 = FALSE;
                continue;
            }
            VmmReadEx(pProcess, vaModuleLdr64, pbLdrModule64, sizeof(LDR_MODULE64), &cbReadData, VMM_FLAG_FORCECACHE_READ);
            if(cbReadData != sizeof(LDR_MODULE64)) {
                ObSet_Push(pObSet_vaTry2, vaModuleLdr64);
                continue;
            }
        } else {
            vaModuleLdr64 = ObSet_Pop(pObSet_vaTry2);
            if(!vaModuleLdr64 && (0 == ObSet_Size(pObSet_vaTry1))) { break; }
            if(!vaModuleLdr64) { fTry1 = TRUE; continue; }
            if(!VmmRead(pProcess, vaModuleLdr64, pbLdrModule64, sizeof(LDR_MODULE64))) { continue; }
        }
        if(!pLdrModule64->BaseAddress || (pLdrModule64->BaseAddress & 0xfff)) { continue; }
        if(!pLdrModule64->SizeOfImage || (pLdrModule64->SizeOfImage >= 0x40000000)) { continue; }
        if(!pLdrModule64->BaseDllName.Length || pLdrModule64->BaseDllName.Length >= 0x1000) { continue; }
        ZeroMemory(&oModule, sizeof(VMM_MAP_MODULEENTRY));
        oModule.vaBase = pLdrModule64->BaseAddress;
        oModule.vaEntry = pLdrModule64->EntryPoint;
        oModule.cbImageSize = (DWORD)pLdrModule64->SizeOfImage;
        oModule.fWoW64 = FALSE;
        // module name
        oModule.cbuText = pLdrModule64->BaseDllName.Length;
        oModule._Reserved1 = pLdrModule64->BaseDllName.Buffer;
        // module path+name
        oModule.cbuFullName = pLdrModule64->FullDllName.Length;
        oModule._Reserved3 = pLdrModule64->FullDllName.Buffer;
        // push module to result map
        ObMap_PushCopy(pmModules, oModule.vaBase, &oModule, sizeof(VMM_MAP_MODULEENTRY));
        // add FLinkAll/BLink lists
        if(pLdrModule64->InLoadOrderModuleList.Flink && !((QWORD)pLdrModule64->InLoadOrderModuleList.Flink & 0x7)) {
            VmmWinLdrModule_Initialize_VSetPutVA(pObSet_vaAll, pObSet_vaTry1, (QWORD)CONTAINING_RECORD64(pLdrModule64->InLoadOrderModuleList.Flink, LDR_MODULE64, InLoadOrderModuleList));
        }
        if(pLdrModule64->InLoadOrderModuleList.Blink && !((QWORD)pLdrModule64->InLoadOrderModuleList.Blink & 0x7)) {
            VmmWinLdrModule_Initialize_VSetPutVA(pObSet_vaAll, pObSet_vaTry1, (QWORD)CONTAINING_RECORD64(pLdrModule64->InLoadOrderModuleList.Blink, LDR_MODULE64, InLoadOrderModuleList));
        }
        if(pProcess->fUserOnly) {
            if(pLdrModule64->InInitializationOrderModuleList.Flink && !((QWORD)pLdrModule64->InInitializationOrderModuleList.Flink & 0x7)) {
                VmmWinLdrModule_Initialize_VSetPutVA(pObSet_vaAll, pObSet_vaTry1, (QWORD)CONTAINING_RECORD64(pLdrModule64->InInitializationOrderModuleList.Flink, LDR_MODULE64, InInitializationOrderModuleList));
            }
            if(pLdrModule64->InInitializationOrderModuleList.Blink && !((QWORD)pLdrModule64->InInitializationOrderModuleList.Blink & 0x7)) {
                VmmWinLdrModule_Initialize_VSetPutVA(pObSet_vaAll, pObSet_vaTry1, (QWORD)CONTAINING_RECORD64(pLdrModule64->InInitializationOrderModuleList.Blink, LDR_MODULE64, InInitializationOrderModuleList));
            }
            if(pLdrModule64->InMemoryOrderModuleList.Flink && !((QWORD)pLdrModule64->InMemoryOrderModuleList.Flink & 0x7)) {
                VmmWinLdrModule_Initialize_VSetPutVA(pObSet_vaAll, pObSet_vaTry1, (QWORD)CONTAINING_RECORD64(pLdrModule64->InMemoryOrderModuleList.Flink, LDR_MODULE64, InMemoryOrderModuleList));
            }
            if(pLdrModule64->InMemoryOrderModuleList.Blink && !((QWORD)pLdrModule64->InMemoryOrderModuleList.Blink & 0x7)) {
                VmmWinLdrModule_Initialize_VSetPutVA(pObSet_vaAll, pObSet_vaTry1, (QWORD)CONTAINING_RECORD64(pLdrModule64->InMemoryOrderModuleList.Blink, LDR_MODULE64, InMemoryOrderModuleList));
            }
        }
    }
    // save prefetch addresses (if desirable)
    if(ctxMain->dev.fVolatile && ctxVmm->ThreadProcCache.fEnabled) {
        ObContainer_SetOb(pProcess->pObPersistent->pObCLdrModulesPrefetch64, pObSet_vaAll);
    }
fail:
    Ob_DECREF(pObSet_vaAll);
    Ob_DECREF(pObSet_vaTry1);
    Ob_DECREF(pObSet_vaTry2);
    if(!fUserOnly && pProcess->win.vaPEB) {
        VmmWinLdrModule_Initialize64(pProcess, pmModules, TRUE);
    }
}

VOID VmmWinLdrModule_Initialize32(_In_ PVMM_PROCESS pProcess, _Inout_ POB_MAP pmModules, _In_ BOOL fUserOnly)
{
    DWORD vaModuleLdrFirst32, vaModuleLdr32 = 0;
    BYTE pbPEB32[sizeof(PEB32)], pbPEBLdrData32[sizeof(PEB_LDR_DATA32)], pbLdrModule32[sizeof(LDR_MODULE32)];
    PPEB32 pPEB32 = (PPEB32)pbPEB32;
    PPEB_LDR_DATA32 pPEBLdrData32 = (PPEB_LDR_DATA32)pbPEBLdrData32;
    PLDR_MODULE32 pLdrModule32 = (PLDR_MODULE32)pbLdrModule32;
    VMM_MAP_MODULEENTRY oModule;
    POB_SET pObSet_vaAll = NULL, pObSet_vaTry1 = NULL, pObSet_vaTry2 = NULL;
    BOOL fTry1;
    DWORD i, cbReadData;
    // prefetch existing addresses (if any) & allocate new vaModuleLdr VSet
    pObSet_vaAll = ObContainer_GetOb(pProcess->pObPersistent->pObCLdrModulesPrefetch32);
    VmmCachePrefetchPages3(pProcess, pObSet_vaAll, sizeof(LDR_MODULE32), 0);
    Ob_DECREF(pObSet_vaAll);
    if(!(pObSet_vaAll = ObSet_New())) { goto fail; }
    if(!(pObSet_vaTry1 = ObSet_New())) { goto fail; }
    if(!(pObSet_vaTry2 = ObSet_New())) { goto fail; }
    // set up initial entry in vaModuleLdr DataSet
    if(fUserOnly) {
        if(!pProcess->win.vaPEB32) { goto fail; }
        if(!VmmRead(pProcess, pProcess->win.vaPEB32, pbPEB32, sizeof(PEB32))) { goto fail; }
        if(!VmmRead(pProcess, (QWORD)pPEB32->Ldr, pbPEBLdrData32, sizeof(PEB_LDR_DATA32))) { goto fail; }
        for(i = 0; i < 6; i++) {
            vaModuleLdrFirst32 = *(PDWORD)((PBYTE)&pPEBLdrData32->InLoadOrderModuleList + i * sizeof(DWORD));
            if(VMM_UADDR32_4(vaModuleLdrFirst32)) {
                ObSet_Push(pObSet_vaAll, vaModuleLdrFirst32);
                ObSet_Push(pObSet_vaTry1, vaModuleLdrFirst32);
            }
        }

    } else if(ctxVmm->tpSystem == VMM_SYSTEM_WINDOWS_X86) {
        // Kernel mode process -> walk PsLoadedModuleList to enumerate drivers / .sys and .dlls.
        if(!ctxVmm->kernel.vaPsLoadedModuleListPtr) { goto fail; }
        if(!VmmRead(pProcess, ctxVmm->kernel.vaPsLoadedModuleListPtr, (PBYTE)&vaModuleLdrFirst32, sizeof(DWORD)) || !vaModuleLdrFirst32) { goto fail; }
        if(!VmmRead(pProcess, ctxVmm->kernel.vaPsLoadedModuleListPtr, pbPEBLdrData32, sizeof(PEB_LDR_DATA32))) { goto fail; }
        ObSet_Push(pObSet_vaAll, vaModuleLdrFirst32);
        ObSet_Push(pObSet_vaTry1, vaModuleLdrFirst32);
    } else {
        goto fail;
    }
    // iterate over modules using all available linked lists in an efficient way.
    fTry1 = TRUE;
    vaModuleLdr32 = 0;
    while(ObMap_Size(pmModules) < VMMPROCWINDOWS_MAX_MODULES) {
        if(fTry1) {
            vaModuleLdr32 = (DWORD)ObSet_Pop(pObSet_vaTry1);
            if(!vaModuleLdr32 && (0 == ObSet_Size(pObSet_vaTry2))) { break; }
            if(!vaModuleLdr32) {
                VmmCachePrefetchPages3(pProcess, pObSet_vaAll, sizeof(PEB_LDR_DATA32), 0);
                fTry1 = FALSE;
                continue;
            }
            VmmReadEx(pProcess, vaModuleLdr32, pbLdrModule32, sizeof(LDR_MODULE32), &cbReadData, VMM_FLAG_FORCECACHE_READ);
            if(cbReadData != sizeof(LDR_MODULE64)) {
                ObSet_Push(pObSet_vaTry2, vaModuleLdr32);
                continue;
            }
        } else {
            vaModuleLdr32 = (DWORD)ObSet_Pop(pObSet_vaTry2);
            if(!vaModuleLdr32 && (0 == ObSet_Size(pObSet_vaTry1))) { break; }
            if(!vaModuleLdr32) { fTry1 = TRUE; continue; }
            if(!VmmRead(pProcess, vaModuleLdr32, pbLdrModule32, sizeof(LDR_MODULE32))) { continue; }
        }
        if(!pLdrModule32->BaseAddress || (pLdrModule32->BaseAddress & 0xfff)) { continue; }
        if(!pLdrModule32->SizeOfImage || (pLdrModule32->SizeOfImage >= 0x10000000)) { continue; }
        if(!pLdrModule32->BaseDllName.Length || pLdrModule32->BaseDllName.Length >= 0x1000) { continue; }
        ZeroMemory(&oModule, sizeof(VMM_MAP_MODULEENTRY));
        oModule.vaBase = (QWORD)pLdrModule32->BaseAddress;
        oModule.vaEntry = (QWORD)pLdrModule32->EntryPoint;
        oModule.cbImageSize = (DWORD)pLdrModule32->SizeOfImage;
        oModule.fWoW64 = pProcess->win.fWow64;
        // module name
        oModule.cbuText = pLdrModule32->BaseDllName.Length;
        oModule._Reserved1 = pLdrModule32->BaseDllName.Buffer;
        // module path+name
        oModule.cbuFullName = pLdrModule32->FullDllName.Length;
        oModule._Reserved3 = pLdrModule32->FullDllName.Buffer;
        // push module to result map
        ObMap_PushCopy(pmModules, oModule.vaBase, &oModule, sizeof(VMM_MAP_MODULEENTRY));
        // add FLinkAll/BLink lists
        if(pLdrModule32->InLoadOrderModuleList.Flink && !((DWORD)pLdrModule32->InLoadOrderModuleList.Flink & 0x3)) {
            VmmWinLdrModule_Initialize_VSetPutVA(pObSet_vaAll, pObSet_vaTry1, (QWORD)CONTAINING_RECORD32(pLdrModule32->InLoadOrderModuleList.Flink, LDR_MODULE32, InLoadOrderModuleList));
        }
        if(pLdrModule32->InLoadOrderModuleList.Blink && !((DWORD)pLdrModule32->InLoadOrderModuleList.Blink & 0x3)) {
            VmmWinLdrModule_Initialize_VSetPutVA(pObSet_vaAll, pObSet_vaTry1, (QWORD)CONTAINING_RECORD32(pLdrModule32->InLoadOrderModuleList.Blink, LDR_MODULE32, InLoadOrderModuleList));
        }
        if(pProcess->fUserOnly) {
            if(pLdrModule32->InInitializationOrderModuleList.Flink && !((DWORD)pLdrModule32->InInitializationOrderModuleList.Flink & 0x3)) {
                VmmWinLdrModule_Initialize_VSetPutVA(pObSet_vaAll, pObSet_vaTry1, (QWORD)CONTAINING_RECORD32(pLdrModule32->InInitializationOrderModuleList.Flink, LDR_MODULE32, InInitializationOrderModuleList));
            }
            if(pLdrModule32->InInitializationOrderModuleList.Blink && !((DWORD)pLdrModule32->InInitializationOrderModuleList.Blink & 0x3)) {
                VmmWinLdrModule_Initialize_VSetPutVA(pObSet_vaAll, pObSet_vaTry1, (QWORD)CONTAINING_RECORD32(pLdrModule32->InInitializationOrderModuleList.Blink, LDR_MODULE32, InInitializationOrderModuleList));
            }
            if(pLdrModule32->InMemoryOrderModuleList.Flink && !((DWORD)pLdrModule32->InMemoryOrderModuleList.Flink & 0x3)) {
                VmmWinLdrModule_Initialize_VSetPutVA(pObSet_vaAll, pObSet_vaTry1, (QWORD)CONTAINING_RECORD32(pLdrModule32->InMemoryOrderModuleList.Flink, LDR_MODULE32, InMemoryOrderModuleList));
            }
            if(pLdrModule32->InMemoryOrderModuleList.Blink && !((DWORD)pLdrModule32->InMemoryOrderModuleList.Blink & 0x3)) {
                VmmWinLdrModule_Initialize_VSetPutVA(pObSet_vaAll, pObSet_vaTry1, (QWORD)CONTAINING_RECORD32(pLdrModule32->InMemoryOrderModuleList.Blink, LDR_MODULE32, InMemoryOrderModuleList));
            }
        }
    }
    // save prefetch addresses (if desirable)
    if(ctxMain->dev.fVolatile && ctxVmm->ThreadProcCache.fEnabled) {
        ObContainer_SetOb(pProcess->pObPersistent->pObCLdrModulesPrefetch64, pObSet_vaAll);
    }
fail:
    Ob_DECREF(pObSet_vaAll);
    Ob_DECREF(pObSet_vaTry1);
    Ob_DECREF(pObSet_vaTry2);
    if(!fUserOnly && pProcess->win.vaPEB) {
        VmmWinLdrModule_Initialize32(pProcess, pmModules, TRUE);
    }
}

VOID VmmWinLdrModule_InitializeVAD(_In_ PVMM_PROCESS pProcess, _Inout_ POB_MAP pmModules)
{
    BOOL fX;
    DWORD iVad, iPte = 0;
    PVMM_MAP_VADENTRY peVad;
    PVMMOB_MAP_PTE pObPteMap = NULL;
    PVMMOB_MAP_VAD pObVadMap = NULL;
    VMM_MAP_MODULEENTRY oModule;
    if(!pProcess->fUserOnly) { return; }
    if(!VmmMap_GetVad(pProcess, &pObVadMap, VMM_VADMAP_TP_PARTIAL)) { return; }
    for(iVad = 0; iVad < pObVadMap->cMap; iVad++) {
        peVad = pObVadMap->pMap + iVad;
        if(!peVad->fImage) { continue; }
        if(ObMap_ExistsKey(pmModules, peVad->vaStart)) { continue; }
        ZeroMemory(&oModule, sizeof(VMM_MAP_MODULEENTRY));
        oModule.vaBase = peVad->vaStart;
        oModule.cbImageSize = (DWORD)PE_GetSize(pProcess, oModule.vaBase);
        if(!oModule.cbImageSize || (oModule.cbImageSize > 0x04000000)) { continue; }
        oModule.fWoW64 = pProcess->win.fWow64 && (oModule.vaBase < 0xffffffff);
        // image vad not already in map found; check if pte map contains hw
        // executable pte's -> assume unlinked module, otherwise assume data.
        if(!pObPteMap && !VmmMap_GetPte(pProcess, &pObPteMap, FALSE)) { goto fail; }
        // move pte index to current vad
        while((iPte < pObPteMap->cMap) && (pObPteMap->pMap[iPte].vaBase + (pObPteMap->pMap[iPte].cPages << 12) <= peVad->vaStart)) {
            iPte++;
        }
        // check if vad contains hw executable page
        fX = FALSE;
        while(!fX && (iPte < pObPteMap->cMap) && (pObPteMap->pMap[iPte].vaBase < peVad->vaEnd)) {
            fX = pObPteMap->pMap[iPte].fPage && !(pObPteMap->pMap[iPte].fPage & VMM_MEMMAP_PAGE_NX);
            iPte++;
        }
        oModule.tp = fX ? VMM_MODULE_TP_NOTLINKED : VMM_MODULE_TP_DATA;
        ObMap_PushCopy(pmModules, oModule.vaBase, &oModule, sizeof(VMM_MAP_MODULEENTRY));
    }
fail:
    Ob_DECREF(pObPteMap);
    Ob_DECREF(pObVadMap);
}

_Success_(return)
BOOL VmmWinLdrModule_InitializeInjectedEntry(_In_ PVMM_PROCESS pProcess, _Inout_ POB_MAP pmModules, _In_ QWORD vaModuleBase)
{
    QWORD cbImageSize;
    VMM_MAP_MODULEENTRY oModule = { 0 };
    cbImageSize = PE_GetSize(pProcess, vaModuleBase);
    if(ObMap_ExistsKey(pmModules, vaModuleBase)) { return FALSE; }
    if(!cbImageSize || cbImageSize > 0x04000000) { return FALSE; }
    oModule.vaBase = vaModuleBase;
    oModule.tp = VMM_MODULE_TP_INJECTED;
    oModule.cbImageSize = (DWORD)cbImageSize;
    oModule.fWoW64 = pProcess->win.fWow64 && (oModule.vaBase < 0xffffffff);
    return ObMap_PushCopy(pmModules, oModule.vaBase, &oModule, sizeof(VMM_MAP_MODULEENTRY));
}

VOID VmmWinLdrModule_InitializeInjected(_In_ PVMM_PROCESS pProcess, _Inout_ POB_MAP pmModules, _Inout_opt_ POB_SET psvaInjected)
{
    DWORD i;
    QWORD va;
    PVMMOB_MAP_VAD pObVadMap = NULL;
    POB_DATA pvaObDataInjected = NULL;
    BOOL fObAlloc_psvaInjected;  
    if(!psvaInjected && !ObContainer_Exists(pProcess->pObPersistent->pObCLdrModulesInjected)) { return; }
    fObAlloc_psvaInjected = !psvaInjected && (psvaInjected = ObSet_New());
    // merge previously saved injected modules into 'psvaInjected' address set
    if((pvaObDataInjected = ObContainer_GetOb(pProcess->pObPersistent->pObCLdrModulesInjected))) {
        ObSet_PushData(psvaInjected, pvaObDataInjected);
        Ob_DECREF_NULL(&pvaObDataInjected);
    }
    // add injected modules module map
    if(ObSet_Size(psvaInjected)) {
        if(!VmmMap_GetVad(pProcess, &pObVadMap, VMM_VADMAP_TP_FULL)) { goto fail; }
        i = 0;
        while(i < ObSet_Size(psvaInjected)) {
            va = ObSet_Get(psvaInjected, i);
            if(!VmmWinLdrModule_InitializeInjectedEntry(pProcess, pmModules, va)) {
                ObSet_Remove(psvaInjected, va);
            } else {
                i++;
            }
        }
        Ob_DECREF_NULL(&pObVadMap);
    }
    //  save to "persistent" refresh memory storage.
    if(ObSet_Size(psvaInjected)) {
        pvaObDataInjected = ObSet_GetAll(psvaInjected);
        ObContainer_SetOb(pProcess->pObPersistent->pObCLdrModulesInjected, pvaObDataInjected);
        Ob_DECREF_NULL(&pvaObDataInjected);
    }
fail:
    if(fObAlloc_psvaInjected) {
        Ob_DECREF(psvaInjected);
    }
    Ob_DECREF(pObVadMap);
}

_Success_(return)
BOOL VmmWinLdrModule_Initialize_Name(_In_ PVMM_PROCESS pProcess, _Inout_ PVMMOB_MAP_MODULE pModuleMap)
{
    BOOL fWow64 = pProcess->win.fWow64;
    DWORD i;
    PVMM_MAP_MODULEENTRY pe;
    POB_SET psObPrefetch = NULL;
    POB_STRMAP psmOb = NULL;
    LPSTR uszPrefix;
    CHAR uszName[MAX_PATH], uszFullName[MAX_PATH], szNamePE[MAX_PATH];
    if(!(psmOb = ObStrMap_New(OB_STRMAP_FLAGS_CASE_INSENSITIVE))) { return FALSE; }
    // 1: prefetch
    psObPrefetch = ObSet_New();
    for(i = 0; i < pModuleMap->cMap; i++) {
        pe = pModuleMap->pMap + i;
        ObSet_Push_PageAlign(psObPrefetch, pe->vaBase, 0x1000);
        ObSet_Push_PageAlign(psObPrefetch, pe->_Reserved1, MAX_PATH * 2);
        ObSet_Push_PageAlign(psObPrefetch, pe->_Reserved3, MAX_PATH * 2);
    }
    VmmCachePrefetchPages(pProcess, psObPrefetch, 0);
    // 2: iterate over entries
    for(i = 0; i < pModuleMap->cMap; i++) {
        pe = pModuleMap->pMap + i;
        uszFullName[0] = 0;
        uszName[0] = 0;
        uszPrefix = "";
        // name from ldr list
        if(pe->_Reserved1) {
            VmmReadWtoU(pProcess, pe->_Reserved1, min(pe->cbuText, 2 * MAX_PATH), VMM_FLAG_FORCECACHE_READ, uszName, sizeof(uszName), NULL, NULL, CHARUTIL_FLAG_TRUNCATE_ONFAIL_NULLSTR | CHARUTIL_FLAG_STR_BUFONLY);
            CharUtil_FixFsNameU(uszName, sizeof(uszName), uszName, 0, FALSE);
            pe->_Reserved1 = 0;
        }
        // fullname from ldr list
        if(pe->_Reserved3) {
            VmmReadWtoU(pProcess, pe->_Reserved3, min(pe->cbuFullName, 2 * MAX_PATH), VMM_FLAG_FORCECACHE_READ, uszFullName, sizeof(uszFullName), NULL, NULL, CHARUTIL_FLAG_TRUNCATE_ONFAIL_NULLSTR | CHARUTIL_FLAG_STR_BUFONLY);
            pe->_Reserved3 = 0;
        }
        // name from pe embedded
        if(!uszName[0] && PE_GetModuleName(pProcess, pe->vaBase, szNamePE, MAX_PATH)) {
            CharUtil_FixFsName(uszName, NULL, szNamePE, NULL, MAX_PATH, 0, FALSE);
        }
        // name from VAD not feasible due to deadlock risk when initializing VAD names.
        // set prefix, fix fullname and commit to strmap
        if(!uszName[0]) {
            sprintf_s(uszName, MAX_PATH, "0x%llx.dll", pe->vaBase);
            uszPrefix = "_NA-";
        }
        // ntdll.dll rename on wow64 processes to avoid name collisions
        if(fWow64 && (pe->vaBase > 0xffffffff) && !strcmp(uszName, "ntdll.dll")) {
            uszPrefix = "_64-";
        }
        if(pe->tp == VMM_MODULE_TP_DATA) { uszPrefix = "_DATA-"; }
        if(pe->tp == VMM_MODULE_TP_NOTLINKED) { uszPrefix = "_NOTLINKED-"; }
        if(pe->tp == VMM_MODULE_TP_INJECTED) { uszPrefix = "_INJECTED-"; }
        ObStrMap_PushUU_snprintf_s(psmOb, &pe->uszText, &pe->cbuText, "%s%s", uszPrefix, uszName);
        ObStrMap_PushPtrUU(psmOb, (uszFullName[0] ? uszFullName : uszName), &pe->uszFullName, &pe->cbuFullName);
    }
    ObStrMap_FinalizeAllocU_DECREF_NULL(&psmOb, &pModuleMap->pbMultiText, &pModuleMap->cbMultiText);
    Ob_DECREF(psObPrefetch);
    return TRUE;
}

VOID VmmWinLdrModule_Initialize_SetHash(_In_ PVMM_PROCESS pProcess, _Inout_ PVMMOB_MAP_MODULE pModuleMap)
{
    QWORD i;
    for(i = 0; i < pModuleMap->cMap; i++) {
        pModuleMap->pHashTableLookup[i] = (i << 32) | CharUtil_HashNameFsU(pModuleMap->pMap[i].uszText, 0);
    }
    qsort(pModuleMap->pHashTableLookup, pModuleMap->cMap, sizeof(QWORD), (int(*)(const void*, const void*))VmmWin_HashTableLookup_CmpSort);
}

VOID VmmWinLdrModule_Initialize_SetSize(_In_ PVMM_PROCESS pProcess, _Inout_ PVMMOB_MAP_MODULE pModuleMap)
{
    DWORD i;
    BYTE pbModuleHeader[0x1000];
    PVMM_MAP_MODULEENTRY pe;
    POB_SET psObPrefetch = NULL;
    // prefetch MZ header
    if(!(psObPrefetch = ObSet_New())) { return; }
    for(i = 0; i < pModuleMap->cMap; i++) {
        ObSet_Push(psObPrefetch, pModuleMap->pMap[i].vaBase);
    }
    // fetch size values from cache loaded nt header.
    VmmCachePrefetchPages(pProcess, psObPrefetch, 0); ObSet_Clear(psObPrefetch);
    for(i = 0; i < pModuleMap->cMap; i++) {
        pe = pModuleMap->pMap + i;
        if(!VmmRead2(pProcess, pe->vaBase, pbModuleHeader, 0x1000, VMM_FLAG_FORCECACHE_READ)) { continue; }
        pe->cbFileSizeRaw = PE_FileRaw_Size(pProcess, 0, pbModuleHeader);
        pe->cSection = PE_SectionGetNumberOfEx(pProcess, 0, pbModuleHeader);
        pe->cIAT = PE_IatGetNumberOfEx(pProcess, 0, pbModuleHeader);
        ObSet_Push_PageAlign(psObPrefetch, pe->vaBase + PE_DirectoryGetOffset(pProcess, 0, pbModuleHeader, IMAGE_DIRECTORY_ENTRY_EXPORT), sizeof(IMAGE_EXPORT_DIRECTORY));
    }
    // fetch number of exports (EAT)
    VmmCachePrefetchPages(pProcess, psObPrefetch, 0);
    for(i = 0; i < pModuleMap->cMap; i++) {
        pModuleMap->pMap[i].cEAT = PE_EatGetNumberOfEx(pProcess, pModuleMap->pMap[i].vaBase, NULL);
    }
    Ob_DECREF(psObPrefetch);
}

VOID VmmWinLdrModule_CallbackCleanup_ObMapModule(PVMMOB_MAP_MODULE pOb)
{
    LocalFree(pOb->pbMultiText);
}

/*
* Initialize the module map containing information about loaded modules in the
* system. This is performed by a PEB/Ldr walk/scan of in-process memory
* structures. This may be unreliable if a process is obfuscated or tampered.
* -- pProcess
* -- psvaInjected = optional set of injected addresses, updated on exit.
* -- return
*/
_Success_(return)
BOOL VmmWinLdrModule_Initialize(_In_ PVMM_PROCESS pProcess, _Inout_opt_ POB_SET psvaInjected)
{
    PVMM_MAP_MODULEENTRY pe;
    POB_MAP pmObModules = NULL;
    PVMMOB_MAP_MODULE pObMap = NULL, pObMap_PreExisting = NULL;
    DWORD i, cModules, cbObMap;
    // check if already initialized -> skip
    if(pProcess->Map.pObModule && (!psvaInjected || !ObSet_Size(psvaInjected))) { return TRUE; }
    VmmTlbSpider(pProcess);
    EnterCriticalSection(&pProcess->LockUpdate);
    if(pProcess->Map.pObModule && (!psvaInjected || !ObSet_Size(psvaInjected))) { goto fail; }  // not strict fail - but triggr cleanup and success.
    // set up context
    if(!(pmObModules = ObMap_New(OB_MAP_FLAGS_OBJECT_LOCALFREE))) { goto fail; }
    // fetch modules: "ordinary" linked list
    if((ctxVmm->tpSystem == VMM_SYSTEM_WINDOWS_X86) || ((ctxVmm->tpSystem == VMM_SYSTEM_WINDOWS_X64) && pProcess->win.fWow64)) {
        VmmWinLdrModule_Initialize32(pProcess, pmObModules, pProcess->fUserOnly);
    }
    if(ctxVmm->tpSystem == VMM_SYSTEM_WINDOWS_X64) {
        VmmWinLdrModule_Initialize64(pProcess, pmObModules, pProcess->fUserOnly);
    }
    // fetch modules: VADs
    VmmWinLdrModule_InitializeVAD(pProcess, pmObModules);
    // fetch modules: optional injected
    VmmWinLdrModule_InitializeInjected(pProcess, pmObModules, psvaInjected);
    // set up module map object
    cModules = ObMap_Size(pmObModules);
    cbObMap = sizeof(VMMOB_MAP_MODULE) + cModules * (sizeof(VMM_MAP_MODULEENTRY) + sizeof(QWORD));
    if(!(pObMap = Ob_Alloc(OB_TAG_MAP_MODULE, LMEM_ZEROINIT, cbObMap, (OB_CLEANUP_CB)VmmWinLdrModule_CallbackCleanup_ObMapModule, NULL))) { goto fail; }
    pObMap->pHashTableLookup = (PQWORD)(((PBYTE)pObMap) + sizeof(VMMOB_MAP_MODULE) + cModules * sizeof(VMM_MAP_MODULEENTRY));
    pObMap->cMap = cModules;
    for(i = 0; i < cModules; i++) {
        pe = ObMap_GetByIndex(pmObModules, i);
        memcpy(pObMap->pMap + i, pe, sizeof(VMM_MAP_MODULEENTRY));
    }
    // fetch module names
    if(!VmmWinLdrModule_Initialize_Name(pProcess, pObMap)) { goto fail; }
    // fetch raw file size, #sections, imports (IAT) and exports (EAT)
    VmmWinLdrModule_Initialize_SetSize(pProcess, pObMap);
    // set name hash table
    VmmWinLdrModule_Initialize_SetHash(pProcess, pObMap);
    // finish set-up
    pObMap_PreExisting = pProcess->Map.pObModule;
    pProcess->Map.pObModule = Ob_INCREF(pObMap);
fail:
    if(!pProcess->Map.pObModule) {
        // try set up zero-sized module map on fail
        pObMap = Ob_Alloc(OB_TAG_MAP_MODULE, LMEM_ZEROINIT, sizeof(VMMOB_MAP_MODULE), NULL, NULL);
        pProcess->Map.pObModule = pObMap;
    }
    LeaveCriticalSection(&pProcess->LockUpdate);
    Ob_DECREF(pmObModules);
    Ob_DECREF(pObMap);
    Ob_DECREF(pObMap_PreExisting);
    return pProcess->Map.pObModule ? TRUE : FALSE;
}



// ----------------------------------------------------------------------------
// UNLOADED MODULE FUNCTIONALITY BELOW:
// ----------------------------------------------------------------------------

VOID VmmWinUnloadedModule_CallbackCleanup_ObMapUnloadedModule(PVMMOB_MAP_UNLOADEDMODULE pOb)
{
    LocalFree(pOb->pbMultiText);
}

QWORD VmmWinUnloadedModule_vaNtdllUnloadedArray(_In_ PVMM_PROCESS pProcess, _In_ BOOL f32)
{
    BYTE pb[8];
    PDB_HANDLE hPDB;
    QWORD va, vaUnloadedArray = 0;
    PVMM_MAP_MODULEENTRY peModule;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    // 1: fetch cached
    vaUnloadedArray = f32 ? ctxVmm->ContextUnloadedModule.vaNtdll32 : ctxVmm->ContextUnloadedModule.vaNtdll64;
    if(-1 == (DWORD)vaUnloadedArray) { return 0; }
    if(vaUnloadedArray) { return vaUnloadedArray; }
    // 2: fetch ntdll module
    if(!VmmMap_GetModuleEntryEx(pProcess, 0, "ntdll.dll", &pObModuleMap, &peModule)) { goto fail; }
    // 2.1: try fetch addr RtlpUnloadEventTrace from dism of RtlGetUnloadEventTrace export
    if((va = PE_GetProcAddress(pProcess, peModule->vaBase, "RtlGetUnloadEventTrace")) && VmmRead(pProcess, va, pb, 8)) {
        if(f32 && (pb[0] == 0xb8) && (pb[5] == 0xc3)) { // x86 dism
            vaUnloadedArray = *(PDWORD)(pb + 1);
        }
        if(!f32 && (pb[0] == 0x48) && (pb[1] == 0x8d) && (pb[2] == 0x05) && (pb[7] == 0xc3)) {  // x64 dism
            va += 7ULL + *(PDWORD)(pb + 3);
            if(VmmRead(pProcess, va, pb, 8)) {
                vaUnloadedArray = va;
            }
        }
    }
    // 2.2: try fetch addr ntdll!RtlpUnloadEventTrace from PDB
    if(!vaUnloadedArray) {
        hPDB = PDB_GetHandleFromModuleAddress(pProcess, peModule->vaBase);
        PDB_GetSymbolAddress(hPDB, "RtlpUnloadEventTrace", &vaUnloadedArray);
    }
    // 3: commit to cache
    if(f32) {
        ctxVmm->ContextUnloadedModule.vaNtdll32 = vaUnloadedArray ? (DWORD)vaUnloadedArray : -1;
    } else {
        ctxVmm->ContextUnloadedModule.vaNtdll64 = vaUnloadedArray ? vaUnloadedArray : -1;
    }
fail:
    Ob_DECREF(pObModuleMap);
    return vaUnloadedArray;
}

/*
* Retrieve unloaded user-mode modules for the specific process. This is achieved
* by parsing the array RtlpUnloadEventTrace in ntdll.dll. The array location is
* retrieved by (1) parsing exports or (2) loading symbol from ntdll.dll PDB.
*/
VOID VmmWinUnloadedModule_InitializeUser(_In_ PVMM_PROCESS pProcess)
{
    BOOL f32 = ctxVmm->f32 || pProcess->win.fWow64;
    BYTE pbBuffer[RTL_UNLOAD_EVENT_TRACE_NUMBER * 0x68] = { 0 };
    QWORD cbStruct, vaUnloadedArray;
    DWORD i, cbBuffer, cMap;
    PRTL_UNLOAD_EVENT_TRACE32 pe32;
    PRTL_UNLOAD_EVENT_TRACE64 pe64;
    PVMM_MAP_UNLOADEDMODULEENTRY pe;
    PVMMOB_MAP_UNLOADEDMODULE pObMap = NULL;
    POB_STRMAP psmOb = NULL;
    // 1: fetch unloaded modules array
    if(!(vaUnloadedArray = VmmWinUnloadedModule_vaNtdllUnloadedArray(pProcess, f32))) { return; }
    cbBuffer = RTL_UNLOAD_EVENT_TRACE_NUMBER * (f32 ? sizeof(RTL_UNLOAD_EVENT_TRACE32) : sizeof(RTL_UNLOAD_EVENT_TRACE64));
    VmmRead2(pProcess, vaUnloadedArray, pbBuffer, cbBuffer, VMM_FLAG_ZEROPAD_ON_FAIL);
    // 2: parse data and count
    if(f32) {
        cbStruct = 0x5c;
        if(ctxVmm->kernel.dwVersionBuild <= 6002) { cbStruct = 0x54; }  // <= VISTA SP2
        for(cMap = 0; cMap < RTL_UNLOAD_EVENT_TRACE_NUMBER; cMap++) {
            pe32 = (PRTL_UNLOAD_EVENT_TRACE32)(pbBuffer + cMap * cbStruct);
            if(!VMM_UADDR32_PAGE(pe32->BaseAddress)) { break; }
            if(!pe32->SizeOfImage || (pe32->SizeOfImage > 0x10000000)) { break; }
            pe32->ImageName[31] = 0;
        }
    } else {
        cbStruct = 0x68;
        if(ctxVmm->kernel.dwVersionBuild <= 6002) { cbStruct = 0x60; }  // <= VISTA SP2
        for(cMap = 0; cMap < RTL_UNLOAD_EVENT_TRACE_NUMBER; cMap++) {
            pe64 = (PRTL_UNLOAD_EVENT_TRACE64)(pbBuffer + cMap * cbStruct);
            if(!VMM_UADDR64_PAGE(pe64->BaseAddress)) { break; }
            if(!pe64->SizeOfImage || (pe64->SizeOfImage > 0x10000000)) { break; }
            pe64->ImageName[31] = 0;
        }
    }
    // 3: alloc and fill
    if(!(psmOb = ObStrMap_New(0))) { return; }
    pObMap = Ob_Alloc(OB_TAG_MAP_UNLOADEDMODULE, LMEM_ZEROINIT, sizeof(VMMOB_MAP_UNLOADEDMODULE) + cMap * sizeof(VMM_MAP_UNLOADEDMODULEENTRY), (OB_CLEANUP_CB)VmmWinUnloadedModule_CallbackCleanup_ObMapUnloadedModule, NULL);
    if(!pObMap) {
        Ob_DECREF(psmOb);
        return;
    }
    pObMap->cMap = cMap;
    if(f32) {
        for(i = 0; i < cMap; i++) {
            pe = pObMap->pMap + i;
            pe32 = (PRTL_UNLOAD_EVENT_TRACE32)(pbBuffer + i * cbStruct);
            pe->fWoW64 = pProcess->win.fWow64;
            pe->vaBase = pe32->BaseAddress;
            pe->cbImageSize = pe32->SizeOfImage;
            pe->dwCheckSum = pe32->CheckSum;
            pe->dwTimeDateStamp = pe32->TimeDateStamp;
            ObStrMap_PushPtrWU(psmOb, pe32->ImageName, &pe->uszText, &pe->cbuText);
        }
    } else {
        for(i = 0; i < cMap; i++) {
            pe = pObMap->pMap + i;
            pe64 = (PRTL_UNLOAD_EVENT_TRACE64)(pbBuffer + i * cbStruct);
            pe->vaBase = pe64->BaseAddress;
            pe->cbImageSize = (DWORD)pe64->SizeOfImage;
            pe->dwCheckSum = pe64->CheckSum;
            pe->dwTimeDateStamp = pe64->TimeDateStamp;
            ObStrMap_PushPtrWU(psmOb, pe64->ImageName, &pe->uszText, &pe->cbuText);
        }
    }
    ObStrMap_FinalizeAllocU_DECREF_NULL(&psmOb, &pObMap->pbMultiText, &pObMap->cbMultiText);
    pProcess->Map.pObUnloadedModule = pObMap;   // pass on reference ownership to pProcess
}

/*
* Retrieve unloaded kernel modules. This is done by analyzing the kernel symbols
* MmUnloadedDrivers and MmLastUnloadedDriver. This function requires a valid PDB
* for the kernel to properly function.
*/
VOID VmmWinUnloadedModule_InitializeKernel(_In_ PVMM_PROCESS pProcess)
{
    BOOL f, f32 = ctxVmm->f32;
    QWORD i, j, va = 0;
    DWORD cMap = 0, cUnloadMax, cbStruct, cbMultiText = 2, owszMultiText = 1;
    PMM_UNLOADED_DRIVER32 pe32;
    PMM_UNLOADED_DRIVER64 pe64;
    PVMM_MAP_UNLOADEDMODULEENTRY pe;
    PVMMOB_MAP_UNLOADEDMODULE pObMap = NULL;
    POB_STRMAP psmOb = NULL;
    BYTE pbBuffer[MM_UNLOADED_DRIVER_MAX * sizeof(MM_UNLOADED_DRIVER64)] = { 0 };
    if(!ctxVmm->kernel.opt.vaMmUnloadedDrivers || !ctxVmm->kernel.opt.vaMmLastUnloadedDriver) { return; }
    // 1: fetch data
    cbStruct = f32 ? sizeof(MM_UNLOADED_DRIVER32) : sizeof(MM_UNLOADED_DRIVER64);
    if(!VmmRead(pProcess, ctxVmm->kernel.opt.vaMmUnloadedDrivers, (PBYTE)&va, f32 ? sizeof(DWORD) : sizeof(QWORD))) { return; }
    if(!VmmRead(pProcess, ctxVmm->kernel.opt.vaMmLastUnloadedDriver, (PBYTE)&cUnloadMax, sizeof(DWORD))) { return; }
    if(!VMM_KADDR_4_8(va) || !cUnloadMax || (cUnloadMax > MM_UNLOADED_DRIVER_MAX)) { return; }
    if(!VmmRead(pProcess, va, pbBuffer, cUnloadMax * cbStruct)) { return; }
    // 2: parse data and count
    for(i = 0; i < cUnloadMax; i++) {
        if(f32) {
            pe32 = (PMM_UNLOADED_DRIVER32)(pbBuffer + i * cbStruct);
            f = VMM_KADDR32_PAGE(pe32->ModuleStart) && VMM_KADDR32(pe32->ModuleEnd) && pe32->UnloadTime &&
                pe32->Name.Length && !(pe32->Name.Length & 1) && VMM_KADDR32(pe32->Name.Buffer) &&
                (pe32->ModuleEnd - pe32->ModuleStart < 0x10000000);
            if(!f) {
                pe32->ModuleStart = 0;
                continue;
            }
            cMap++;
        } else {
            pe64 = (PMM_UNLOADED_DRIVER64)(pbBuffer + i * cbStruct);
            f = VMM_KADDR64_PAGE(pe64->ModuleStart) && VMM_KADDR64(pe64->ModuleEnd) && pe64->UnloadTime &&
                pe64->Name.Length && !(pe64->Name.Length & 1) && VMM_KADDR64(pe64->Name.Buffer) &&
                (pe64->ModuleEnd - pe64->ModuleStart < 0x10000000);
            if(!f) {
                pe64->ModuleStart = 0;
                continue;
            }
            cMap++;
        }
    }
    // 3: alloc and fill
    if(!(psmOb = ObStrMap_New(0))) { return; }
    pObMap = Ob_Alloc(OB_TAG_MAP_UNLOADEDMODULE, LMEM_ZEROINIT, sizeof(VMMOB_MAP_UNLOADEDMODULE) + cMap * sizeof(VMM_MAP_UNLOADEDMODULEENTRY), (OB_CLEANUP_CB)VmmWinUnloadedModule_CallbackCleanup_ObMapUnloadedModule, NULL);
    if(!pObMap) {
        Ob_DECREF(psmOb);
        return;
    }
    pObMap->cMap = cMap;
    for(i = 0, j = 0; i < cUnloadMax; i++) {
        if(f32) {
            pe32 = (PMM_UNLOADED_DRIVER32)(pbBuffer + i * cbStruct);
            if(!pe32->ModuleStart) { continue; }
            pe = pObMap->pMap + j; j++;
            pe->vaBase = pe32->ModuleStart;
            pe->cbImageSize = pe32->ModuleEnd+ pe32->ModuleStart;
            pe->ftUnload = pe32->UnloadTime;
            ObStrMap_Push_UnicodeBuffer(psmOb, pe32->Name.Length, pe32->Name.Buffer, &pe->uszText, &pe->cbuText);
        } else {
            pe64 = (PMM_UNLOADED_DRIVER64)(pbBuffer + i * cbStruct);
            if(!pe64->ModuleStart) { continue; }
            pe = pObMap->pMap + j; j++;
            pe->vaBase = pe64->ModuleStart;
            pe->cbImageSize = (DWORD)(pe64->ModuleEnd + pe64->ModuleStart);
            pe->ftUnload = pe64->UnloadTime;
            ObStrMap_Push_UnicodeBuffer(psmOb, pe64->Name.Length, pe64->Name.Buffer, &pe->uszText, &pe->cbuText);
        }
    }
    ObStrMap_FinalizeAllocU_DECREF_NULL(&psmOb, &pObMap->pbMultiText, &pObMap->cbMultiText);
    pProcess->Map.pObUnloadedModule = pObMap;   // pass on reference ownership to pProcess
}

/*
* Initialize the unloaded module map containing information about unloaded modules.
* -- pProcess
* -- return
*/
_Success_(return)
BOOL VmmWinUnloadedModule_Initialize(_In_ PVMM_PROCESS pProcess)
{
    if(pProcess->Map.pObUnloadedModule) { return TRUE; }
    EnterCriticalSection(&pProcess->LockUpdate);
    if(!pProcess->Map.pObUnloadedModule) {
        if(pProcess->fUserOnly) {
            VmmWinUnloadedModule_InitializeUser(pProcess);
        } else {
            VmmWinUnloadedModule_InitializeKernel(pProcess);
        }
    }
    if(!pProcess->Map.pObUnloadedModule) {
        pProcess->Map.pObUnloadedModule = Ob_Alloc(OB_TAG_MAP_UNLOADEDMODULE, LMEM_ZEROINIT, sizeof(VMMOB_MAP_UNLOADEDMODULE), NULL, NULL);
    }
    LeaveCriticalSection(&pProcess->LockUpdate);
    return pProcess->Map.pObUnloadedModule ? TRUE : FALSE;
}



// ----------------------------------------------------------------------------
// USER PROCESS PARAMETERS FUNCTIONALITY BELOW:
// ----------------------------------------------------------------------------

PVMMWIN_USER_PROCESS_PARAMETERS VmmWin_UserProcessParameters_Get(_In_ PVMM_PROCESS pProcess)
{
    BOOL f;
    LPWSTR wszTMP = NULL;
    QWORD vaUserProcessParameters = 0;
    PVMMWIN_USER_PROCESS_PARAMETERS pu = &pProcess->pObPersistent->UserProcessParams;
    if(pu->fProcessed || pProcess->dwState) { return pu; }
    EnterCriticalSection(&pProcess->LockUpdate);
    if(ctxVmm->f32) {
        f = pProcess->win.vaPEB &&
            VmmRead(pProcess, pProcess->win.vaPEB + 0x10, (PBYTE)&vaUserProcessParameters, sizeof(DWORD)) &&
            !(vaUserProcessParameters & 0x80000003);
    } else {
        f = pProcess->win.vaPEB &&
            VmmRead(pProcess, pProcess->win.vaPEB + 0x20, (PBYTE)&vaUserProcessParameters, sizeof(QWORD)) &&
            !(vaUserProcessParameters & 0xffff800000000007);
    }
    if(f) {
        // ImagePathName or DllPath
        if(!VmmReadAllocUnicodeString(pProcess, ctxVmm->f32, 0, vaUserProcessParameters + (ctxVmm->f32 ? 0x038 : 0x060), 0x400, &wszTMP, NULL)) {    // ImagePathName
            VmmReadAllocUnicodeString(pProcess, ctxVmm->f32, 0, vaUserProcessParameters + (ctxVmm->f32 ? 0x030 : 0x050), 0x400, &wszTMP, NULL);      // DllPath (mutually exclusive with ImagePathName?)
        }
        CharUtil_WtoU(wszTMP, 0x400, NULL, 0, &pu->uszImagePathName, &pu->cbuImagePathName, CHARUTIL_FLAG_ALLOC);
        LocalFree(wszTMP); wszTMP = NULL;
        // CommandLine
        VmmReadAllocUnicodeString(pProcess, ctxVmm->f32, 0, vaUserProcessParameters + (ctxVmm->f32 ? 0x040 : 0x070), 0x800, &wszTMP, NULL);
        CharUtil_WtoU(wszTMP, 0x800, NULL, 0, &pu->uszCommandLine, &pu->cbuCommandLine, CHARUTIL_FLAG_ALLOC);
        LocalFree(wszTMP); wszTMP = NULL;
    }
    pu->fProcessed = TRUE;
    LeaveCriticalSection(&pProcess->LockUpdate);
    return pu;
}

// ----------------------------------------------------------------------------
// PTE MAP FUNCTIONALITY BELOW:
//
// Memory Maps based on hardware page tables (PTE MAP) is generated by the
// virtual memory sub-system be waking the hardware page tables. The generated
// pte map does initially not contain information about loaded modules but may
// be enriched with this information by calling VmmWinPte_InitializeMapText().
// Module names will be inserted from:
// 1) the module map
// 2) if not found in (1) and suitable pte signature by PE header peek.
// ----------------------------------------------------------------------------

/*
* Map a tag into the sorted memory map in O(log2) operations. Supply only one of szTag or wszTag.
* -- pProcess
* -- psm
* -- vaBase
* -- vaLimit = limit == vaBase + size (== top address in range +1)
* -- uszTag
* -- fWoW64
*/
VOID VmmWinPte_InitializeMapText_MapTag(_In_ PVMM_PROCESS pProcess, _In_ POB_STRMAP psm, _In_ QWORD vaBase, _In_ QWORD vaLimit, _In_opt_z_ LPSTR uszTag, _In_ BOOL fWoW64)
{
    PVMM_MAP_PTEENTRY pMap;
    QWORD i, lvl, cMap;
    pMap = pProcess->Map.pObPte->pMap;
    cMap = pProcess->Map.pObPte->cMap;
    if(!pMap || !cMap) { return; }
    if(!uszTag) { return; }
    // 1: locate base
    lvl = 1;
    i = cMap >> lvl;
    while(TRUE) {
        lvl++;
        if((cMap >> lvl) == 0) {
            break;
        }
        if(pMap[i].vaBase > vaBase) {
            i -= (cMap >> lvl);
        } else {
            i += (cMap >> lvl);
        }
    }
    // 2: scan back if needed
    while(i && (pMap[i].vaBase > vaBase)) {
        i--;
    }
    // 3.1: fill in tag
    for(; i < cMap; i++) {
        if(pMap[i].vaBase >= vaLimit) { break; }                              // outside scope
        if(pMap[i].vaBase + (pMap[i].cPages << 12) <= vaBase) { continue; }   // outside scope
        if(pMap[i].cbuText > 1) { continue; }
        pMap[i].fWoW64 = fWoW64;
        ObStrMap_PushPtrUU(psm, uszTag, &pMap[i].uszText, &pMap[i].cbuText);
    }
}

/*
* Identify module names by scanning for PE headers and tag them into the memory map.
*/
VOID VmmWinPte_InitializeMapText_ScanHeaderPE(_In_ PVMM_PROCESS pProcess, _In_ POB_STRMAP psm)
{
    DWORD cMap;
    PVMMOB_MAP_PTE pObMemMap = NULL;
    PVMM_MAP_PTEENTRY pMap;
    PVMM_MAP_PTEENTRY ppMAPs[0x400];
    PPMEM_SCATTER ppMEMs = NULL;
    DWORD i, cMEMs = 0, cbImageSize;
    BOOL result;
    CHAR szBuffer[MAX_PATH];
    // 1: checks and allocate buffers for parallel read of MZ header candidates
    if(!LcAllocScatter1(0x400, &ppMEMs)) { goto fail; }
    if(!VmmMap_GetPte(pProcess, &pObMemMap, FALSE)) { goto fail; }
    if(!pObMemMap || !pObMemMap->cMap) { goto fail; }
    cMap = pObMemMap->cMap;
    pMap = pObMemMap->pMap;
    // 2: scan memory map for MZ header candidates and put them on list for read
    for(i = 0; i < cMap - 1; i++) {
        if(ctxVmm->tpMemoryModel == VMM_MEMORYMODEL_X86) {
            result =
                !(pMap[i].vaBase & 0xffff) &&                   // starts at even 0x10000 offset
                !pMap[i].cbuText;                               // tag not already set
        } else {
            result =
                (pMap[i].cPages == 1) &&                        // PE header is only 1 page
                !(pMap[i].vaBase & 0xffff) &&                   // starts at even 0x10000 offset
                !pMap[i].cbuText &&                             // tag not already set
                (pMap[i].fPage & VMM_MEMMAP_PAGE_NX) &&         // no-execute
                !(pMap[i + 1].fPage & VMM_MEMMAP_PAGE_NX);      // next page is executable
        }
        if(result) {
            ppMEMs[cMEMs]->qwA = pMap[i].vaBase;
            ppMAPs[cMEMs] = pMap + i;
            cMEMs++;
            if(cMEMs == 0x400) { break; }
        }
    }
    // 3: read all MZ header candicates previously selected and try load name from them (after read is successful)
    if(cMEMs) {
        VmmReadScatterVirtual(pProcess, ppMEMs, cMEMs, 0);
        for(i = 0; i < cMEMs; i++) {
            if(ppMEMs[i]->f) {
                result = PE_GetModuleNameEx(pProcess, ppMAPs[i]->vaBase, TRUE, ppMEMs[i]->pb, szBuffer, _countof(szBuffer), &cbImageSize);
                if(result && (cbImageSize < 0x01000000)) {
                    VmmWinPte_InitializeMapText_MapTag(pProcess, psm, ppMAPs[i]->vaBase, ppMAPs[i]->vaBase + cbImageSize - 1, szBuffer, FALSE);
                }
            }
        }
    }
fail:
    LcMemFree(ppMEMs);
    Ob_DECREF(pObMemMap);
}

VOID VmmWinPte_InitializeMapText_Modules(_In_ PVMM_PROCESS pProcess, _In_ POB_STRMAP psm)
{
    DWORD i;
    PVMM_MAP_MODULEENTRY pModule;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    if(VmmMap_GetModule(pProcess, &pObModuleMap)) {
        // update memory map with names
        for(i = 0; i < pObModuleMap->cMap; i++) {
            pModule = pObModuleMap->pMap + i;
            VmmWinPte_InitializeMapText_MapTag(pProcess, psm, pModule->vaBase, pModule->vaBase + pModule->cbImageSize - 1, pModule->uszText, pModule->fWoW64);
        }
        Ob_DECREF(pObModuleMap);
    }
}

VOID VmmWinPte_InitializeMapText_Drivers(_In_ PVMM_PROCESS pProcess, _In_ POB_STRMAP psm)
{
    DWORD i;
    PVMM_MAP_KDRIVERENTRY pDrv;
    PVMMOB_MAP_KDRIVER pObDrvMap = NULL;
    if(pProcess->dwPID != 4) { return; }
    VmmWinPte_InitializeMapText_MapTag(pProcess, psm, ctxVmm->kernel.vaBase, ctxVmm->kernel.cbSize, "nt", FALSE);
    if(VmmMap_GetKDriver(&pObDrvMap)) {
        // update memory map with names
        for(i = 0; i < pObDrvMap->cMap; i++) {
            pDrv = pObDrvMap->pMap + i;
            if(pDrv->vaStart && pDrv->cbDriverSize && (pDrv->cbDriverSize < 0x10000000)) {
                VmmWinPte_InitializeMapText_MapTag(pProcess, psm, pDrv->vaStart, pDrv->vaStart + pDrv->cbDriverSize - 1, pDrv->uszName, FALSE);
            }
        }
        Ob_DECREF(pObDrvMap);
    }
}

VOID VmmWinPte_InitializeMapText_DoWork(_In_ PVMM_PROCESS pProcess)
{
    DWORD i;
    PVMMOB_MAP_PTE pMapPte = pProcess->Map.pObPte;
    POB_STRMAP psmOb = NULL;
    if(!(psmOb = ObStrMap_New(OB_STRMAP_FLAGS_STR_ASSIGN_TEMPORARY))) { return; }
    VmmWinPte_InitializeMapText_Drivers(pProcess, psmOb);
    VmmWinPte_InitializeMapText_Modules(pProcess, psmOb);
    VmmWinPte_InitializeMapText_ScanHeaderPE(pProcess, psmOb);
    ObStrMap_FinalizeAllocU_DECREF_NULL(&psmOb, &pMapPte->pbMultiText, &pMapPte->cbMultiText);
    // fixups not set values
    for(i = 0; i < pMapPte->cMap; i++) {
        if(!pMapPte->pMap[i].uszText) {
            pMapPte->pMap[i].uszText = (LPSTR)pMapPte->pbMultiText;
            pMapPte->pMap[i].cbuText = 1;
        }
    }
    pMapPte->fTagScan = TRUE;
}

/*
* Try initialize PteMap text descriptions. This function will first try to pop-
* ulate the pre-existing VMMOB_MAP_PTE object in pProcess with module names and
* then, if failed or partially failed, try to initialize from PE file headers.
* -- pProcess
* -- return
*/
_Success_(return)
BOOL VmmWinPte_InitializeMapText(_In_ PVMM_PROCESS pProcess)
{
    if(pProcess->Map.pObPte->fTagScan) { return TRUE; }
    EnterCriticalSection(&pProcess->LockUpdate);
    if(!pProcess->Map.pObPte->fTagScan) {
        VmmTlbSpider(pProcess);
        VmmWinPte_InitializeMapText_DoWork(pProcess);
    }
    LeaveCriticalSection(&pProcess->LockUpdate);
    return pProcess->Map.pObPte->fTagScan;
}

// ----------------------------------------------------------------------------
// HEAP FUNCTIONALITY BELOW:
// ----------------------------------------------------------------------------

typedef struct tdVMMWIN_HEAP_SEGMENT64 {
    QWORD HeapEntry[2];
    DWORD SegmentSignature;
    DWORD SegmentFlags;
    LIST_ENTRY64 _ListEntry;
    QWORD Heap;
    QWORD BaseAddress;
    QWORD NumberOfPages;
    QWORD FirstEntry;
    QWORD LastValidEntry;
    DWORD NumberOfUnCommittedPages;
    DWORD NumberOfUnCommittedRanges;
    DWORD SegmentAllocatorBackTraceIndex;
    DWORD Reserved;
    LIST_ENTRY64 UCRSegmentList;
} VMMWIN_HEAP_SEGMENT64, *PVMMWIN_HEAP_SEGMENT64;

typedef struct tdVMMWIN_HEAP_SEGMENT32 {
    DWORD HeapEntry[2];
    DWORD SegmentSignature;
    DWORD SegmentFlags;
    LIST_ENTRY32 _ListEntry;
    DWORD Heap;
    DWORD BaseAddress;
    DWORD NumberOfPages;
    DWORD FirstEntry;
    DWORD LastValidEntry;
    DWORD NumberOfUnCommittedPages;
    DWORD NumberOfUnCommittedRanges;
    DWORD SegmentAllocatorBackTraceIndex;
    DWORD Reserved;
    LIST_ENTRY32 UCRSegmentList;
} VMMWIN_HEAP_SEGMENT32, *PVMMWIN_HEAP_SEGMENT32;

typedef struct tdVMMWIN_HEAP_SEGMENT32_XP {
    DWORD HeapEntry[2];
    DWORD SegmentSignature;
    DWORD SegmentFlags;
    DWORD Heap;
    DWORD LargestUnCommittedRange;
    DWORD BaseAddress;
    DWORD NumberOfPages;
    DWORD FirstEntry;
    DWORD LastValidEntry;
    DWORD NumberOfUnCommittedPages;
    DWORD NumberOfUnCommittedRanges;
    DWORD UnCommittedRanges;
    WORD AllocatorBackTraceIndex;
    WORD Reserved;
    DWORD LastEntryInSegment;
} VMMWIN_HEAP_SEGMENT32_XP, *PVMMWIN_HEAP_SEGMENT32_XP;

VOID VmmWinHeap_Initialize32_Pre_XP(_In_ PVMM_PROCESS pProcess, _In_ POB_MAP ctx, _In_ QWORD vaHeaps[], _In_ DWORD cHeaps)
{
    QWORD i;
    VMM_MAP_HEAPENTRY e = { 0 };
    VMMWIN_HEAP_SEGMENT32_XP h = { 0 };
    VmmCachePrefetchPages4(pProcess, cHeaps, vaHeaps, sizeof(VMMWIN_HEAP_SEGMENT32_XP), 0);
    for(i = 0; i < cHeaps; i++) {
        if(!VmmRead(pProcess, vaHeaps[i], (PBYTE)&h, sizeof(VMMWIN_HEAP_SEGMENT32_XP))) { continue; }
        if((h.SegmentSignature != 0xeeffeeff) || (h.NumberOfPages >= 0x00f00000)) { continue; }
        e.HeapId = ObMap_Size(ctx);
        e.fPrimary = 1;
        e.cPages = h.NumberOfPages;
        e.cPagesUnCommitted = h.NumberOfUnCommittedPages;
        ObMap_Push(ctx, vaHeaps[i], (PVOID)e.qwHeapData);
    }
}

VOID VmmWinHeap_Initialize32_Pre(_In_ PVMM_PROCESS pProcess, _In_opt_ POB_MAP ctx, _In_ QWORD va, _In_ PBYTE pb, _In_ DWORD cb, _In_ QWORD vaFLink, _In_ QWORD vaBLink, _In_ POB_SET pVSetAddress, _Inout_ PBOOL pfValidEntry, _Inout_ PBOOL pfValidFLink, _Inout_ PBOOL pfValidBLink)
{
    QWORD v;
    VMM_MAP_HEAPENTRY e = { 0 };
    PVMMWIN_HEAP_SEGMENT32 h = (PVMMWIN_HEAP_SEGMENT32)pb;
    if((h->SegmentSignature != 0xffeeffee) || (h->NumberOfPages >= 0x00f00000)) { return; }
    *pfValidFLink = VMM_UADDR32_4(vaFLink);
    *pfValidBLink = VMM_UADDR32_4(vaBLink);
    *pfValidEntry = *pfValidFLink || *pfValidBLink;
    if((v = (QWORD)ObMap_GetByKey(ctx, h->Heap))) {
        e.HeapId = v >> 57;
    } else {
        e.HeapId = ObMap_Size(ctx);
        e.fPrimary = 1;
    }
    e.cPages = h->NumberOfPages;
    e.cPagesUnCommitted = h->NumberOfUnCommittedPages;
    ObMap_Push(ctx, va, (PVOID)e.qwHeapData);
}

VOID VmmWinHeap_Initialize64_Pre(_In_ PVMM_PROCESS pProcess, _In_opt_ POB_MAP ctx, _In_ QWORD va, _In_ PBYTE pb, _In_ DWORD cb, _In_ QWORD vaFLink, _In_ QWORD vaBLink, _In_ POB_SET pVSetAddress, _Inout_ PBOOL pfValidEntry, _Inout_ PBOOL pfValidFLink, _Inout_ PBOOL pfValidBLink)
{
    QWORD v;
    VMM_MAP_HEAPENTRY e = { 0 };
    PVMMWIN_HEAP_SEGMENT64 h = (PVMMWIN_HEAP_SEGMENT64)pb;
    if((h->SegmentSignature != 0xffeeffee) || (h->NumberOfPages >= 0x00f00000)) { return; }
    *pfValidFLink = VMM_UADDR64_8(vaFLink);
    *pfValidBLink = VMM_UADDR64_8(vaBLink);
    *pfValidEntry = *pfValidFLink || *pfValidBLink;
    if((v = (QWORD)ObMap_GetByKey(ctx, h->Heap))) {
        e.HeapId = v >> 57;
    } else {
        e.HeapId = ObMap_Size(ctx);
        e.fPrimary = 1;
    }
    e.cPages = (DWORD)h->NumberOfPages;
    e.cPagesUnCommitted = h->NumberOfUnCommittedPages;
    ObMap_Push(ctx, va, (PVOID)e.qwHeapData);
}

int VmmWinHeap_Initialize_CmpHeapEntry(PVMM_MAP_HEAPENTRY v1, PVMM_MAP_HEAPENTRY v2)
{
    return
        (v1->vaHeapSegment < v2->vaHeapSegment) ? -1 :
        (v1->vaHeapSegment > v2->vaHeapSegment) ? 1 : 0;
}

/*
* Identify and scan for 64-bit heaps in a process memory space and commit the
* result to the pProcess memory map.
* NB! The 32-bit variant below is NOT robust. It will fail a lot of times
* especially on older versions - but it will fail silently without causing
* harm except a few extra reads. Probably due to bad hardcoded values. It's
* primarily heap-header analysis that is failing. But it seems to mostly work
* on newer windows versions.
* NB! Must be called in thread-safe way.
*/
VOID VmmWinHeap_Initialize32(_In_ PVMM_PROCESS pProcess, _In_ BOOL fWow64)
{
    BOOL f;
    BYTE pbPEB32[sizeof(PEB32)];
    PPEB32 pPEB32 = (PPEB32)pbPEB32;
    DWORD i, cHeaps, vaHeaps[0x80];
    QWORD vaHeapPrimary, vaHeaps64[0x80] = { 0 };
    POB_MAP pmObHeap;
    PVMMOB_MAP_HEAP pObHeapMap;
    // 1: Read PEB
    if(!fWow64 && !pProcess->win.vaPEB) { return; }
    if(fWow64 && !pProcess->win.vaPEB32) { return; }
    if(!VmmRead(pProcess, (fWow64 ? pProcess->win.vaPEB32 : pProcess->win.vaPEB), pbPEB32, sizeof(PEB32))) { return; }
    vaHeapPrimary = pPEB32->ProcessHeap;
    cHeaps = pPEB32->NumberOfHeaps;
    if(cHeaps > 0x80) { return; } // probably not valid
    // 2: Read heap array
    f = (cHeaps <= 0x80) &&
        VmmRead(pProcess, pPEB32->ProcessHeaps, (PBYTE)vaHeaps, sizeof(DWORD) * cHeaps) &&
        (vaHeaps[0] == vaHeapPrimary);
    if(!f) { return; }
    for(i = 0; i < cHeaps; i++) {
        vaHeaps64[i] = vaHeaps[i];
    }
    // 3: Traverse heap linked list
    if(!(pmObHeap = ObMap_New(0))) { return; }
    if(ctxVmm->kernel.dwVersionBuild <= 2600) {
        // WINXP
        VmmWinHeap_Initialize32_Pre_XP(pProcess, pmObHeap, vaHeaps64, cHeaps);
    } else {
        // VISTA+
        VmmWin_ListTraversePrefetch(
            pProcess,
            TRUE,
            pmObHeap,
            cHeaps,
            vaHeaps64,
            0x0c,
            sizeof(VMMWIN_HEAP_SEGMENT32),
            (VMMWIN_LISTTRAVERSE_PRE_CB)VmmWinHeap_Initialize32_Pre,
            NULL,
            NULL
        );
    }
    // 4: allocate and set result
    cHeaps = ObMap_Size(pmObHeap);
    if((pObHeapMap = Ob_Alloc('HeaM', 0, sizeof(VMMOB_MAP_HEAP) + cHeaps * sizeof(VMM_MAP_HEAPENTRY), NULL, NULL))) {
        pObHeapMap->cMap = cHeaps;
        while(cHeaps) {
            cHeaps--;
            pObHeapMap->pMap[cHeaps].qwHeapData = (QWORD)ObMap_PopWithKey(pmObHeap, &pObHeapMap->pMap[cHeaps].vaHeapSegment);
        }
        qsort(pObHeapMap->pMap, pObHeapMap->cMap, sizeof(VMM_MAP_HEAPENTRY), (int(*)(const void *, const void *))VmmWinHeap_Initialize_CmpHeapEntry);
        pProcess->Map.pObHeap = pObHeapMap;
    }
    Ob_DECREF(pmObHeap);
}

/*
* Identify and scan for 64-bit heaps in a process memory space and commit the
* result to the pProcess memory map.
* NB! WINXP is not supported.
* NB! Must be called in thread-safe way.
*/
VOID VmmWinHeap_Initialize64(_In_ PVMM_PROCESS pProcess)
{
    BOOL f;
    BYTE pbPEB64[sizeof(PEB64)];
    PPEB64 pPEB64 = (PPEB64)pbPEB64;
    DWORD cHeaps;
    QWORD vaHeapPrimary, vaHeaps[0x80];
    POB_MAP pmObHeap;
    PVMMOB_MAP_HEAP pObHeapMap;
    // 1: Read PEB
    f = pProcess->win.vaPEB && VmmRead(pProcess, pProcess->win.vaPEB, pbPEB64, sizeof(PEB64));
    if(!f) { return; }
    vaHeapPrimary = pPEB64->ProcessHeap;
    cHeaps = pPEB64->NumberOfHeaps;
    // 2: Read heap array
    f = (cHeaps <= 0x80) &&
        VmmRead(pProcess, pPEB64->ProcessHeaps, (PBYTE)vaHeaps, sizeof(QWORD) * cHeaps) &&
        (vaHeaps[0] == vaHeapPrimary);
    if(!f) { return; }
    // 3: Traverse heap linked list
    if(!(pmObHeap = ObMap_New(0))) { return; }
    VmmWin_ListTraversePrefetch(
        pProcess,
        FALSE,
        pmObHeap,
        cHeaps,
        vaHeaps,
        0x18,
        sizeof(VMMWIN_HEAP_SEGMENT64),
        (VMMWIN_LISTTRAVERSE_PRE_CB)VmmWinHeap_Initialize64_Pre,
        NULL,
        NULL
    );
    // 4: allocate and set result
    cHeaps = ObMap_Size(pmObHeap);
    if((pObHeapMap = Ob_Alloc('HeaM', 0, sizeof(VMMOB_MAP_HEAP) + cHeaps * sizeof(VMM_MAP_HEAPENTRY), NULL, NULL))) {
        pObHeapMap->cMap = cHeaps;
        while(cHeaps) {
            cHeaps--;
            pObHeapMap->pMap[cHeaps].qwHeapData = (QWORD)ObMap_PopWithKey(pmObHeap, &pObHeapMap->pMap[cHeaps].vaHeapSegment);
        }
        qsort(pObHeapMap->pMap, pObHeapMap->cMap, sizeof(VMM_MAP_HEAPENTRY), (int(*)(const void *, const void *))VmmWinHeap_Initialize_CmpHeapEntry);
        pProcess->Map.pObHeap = pObHeapMap;     // pProcess take reference responsibility
    }
    Ob_DECREF(pmObHeap);
}

/*
* Initialize the meap map containing information about the process heaps in the
* specific process. This is performed by a PEB walk/scan of in-process memory
* structures. This may be unreliable if a process is obfuscated or tampered.
* -- pProcess
* -- return
*/
BOOL VmmWinHeap_Initialize(_In_ PVMM_PROCESS pProcess)
{
    if(pProcess->Map.pObHeap) { return TRUE; }
    VmmTlbSpider(pProcess);
    EnterCriticalSection(&pProcess->LockUpdate);
    if(!pProcess->Map.pObHeap) {
        if((ctxVmm->tpSystem == VMM_SYSTEM_WINDOWS_X86) || ((ctxVmm->tpSystem == VMM_SYSTEM_WINDOWS_X64) && pProcess->win.fWow64)) {
            VmmWinHeap_Initialize32(pProcess, pProcess->win.fWow64);
        } else if(ctxVmm->tpSystem == VMM_SYSTEM_WINDOWS_X64) {
            VmmWinHeap_Initialize64(pProcess);
        }
    }
    LeaveCriticalSection(&pProcess->LockUpdate);
    return pProcess->Map.pObHeap ? TRUE : FALSE;
}

// ----------------------------------------------------------------------------
// THREADING FUNCTIONALITY BELOW:
//
// The threading subsystem is dependent on loaded kernel pdb symbols and being
// initialized asynchronously at startup. i.e. it may not be immediately avail-
// able at startup time or not available at all. Loading threads may be slow
// the first time if many threads exist in a process since a list have to be
// traversed - hence functionality exists to start a load asynchronously.
// ----------------------------------------------------------------------------

typedef struct tdVMMWIN_INITIALIZETHREAD_CONTEXT {
    POB_MAP pmThread;
    POB_SET psObTeb;
    POB_SET psObTrapFrame;
    PVMM_PROCESS pProcess;
} VMMWIN_INITIALIZETHREAD_CONTEXT, *PVMMWIN_INITIALIZETHREAD_CONTEXT;

int VmmWinThread_Initialize_CmpThreadEntry(PVMM_MAP_THREADENTRY v1, PVMM_MAP_THREADENTRY v2)
{
    return
        (v1->dwTID < v2->dwTID) ? -1 :
        (v1->dwTID > v2->dwTID) ? 1 : 0;
}

VOID VmmWinThread_Initialize_DoWork_Pre(_In_ PVMM_PROCESS pSystemProcess, _In_opt_ PVMMWIN_INITIALIZETHREAD_CONTEXT ctx, _In_ QWORD va, _In_ PBYTE pb, _In_ DWORD cb, _In_ QWORD vaFLink, _In_ QWORD vaBLink, _In_ POB_SET pVSetAddress, _Inout_ PBOOL pfValidEntry, _Inout_ PBOOL pfValidFLink, _Inout_ PBOOL pfValidBLink)
{
    BOOL f, f32 = ctxVmm->f32;
    DWORD dwTID;
    PVMM_MAP_THREADENTRY e;
    PVMM_OFFSET_ETHREAD ot = &ctxVmm->offset.ETHREAD;
    // 1: sanity check
    f = ctx &&
        (f32 ? VMM_KADDR32_4(vaFLink) : VMM_KADDR64_8(vaFLink)) &&
        (f32 ? VMM_KADDR32_4(vaBLink) : VMM_KADDR64_8(vaBLink)) &&
        (!ot->oProcessOpt || (VMM_PTR_OFFSET(f32, pb, ot->oProcessOpt) == ctx->pProcess->win.EPROCESS.va)) &&
        (dwTID = (DWORD)VMM_PTR_OFFSET(f32, pb, ot->oCid + (f32 ? 4ULL : 8ULL)));
    if(!f) { return; }
    *pfValidEntry = *pfValidFLink = *pfValidBLink = TRUE;
    // 2: allocate and populate thread entry with info.
    if(!(e = LocalAlloc(LMEM_ZEROINIT, sizeof(VMM_MAP_THREADENTRY)))) { return; }
    e->vaETHREAD = va;
    e->dwTID = dwTID;
    e->dwPID = (DWORD)VMM_PTR_OFFSET(f32, pb, ot->oCid);
    e->dwExitStatus = *(PDWORD)(pb + ot->oExitStatus);
    e->bState = *(PUCHAR)(pb + ot->oState);
    e->bSuspendCount = *(PUCHAR)(pb + ot->oSuspendCount);
    if(ot->oRunningOpt) { e->bRunning = *(PUCHAR)(pb + ot->oRunningOpt); }
    e->bPriority = *(PUCHAR)(pb + ot->oPriority);
    e->bBasePriority = *(PUCHAR)(pb + ot->oBasePriority);
    e->vaTeb = VMM_PTR_OFFSET(f32, pb, ot->oTeb);
    e->ftCreateTime = *(PQWORD)(pb + ot->oCreateTime);
    e->ftExitTime = *(PQWORD)(pb + ot->oExitTime);
    e->vaStartAddress = VMM_PTR_OFFSET(f32, pb, ot->oStartAddress);
    e->vaStackBaseKernel = VMM_PTR_OFFSET(f32, pb, ot->oStackBase);
    e->vaStackLimitKernel = VMM_PTR_OFFSET(f32, pb, ot->oStackLimit);
    e->vaTrapFrame = VMM_PTR_OFFSET(f32, pb, ot->oTrapFrame);
    e->qwAffinity = VMM_PTR_OFFSET(f32, pb, ot->oAffinity);
    e->dwKernelTime = *(PDWORD)(pb + ot->oKernelTime);
    e->dwUserTime = *(PDWORD)(pb + ot->oUserTime);
    if(e->ftExitTime > 0x0200000000000000) { e->ftExitTime = 0; }
    ObSet_Push(ctx->psObTeb, e->vaTeb);
    ObSet_Push(ctx->psObTrapFrame, e->vaTrapFrame);
    ObMap_Push(ctx->pmThread, e->dwTID, e);  // map will free allocation when cleared
}

VOID VmmWinThread_Initialize_DoWork(_In_ PVMM_PROCESS pProcess)
{
    BOOL f, f32 = ctxVmm->f32;
    BYTE pb[0x200];
    DWORD i, cMap, cbTrapFrame = 0;
    QWORD va, vaThreadListEntry;
    POB_SET psObTeb = NULL, psObTrapFrame = NULL;
    POB_MAP pmObThreads = NULL;
    PVMMOB_MAP_THREAD pObThreadMap = NULL;
    PVMM_MAP_THREADENTRY pThreadEntry;
    PVMM_PROCESS pObSystemProcess = NULL;
    VMMWIN_INITIALIZETHREAD_CONTEXT ctx = { 0 };
    PVMM_OFFSET_ETHREAD ot = &ctxVmm->offset.ETHREAD;
    // 1: set up and perform list traversal call.
    vaThreadListEntry = VMM_PTR_OFFSET(f32, pProcess->win.EPROCESS.pb, ctxVmm->offset.ETHREAD.oThreadListHeadKP);
    if(f32 ? !VMM_KADDR32_4(vaThreadListEntry) : !VMM_KADDR64_8(vaThreadListEntry)) { goto fail; }
    if(!(pObSystemProcess = VmmProcessGet(4))) { goto fail; }
    if(!(psObTeb = ObSet_New())) { goto fail; }
    if(!(psObTrapFrame = ObSet_New())) { goto fail; }
    if(!(pmObThreads = ObMap_New(OB_MAP_FLAGS_OBJECT_LOCALFREE))) { goto fail; }
    ctx.pmThread = pmObThreads;
    ctx.psObTeb = psObTeb;
    ctx.psObTrapFrame = psObTrapFrame;
    ctx.pProcess = pProcess;
    va = vaThreadListEntry - ctxVmm->offset.ETHREAD.oThreadListEntry;
    VmmWin_ListTraversePrefetch(
        pObSystemProcess,
        f32,
        &ctx,
        1,
        &va,
        ctxVmm->offset.ETHREAD.oThreadListEntry,
        ctxVmm->offset.ETHREAD.oMax,
        (VMMWIN_LISTTRAVERSE_PRE_CB)VmmWinThread_Initialize_DoWork_Pre,
        NULL,
        pProcess->pObPersistent->pObCMapThreadPrefetch);
    // 2: transfer result from generic map into PVMMOB_MAP_THREAD
    if(!(cMap = ObMap_Size(pmObThreads))) { goto fail; }
    if(!(pObThreadMap = Ob_Alloc(OB_TAG_MAP_THREAD, 0, sizeof(VMMOB_MAP_THREAD) + cMap * sizeof(VMM_MAP_THREADENTRY), NULL, NULL))) { goto fail; }
    pObThreadMap->cMap = cMap;
    cbTrapFrame = ((ot->oTrapRsp < 0x200 - 8) && (ot->oTrapRip < 0x200 - 8)) ? 8 + max(ot->oTrapRsp, ot->oTrapRip) : 0;
    VmmCachePrefetchPages3(pObSystemProcess, psObTrapFrame, cbTrapFrame, 0);
    VmmCachePrefetchPages3(pProcess, psObTeb, 0x20, 0);
    for(i = 0; i < cMap; i++) {
        pThreadEntry = (PVMM_MAP_THREADENTRY)ObMap_GetByIndex(pmObThreads, i);
        // fetch Teb
        if(VmmRead2(pProcess, pThreadEntry->vaTeb, pb, 0x20, VMM_FLAG_FORCECACHE_READ)) {
            pThreadEntry->vaStackBaseUser = VMM_PTR_OFFSET_DUAL(f32, pb, 4, 8);
            pThreadEntry->vaStackLimitUser = VMM_PTR_OFFSET_DUAL(f32, pb, 8, 16);
        }
        // fetch TrapFrame (RSP/RIP)
        if(cbTrapFrame && VmmRead2(pObSystemProcess, pThreadEntry->vaTrapFrame, pb, cbTrapFrame, VMM_FLAG_FORCECACHE_READ)) {
            pThreadEntry->vaRIP = VMM_PTR_OFFSET(f32, pb, ot->oTrapRip);
            pThreadEntry->vaRSP = VMM_PTR_OFFSET(f32, pb, ot->oTrapRsp);
            f = ((pThreadEntry->vaStackBaseUser > pThreadEntry->vaRSP) && (pThreadEntry->vaStackLimitUser < pThreadEntry->vaRSP)) ||
                ((pThreadEntry->vaStackBaseKernel > pThreadEntry->vaRSP) && (pThreadEntry->vaStackLimitKernel < pThreadEntry->vaRSP));
            if(!f) {
                pThreadEntry->vaRIP = 0;
                pThreadEntry->vaRSP = 0;
            }
        }
        // commit
        memcpy(pObThreadMap->pMap + i, pThreadEntry, sizeof(VMM_MAP_THREADENTRY));
    }
    // 3: sort on thread id (TID) and assign result to process object.
    qsort(pObThreadMap->pMap, cMap, sizeof(VMM_MAP_THREADENTRY), (int(*)(const void*, const void*))VmmWinThread_Initialize_CmpThreadEntry);
    pProcess->Map.pObThread = pObThreadMap;     // pProcess take reference responsibility
fail:
    Ob_DECREF(psObTeb);
    Ob_DECREF(psObTrapFrame);
    Ob_DECREF(pmObThreads);
    Ob_DECREF(pObSystemProcess);
}

/*
* Initialize the thread map for a specific process.
* NB! The threading sub-system is dependent on pdb symbols and may take a small
* amount of time before it's available after system startup.
* -- pProcess
* -- return
*/
BOOL VmmWinThread_Initialize(_In_ PVMM_PROCESS pProcess)
{
    if(pProcess->Map.pObThread) { return TRUE; }
    if(!ctxVmm->fThreadMapEnabled) { return FALSE; }
    VmmTlbSpider(pProcess);
    EnterCriticalSection(&pProcess->Map.LockUpdateThreadExtendedInfo);
    if(!pProcess->Map.pObThread) {
        VmmWinThread_Initialize_DoWork(pProcess);
        if(!pProcess->Map.pObThread) {
            pProcess->Map.pObThread = Ob_Alloc(OB_TAG_MAP_THREAD, LMEM_ZEROINIT, sizeof(VMMOB_MAP_THREAD), NULL, NULL);
        }
    }
    LeaveCriticalSection(&pProcess->Map.LockUpdateThreadExtendedInfo);
    return pProcess->Map.pObThread ? TRUE : FALSE;
}

// ----------------------------------------------------------------------------
// HANDLE FUNCTIONALITY BELOW:
//
// The code below is responsible for parsing the HANDLE table into a map. The
// function will read the handle table and then also peek into each handle to
// determine its type. Even though parsing is generally efficient in number of
// calls quite a few memory pages may be retrieved - worst case ~1 per handle!
// ----------------------------------------------------------------------------

typedef struct tdVMMWIN_OBJECTTYPE_NAME2OBJECT_ENTRY {
    LPSTR usz;
    LPSTR sz;
} VMMWIN_OBJECTTYPE_NAME2OBJECT_ENTRY;

static const VMMWIN_OBJECTTYPE_NAME2OBJECT_ENTRY VMMWIN_OBJECTTYPE_NAME2OBJECT_ARRAY[] = {
    // NB! order and count must correspond to: VMMWIN_OBJECT_TYPE_TABLE._tpAll
    {.usz = "ALPC Port", .sz = "_ALPC_PORT"},
    {.usz = "Device", .sz = "_DEVICE_OBJECT"},
    {.usz = "Directory", .sz = "_OBJECT_DIRECTORY"},
    {.usz = "Driver", .sz = "_DRIVER_OBJECT"},
    {.usz = "Event", .sz = "_KEVENT"},
    {.usz = "File", .sz = "_FILE_OBJECT"},
    {.usz = "Job", .sz = "_EJOB"},
    {.usz = "Key", .sz = "_CM_KEY_BODY"},
    {.usz = "Mutant", .sz = "_KMUTANT"},
    {.usz = "Process", .sz = "_EPROCESS"},
    {.usz = "Section", .sz = "_SECTION"},
    {.usz = "Semaphore", .sz = "_KSEMAPHORE"},
    {.usz = "Session", .sz = "_MM_SESSION_SPACE"},
    {.usz = "SymbolicLink", .sz = "_OBJECT_SYMBOLIC_LINK"},
    {.usz = "Thread", .sz = "_ETHREAD"},
    {.usz = "Timer", .sz = "_KTIMER"},
    {.usz = "Token", .sz = "_TOKEN"},
    {.usz = "Type", .sz = "_OBJECT_TYPE"}
};

/*
* Retrieve a pointer to a VMMWIN_OBJECT_TYPE if possible. Initialization of the
* table takes place on first use. The table only exists in Win7+ and is is
* dependant on PDB symbol functionality for initialization.
* -- iObjectType
* -- return
*/
_Success_(return != NULL)
PVMMWIN_OBJECT_TYPE VmmWin_ObjectTypeGet(_In_ BYTE iObjectType)
{
    static SRWLOCK InitLockSRW = { 0 };
    BOOL f, fResult = FALSE;
    QWORD vaTypeTable = 0;
    PVMM_PROCESS pObSystemProcess = NULL;
    POB_STRMAP pObStrMap = NULL;
    DWORD i, j, cType = 2;
    QWORD ava[256];
    WORD acbwsz[256];
    BYTE pb[256 * 8];
    LPWSTR wsz;
    PQWORD pva64;
    PDWORD pva32;
    PVMMWIN_OBJECT_TYPE ptp;
    if(ctxVmm->ObjectTypeTable.fInitialized) {
        return ctxVmm->ObjectTypeTable.h[iObjectType].usz ? &ctxVmm->ObjectTypeTable.h[iObjectType] : NULL;
    }
    PDB_Initialize_WaitComplete();
    AcquireSRWLockExclusive(&InitLockSRW);
    if(ctxVmm->ObjectTypeTable.fInitialized) {
        ReleaseSRWLockExclusive(&InitLockSRW);
        return ctxVmm->ObjectTypeTable.h[iObjectType].usz ? &ctxVmm->ObjectTypeTable.h[iObjectType] : NULL;
    }
    if(!(pObSystemProcess = VmmProcessGet(4))) { goto fail; }
    if(!PDB_GetSymbolAddress(PDB_HANDLE_KERNEL, "ObTypeIndexTable", &vaTypeTable)) { goto fail; }
    if(ctxVmm->kernel.dwVersionMajor == 10) {
        if(!PDB_GetSymbolDWORD(PDB_HANDLE_KERNEL, "ObHeaderCookie", pObSystemProcess, &i)) { goto fail; }
        ctxVmm->ObjectTypeTable.bObjectHeaderCookie = (BYTE)i;
    }
    // fetch and count object type addresses
    ZeroMemory(ava, sizeof(ava));
    ZeroMemory(acbwsz, sizeof(acbwsz));
    VmmReadEx(pObSystemProcess, vaTypeTable, pb, 256 * 8, NULL, VMM_FLAG_ZEROPAD_ON_FAIL);
    if(ctxVmm->f32) {
        pva32 = (PDWORD)pb;
        while(VMM_KADDR32_8(pva32[cType]) && (cType < 256)) {
            ava[cType] = pva32[cType];
            cType++;
        }
    } else {
        pva64 = (PQWORD)pb;
        while(VMM_KADDR64_16(pva64[cType]) && (cType < 256)) {
            ava[cType] = pva64[cType];
            cType++;
        }
    }
    if(cType == 2) { goto fail; }   // none found
    // fetch unicode length and addresses of text
    VmmCachePrefetchPages4(pObSystemProcess, cType, ava, 0x10, 0);
    for(i = 2; i < cType; i++) {
        f = VmmRead2(pObSystemProcess, ava[i] + (ctxVmm->f32 ? 8 : 16), pb, 0x10, VMM_FLAG_FORCECACHE_READ);
        f = f && (*(PWORD)(pb) < MAX_PATH);
        f = f && (*(PWORD)(pb) <= *(PQWORD)(pb + 2));
        f = f && (acbwsz[i] = *(PWORD)(pb));
        f = f && (ava[i] = ctxVmm->f32 ? *(PDWORD)(pb + 4) : *(PQWORD)(pb + 8));
        f = f && (ctxVmm->f32 ? VMM_KADDR32_8(ava[i]) : VMM_KADDR64_16(ava[i]));
        if(!f) {
            ava[i] = 0;
        }
        ctxVmm->ObjectTypeTable.h[i].iType = i;
    }
    // fetch text
    wsz = (LPWSTR)(pb + 16);
    VmmCachePrefetchPages4(pObSystemProcess, cType, ava, 2 * MAX_PATH, 0);
    if(!(pObStrMap = ObStrMap_New(0))) { goto fail; }
    for(i = 2; i < cType; i++) {
        if(ava[i] && VmmRead2(pObSystemProcess, ava[i] - 16, pb, 16 + acbwsz[i], VMM_FLAG_FORCECACHE_READ) && VMM_POOLTAG_PREPENDED(pb, 16, 'ObNm')) {
            wsz[acbwsz[i] >> 1] = 0;
            ObStrMap_PushPtrWU(pObStrMap, wsz, &ctxVmm->ObjectTypeTable.h[i].usz, &ctxVmm->ObjectTypeTable.h[i].cbu);
        }
    }
    ObStrMap_FinalizeAllocU_DECREF_NULL(&pObStrMap, (PBYTE*)&ctxVmm->ObjectTypeTable.pbMultiText, &ctxVmm->ObjectTypeTable.cbMultiText);
    // specific type lookups
    for(i = 2; i < cType; i++) {
        ptp = ctxVmm->ObjectTypeTable.h + i;
        for(j = 0; j < sizeof(VMMWIN_OBJECTTYPE_NAME2OBJECT_ARRAY) / sizeof(VMMWIN_OBJECTTYPE_NAME2OBJECT_ENTRY); j++) {
            if(ptp->usz && (ptp->usz[0] == VMMWIN_OBJECTTYPE_NAME2OBJECT_ARRAY[j].usz[0]) && !strcmp(ptp->usz, VMMWIN_OBJECTTYPE_NAME2OBJECT_ARRAY[j].usz)) {
                ctxVmm->ObjectTypeTable._tpAll[j] = (BYTE)i;
                PDB_GetTypeSize(PDB_HANDLE_KERNEL, VMMWIN_OBJECTTYPE_NAME2OBJECT_ARRAY[j].sz, &ptp->cb);
                ptp->szType = VMMWIN_OBJECTTYPE_NAME2OBJECT_ARRAY[j].sz;
            }
        }
    }
    // finish!
    ctxVmm->ObjectTypeTable.c = cType;
    fResult = TRUE;
    // fall-trough to cleanup / "fail"
fail:
    ctxVmm->ObjectTypeTable.fInitialized = TRUE;
    if(!fResult) { ctxVmm->ObjectTypeTable.fInitializedFailed = TRUE; }
    ReleaseSRWLockExclusive(&InitLockSRW);
    Ob_DECREF(pObSystemProcess);
    return ctxVmm->ObjectTypeTable.h[iObjectType].usz ? &ctxVmm->ObjectTypeTable.h[iObjectType] : NULL;
}

/*
* _OBJECT_HEADER.TypeIndex is encoded on Windows 10 - this function decodes it.
* https://medium.com/@ashabdalhalim/e8f907e7073a
* -- vaObjectHeader
* -- iTypeIndexTableEncoded
* -- return
*/
BYTE VmmWin_ObjectTypeGetIndexFromEncoded(_In_ QWORD vaObjectHeader, _In_ BYTE iTypeIndexTableEncoded)
{
    if(ctxVmm->kernel.dwVersionMajor != 10) { return iTypeIndexTableEncoded; }
    if(!ctxVmm->ObjectTypeTable.fInitialized) { VmmWin_ObjectTypeGet(0); }  // DUMMY call to initialize ctxVmm->ObjectTypeTable
    if(ctxVmm->ObjectTypeTable.fInitializedFailed) { return 0; }
    return iTypeIndexTableEncoded ^ (BYTE)(vaObjectHeader >> 8) ^ ctxVmm->ObjectTypeTable.bObjectHeaderCookie;
}

typedef struct tdVMMWIN_INITIALIZE_HANDLE_CONTEXT {
    PVMM_PROCESS pSystemProcess;
    PVMM_PROCESS pProcess;
    DWORD cTables;
    DWORD cTablesMax;
    PQWORD pvaTables;
    PVMMOB_MAP_HANDLE pHandleMap;
    DWORD iMap;
} VMMWIN_INITIALIZE_HANDLE_CONTEXT, *PVMMWIN_INITIALIZE_HANDLE_CONTEXT;

/*
* Object manager callback function for object cleanup tasks.
* -- pVmmHandle
*/
VOID VmmWinHandle_CloseObCallback(_In_ PVMMOB_MAP_HANDLE pOb)
{
    LocalFree(pOb->pbMultiText);
}

/*
* Spider the handle table hierarchy if there is one.
* -- ctx
* -- vaTable
* -- fLevel2
*/
VOID VmmWinHandle_InitializeCore_SpiderTables(_In_ PVMMWIN_INITIALIZE_HANDLE_CONTEXT ctx, _In_ QWORD vaTable, _In_ BOOL fLevel2)
{
    QWORD i, va = 0;
    union {
        BYTE pb[0x1000];
        DWORD pdw[0x400];
        QWORD pqw[0x200];
    } u;
    if(!VmmRead(ctx->pSystemProcess, vaTable, u.pb, 0x1000)) { return; }
    if(ctxVmm->f32) {
        for(i = 0; i < 0x400; i++) {
            va = u.pdw[i];
            if(!VMM_KADDR32_PAGE(va)) { return; }
            if(fLevel2) {
                VmmWinHandle_InitializeCore_SpiderTables(ctx, va, FALSE);
                if(ctx->cTables == ctx->cTablesMax) { return; }
            } else {
                ctx->pvaTables[ctx->cTables] = va;
                ctx->cTables++;
                if(ctx->cTables == ctx->cTablesMax) { return; }
            }
        }
    } else {
        for(i = 0; i < 0x200; i++) {
            va = u.pqw[i];
            if(!VMM_KADDR64_PAGE(va)) { return; }
            if(fLevel2) {
                VmmWinHandle_InitializeCore_SpiderTables(ctx, va, FALSE);
                if(ctx->cTables == ctx->cTablesMax) { return; }
            } else {
                ctx->pvaTables[ctx->cTables] = va;
                ctx->cTables++;
                if(ctx->cTables == ctx->cTablesMax) { return; }
            }
        }
    }
}

/*
* Count the number of valid handles.
* -- ctx
* -- return = the number of valid handles.
*/
DWORD VmmWinHandle_InitializeCore_CountHandles(_In_ PVMMWIN_INITIALIZE_HANDLE_CONTEXT ctx)
{
    QWORD va;
    DWORD iTable, i, cHandles = 0;
    union {
        BYTE pb[0x1000];
        DWORD pdw[0x400];
        QWORD pqw[0x200];
    } u;
    VmmCachePrefetchPages4(ctx->pSystemProcess, ctx->cTables, ctx->pvaTables, 0x1000, 0);
    for(iTable = 0; iTable < ctx->cTables; iTable++) {
        if(!VmmRead(ctx->pSystemProcess, ctx->pvaTables[iTable], u.pb, 0x1000)) { continue; }
        if(ctxVmm->f32) {
            for(i = 1; i < 512; i++) {
                if(!VMM_KADDR32(u.pdw[i << 1])) { continue; }
                cHandles++;
            }
        } else {
            for(i = 1; i < 256; i++) {
                va = u.pqw[i << 1];
                if(ctxVmm->kernel.dwVersionBuild >= 9200) {     // Win8 or later
                    va = 0xffff000000000000 | (va >> 16);
                }
                if(!VMM_KADDR64(va)) { continue; }
                cHandles++;
            }
        }
    }
    return cHandles;
}

/*
* Read the handle tables and populate only basic information into the HandleMap
* i.e. data that don't require reading of the actual objects pointed to.
* -- ctx
* -- vaHandleTable
* -- dwBaseHandleId
*/
VOID VmmWinHandle_InitializeCore_ReadHandleTable(_In_ PVMMWIN_INITIALIZE_HANDLE_CONTEXT ctx, _In_ QWORD vaHandleTable, _In_ DWORD dwBaseHandleId)
{
    DWORD i;
    QWORD va;
    PVMM_MAP_HANDLEENTRY pe;
    union {
        BYTE pb[0x1000];
        DWORD pdw[0x400];
        QWORD pqw[0x200];
    } u;
    if(!VmmRead(ctx->pSystemProcess, vaHandleTable, u.pb, 0x1000)) { return; }
    if(ctxVmm->f32) {
        for(i = 1; i < 512; i++) {
            if(ctx->iMap == ctx->pHandleMap->cMap) { break; }
            va = u.pdw[i << 1] & ~3;
            if(!VMM_KADDR32(va)) { continue; }
            pe = ctx->pHandleMap->pMap + ctx->iMap;
            pe->vaObject = (va & ~7) + 0x18ULL;
            pe->dwGrantedAccess = u.pdw[(i << 1) + 1] & 0x00ffffff;
            pe->dwHandle = dwBaseHandleId + (i << 2);
            pe->dwPID = ctx->pProcess->dwPID;
            ctx->iMap++;
        }
    } else {
        for(i = 1; i < 256; i++) {
            if(ctx->iMap == ctx->pHandleMap->cMap) { break; }
            va = u.pqw[i << 1];
            if(ctxVmm->kernel.dwVersionBuild >= 9600) {         // Win8.1 or later
                va = 0xffff000000000000 | (va >> 16);
            } else if(ctxVmm->kernel.dwVersionBuild >= 9200) {  // Win8 or later
                va = 0xfffff80000000000 | (va >> 19);
            }
            if(!VMM_KADDR64(va)) { continue; }
            if(!(va & 0x000007ffffffff00)) { continue; }        // free handle
            pe = ctx->pHandleMap->pMap + ctx->iMap;
            pe->vaObject = (va & ~7) + 0x30;
            pe->dwGrantedAccess = (DWORD)u.pqw[(i << 1) + 1] & 0x00ffffff;
            pe->dwHandle = dwBaseHandleId + (i << 2);
            pe->dwPID = ctx->pProcess->dwPID;
            ctx->iMap++;
        }
    }
}

typedef struct tdVMMWIN_OBJECT_HEADER32 {
    DWORD PointerCount;
    DWORD HandleCount;
    DWORD Lock;
    BYTE TypeIndex;
    BYTE TraceFlags;
    BYTE _Flags[2];
    DWORD ObjectCreateInfo;
    DWORD SecurityDescriptor;
} VMMWIN_OBJECT_HEADER32, *PVMMWIN_OBJECT_HEADER32;

typedef struct tdVMMWIN_OBJECT_HEADER64 {
    QWORD PointerCount;
    QWORD HandleCount;
    QWORD Lock;
    BYTE TypeIndex;
    BYTE TraceFlags;
    BYTE _Flags[2];
    DWORD _Reserved;
    QWORD ObjectCreateInfo;
    QWORD SecurityDescriptor;
} VMMWIN_OBJECT_HEADER64, *PVMMWIN_OBJECT_HEADER64;

DWORD VmmWinHandle_InitializeText_GetPoolHeader2(DWORD dwPoolHeaderCandidate)
{
    CHAR i, ch;
    for(i = 0; i < 32; i = i + 8) {
        ch = (CHAR)(dwPoolHeaderCandidate >> i);
        if(ch >= 'a' && ch <= 'z') { continue; }
        if(ch >= 'A' && ch <= 'Z') { continue; }
        if(ch == ' ') { continue; }
        if((i == 24) && (ctxVmm->kernel.dwVersionBuild <= 9601)) {
            return 0x20000000 | (dwPoolHeaderCandidate & 0x00ffffff);   // last char usually A-Z in win7
        }
        return 0;
    }
    return dwPoolHeaderCandidate;
}

DWORD VmmWinHandle_InitializeText_GetPoolHeader32(_In_reads_(0x40) PBYTE pb, _Out_ PDWORD pdwOffset)
{
    DWORD dwPoolHeader, i = 0x40;
    while(i) {
        i -= 0x08;
        if((dwPoolHeader = VmmWinHandle_InitializeText_GetPoolHeader2(*(PDWORD)(pb + i + 4)))) {
            *pdwOffset = i + 4;
            return dwPoolHeader;
        }
    }
    *pdwOffset = 0;
    return 0;
}

DWORD VmmWinHandle_InitializeText_GetPoolHeader64(_In_reads_(0x60) PBYTE pb, _Out_ PDWORD pdwOffset)
{
    DWORD dwPoolHeader, i = 0x60;
    while(i) {
        i -= 0x10;
        if((dwPoolHeader = VmmWinHandle_InitializeText_GetPoolHeader2(*(PDWORD)(pb + i + 4)))) {
            *pdwOffset = i + 4;
            return dwPoolHeader;
        }
    }
    *pdwOffset = 0;
    return 0;
}

typedef struct tdVMMWINHANDLE_REGHELPER {
    QWORD vaCmKeyControlBlock;
    QWORD vaHive;
    DWORD raKeyCell;
    VMM_REGISTRY_KEY_INFO KeyInfo;
} VMMWINHANDLE_REGHELPER, *PVMMWINHANDLE_REGHELPER;

/*
* Helper function for VmmWinHandle_InitializeText_DoWork that fetches registry
* names provided that the underlying _CM_KEY_CONTROL_BLOCK is prefetched.
* -- pSystemProcess
* -- pm
*/
VOID VmmWinHandle_InitializeText_DoWork_RegKeyHelper(_In_ PVMM_PROCESS pSystemProcess, _In_ POB_MAP pm)
{
    BYTE pb[0x30];
    DWORD raCell, dwBuild = ctxVmm->kernel.dwVersionBuild;
    QWORD vaHive;
    POB_REGISTRY_KEY pObKey = NULL;
    POB_REGISTRY_HIVE pObHive = NULL;
    PVMMWINHANDLE_REGHELPER prh = NULL;
    while((prh = ObMap_GetNext(pm, prh))) {
        if(!VmmRead2(pSystemProcess, prh->vaCmKeyControlBlock, pb, 0x30, VMM_FLAG_FORCECACHE_READ)) { continue; }
        if(ctxVmm->f32) {
            if((dwBuild >= 7600) && (dwBuild <= 10586)) {
                // Win7 :: Win10_10586
                vaHive = *(PDWORD)(pb + 0x14);
                raCell = *(PDWORD)(pb + 0x18);
            } else {
                vaHive = *(PDWORD)(pb + 0x10);
                raCell = *(PDWORD)(pb + 0x14);
            }
            if(!VMM_KADDR32(vaHive)) { continue; }
        } else {
            if((dwBuild <= 6002) || ((dwBuild >= 14393) && (dwBuild <= 17763))) {
                // VISTA & Win10_1607 :: Win10_1809
                vaHive = *(PQWORD)(pb + 0x18);
                raCell = *(PDWORD)(pb + 0x20);
            } else {
                vaHive = *(PQWORD)(pb + 0x20);
                raCell = *(PDWORD)(pb + 0x28);
            }
            if(!VMM_KADDR64(vaHive)) { continue; }
        }
        if(!raCell || ((raCell & 0x7fffffff) > 0x20000000)) { continue; }
        prh->vaHive = vaHive;
        prh->raKeyCell = raCell;
    }
    while((prh = ObMap_GetNext(pm, prh))) {
        if((pObHive = VmmWinReg_HiveGetByAddress(prh->vaHive))) {
            if((pObKey = VmmWinReg_KeyGetByCellOffset(pObHive, prh->raKeyCell))) {
                VmmWinReg_KeyInfo2(pObHive, pObKey, &prh->KeyInfo);
                Ob_DECREF_NULL(&pObKey);
            }
            Ob_DECREF_NULL(&pObHive);
        }
    }
}

VOID VmmWinHandle_InitializeText_DoWork_FileSizeHelper(_In_ PVMM_PROCESS pSystemProcess, _In_ POB_SET psPrefetch, _In_ PVMMOB_MAP_HANDLE pHandleMap)
{
    BOOL f;
    QWORD i, cMax, cb, va;
    BYTE pb[0x100];
    PVMM_MAP_HANDLEENTRY pe;
    // 1: fetch, if required, _SHARED_CACHE_MAP // _CONTROL_AREA
    if(0 == ObSet_Size(psPrefetch)) { return; }
    VmmCachePrefetchPages3(pSystemProcess, psPrefetch, 0x20, 0);
    ObSet_Clear(psPrefetch);
    for(i = 0, cMax = pHandleMap->cMap; i < cMax; i++) {
        pe = pHandleMap->pMap + i;
        if(pe->tpInfoEx != HANDLEENTRY_TP_INFO_FILE) { continue; }
        if(!VmmRead2(pSystemProcess, pe->_Reserved.qw - 0x10, pb, 0x20, VMM_FLAG_FORCECACHE_READ)) { continue; }
        if(VMM_POOLTAG_PREPENDED(pb, 0x10, 'CcSc')) {
            cb = *(PQWORD)(pb + 0x10 + O_SHARED_CACHE_MAP_FileSize);
            pe->_InfoFile.cb = (cb <= 0xffffffff) ? (DWORD)cb : 0xffffffff;
            continue;
        }
        f = VMM_POOLTAG_PREPENDED(pb, 0x10, 'MmCa') &&
            (va = VMM_PTR_OFFSET(ctxVmm->f32, pb + 0x10, O_CONTROL_AREA_Segment)) &&
            VMM_KADDR_8_16(va);
        if(f) {
            pe->_Reserved.qw = va;
            ObSet_Push(psPrefetch, va - 0x10);
        }
    }
    // 2: fetch, if required, _SEGMENT
    if(0 == ObSet_Size(psPrefetch)) { return; }
    VmmCachePrefetchPages3(pSystemProcess, psPrefetch, 0x30, 0);
    for(i = 0, cMax = pHandleMap->cMap; i < cMax; i++) {
        pe = pHandleMap->pMap + i;
        if(pe->tpInfoEx != HANDLEENTRY_TP_INFO_FILE) { continue; }
        if(!VmmRead2(pSystemProcess, pe->_Reserved.qw - 0x10, pb, 0x30, VMM_FLAG_FORCECACHE_READ)) { continue; }
        if(VMM_POOLTAG_PREPENDED(pb, 0x10, 'MmSm')) {
            cb = *(PQWORD)(pb + 0x10 + (ctxVmm->f32 ? O32_SEGMENT_SizeOfSegment : O64_SEGMENT_SizeOfSegment));
            cb = (cb <= 0xffffffff) ? cb : 0xffffffff;
            pe->_InfoFile.cb = (DWORD)(pe->_InfoFile.cb ? min(pe->_InfoFile.cb, cb) : cb);
        }
    }
}

VOID VmmWinHandle_InitializeText_DoWork(_In_ PVMM_PROCESS pSystemProcess, _In_ PVMMOB_MAP_HANDLE pHandleMap)
{
    BOOL f, fThreadingEnabled;
    PBYTE pbMultiText = NULL;
    QWORD va;
    DWORD i, cbRead, oPoolHdr, cbObjectRead;
    BYTE pbBuffer[2 * MAX_PATH];
    POB_SET psObPrefetch = NULL, psObRegPrefetch = NULL;
    POB_MAP pmObRegHelperMap = NULL;
    PUNICODE_STRING32 pus32;
    PUNICODE_STRING64 pus64;
    PVMM_MAP_HANDLEENTRY pe;
    PVMM_PROCESS pObProcessHnd;
    LPSTR uszTMP;
    union {
        BYTE pb[0x1000];
        struct {
            BYTE _Filler1[0x60 - 0x18 - 0x0c];
            UNICODE_STRING32 String;
            DWORD _Filler2;
            VMMWIN_OBJECT_HEADER32 Header;
            BYTE pb[0];
        } O32;
        struct {
            BYTE _Filler1[0x90 - 0x30 - 0x18];
            UNICODE_STRING64 String;
            QWORD _Filler2;
            VMMWIN_OBJECT_HEADER64 Header;
            BYTE pb[0];
        } O64;
    } u;
    PVMMWINHANDLE_REGHELPER pRegHelp = NULL;
    POB_STRMAP psmOb = NULL;
    fThreadingEnabled = (ctxVmm->offset.ETHREAD.oCid > 0);
    cbObjectRead = max(ctxVmm->offset.EPROCESS.PID + 0x08, ctxVmm->offset.ETHREAD.oCid + 0x20);
    cbObjectRead = 0x90 + max(0x70, cbObjectRead);
    // 1: cache prefetch object data
    if(!(psObPrefetch = ObSet_New())) { goto fail; }
    if(!(psObRegPrefetch = ObSet_New())) { goto fail; }
    if(!(pmObRegHelperMap = ObMap_New(OB_MAP_FLAGS_OBJECT_LOCALFREE))) { goto fail; }
    for(i = 0; i < pHandleMap->cMap; i++) {
        ObSet_Push(psObPrefetch, pHandleMap->pMap[i].vaObject - 0x90);
    }
    VmmCachePrefetchPages3(pSystemProcess, psObPrefetch, cbObjectRead, 0);
    ObSet_Clear(psObPrefetch);
    // 2: read and interpret object data
    if(ctxVmm->f32) {
        for(i = 0; i < pHandleMap->cMap; i++) {
            pe = pHandleMap->pMap + i;
            VmmReadEx(pSystemProcess, pe->vaObject - 0x60, u.pb, cbObjectRead, &cbRead, VMM_FLAG_ZEROPAD_ON_FAIL | VMM_FLAG_FORCECACHE_READ);
            if(cbRead < 0x60) { continue; }
            // fetch and validate type index (if possible)
            pe->iType = VmmWin_ObjectTypeGetIndexFromEncoded(pe->vaObject - 0x18, u.O32.Header.TypeIndex);
            // fetch pool tag (if found)
            pe->dwPoolTag = VmmWinHandle_InitializeText_GetPoolHeader32(u.pb, &oPoolHdr);
            // fetch remaining object header values
            pe->qwHandleCount = u.O32.Header.HandleCount;
            pe->qwPointerCount = u.O32.Header.PointerCount;
            pe->vaObjectCreateInfo = u.O32.Header.ObjectCreateInfo;
            pe->vaSecurityDescriptor = u.O32.Header.SecurityDescriptor;
            // fetch text description length and address (if possible)
            if(pe->dwPoolTag) {
                pus32 = NULL;
                if((pe->dwPoolTag & 0x00ffffff) == 'yeK') {         // REG KEY
                    if(!VMM_KADDR32(*(PDWORD)(u.O32.pb + 4))) { continue; }
                    if(ObMap_ExistsKey(pmObRegHelperMap, pe->vaObject)) { continue; }
                    if(!(pRegHelp = LocalAlloc(LMEM_ZEROINIT, sizeof(VMMWINHANDLE_REGHELPER)))) { continue; }
                    pRegHelp->vaCmKeyControlBlock = *(PDWORD)(u.O32.pb + 4);
                    ObMap_Push(pmObRegHelperMap, pe->vaObject, pRegHelp);           // map is responsible for free of pRegHelp
                    ObSet_Push(psObRegPrefetch, pRegHelp->vaCmKeyControlBlock);
                } else if((pe->dwPoolTag & 0x00ffffff) == 'orP') {  // PROCESS
                    pe->_Reserved.dw = *(PDWORD)(u.O32.pb + ctxVmm->offset.EPROCESS.PID);
                } else if(((pe->dwPoolTag & 0x00ffffff) == 'rhT') && fThreadingEnabled) {   // THREAD
                    if(ctxVmm->offset.ETHREAD.oCid && *(PDWORD)(u.O32.pb + ctxVmm->offset.ETHREAD.oCid + 4)) {
                        pe->_Reserved.dw = *(PDWORD)(u.O32.pb + ctxVmm->offset.ETHREAD.oCid + 4);
                    }
                } else if((pe->dwPoolTag & 0x00ffffff) == 'liF') {  // FILE HANDLE
                    pus32 = (PUNICODE_STRING32)(u.O32.pb + O32_FILE_OBJECT_FileName);
                    if((va = *(PDWORD)(u.O32.pb + O32_FILE_OBJECT_SectionObjectPointer)) && VMM_KADDR32_4(va)) {
                        ObSet_Push(psObPrefetch, va);
                        pe->tpInfoEx = HANDLEENTRY_TP_INFO_PRE_1;
                        pe->_Reserved.qw2 = va;
                    }
                } else if(pe->dwPoolTag && (oPoolHdr <= 0x34)) {
                    pus32 = &u.O32.String;
                }
                f = pus32 && (pus32->Length > 2) &&
                    !(pus32->Length & 1) && (pus32->Length < (2 * MAX_PATH)) && (pus32->Length <= pus32->MaximumLength) &&
                    VMM_KADDR32(pus32->Buffer);
                if(f) {
                    pe->_Reserved.dw = pus32->Length;
                    pe->_Reserved.qw = pus32->Buffer;
                    ObSet_Push(psObPrefetch, pus32->Buffer);
                }
            }
        }
    } else {
        for(i = 0; i < pHandleMap->cMap; i++) {
            pe = pHandleMap->pMap + i;
            max(0x70, ctxVmm->offset.ETHREAD.oCid + 0x10);
            VmmReadEx(pSystemProcess, pe->vaObject - 0x90, u.pb, cbObjectRead, &cbRead, VMM_FLAG_ZEROPAD_ON_FAIL | VMM_FLAG_FORCECACHE_READ);
            if(cbRead < 0x90) { continue; }
            // fetch and validate type index (if possible)
            pe->iType = VmmWin_ObjectTypeGetIndexFromEncoded(pe->vaObject - 0x30, u.O64.Header.TypeIndex);
            // fetch pool tag (if found)
            pe->dwPoolTag = VmmWinHandle_InitializeText_GetPoolHeader64(u.pb, &oPoolHdr);
            // fetch remaining object header values
            pe->qwHandleCount = u.O64.Header.HandleCount;
            pe->qwPointerCount = u.O64.Header.PointerCount;
            pe->vaObjectCreateInfo = u.O64.Header.ObjectCreateInfo;
            pe->vaSecurityDescriptor = u.O64.Header.SecurityDescriptor;
            // fetch text description length and address (if possible)
            if(pe->dwPoolTag) {
                pus64 = NULL;
                if((pe->dwPoolTag & 0x00ffffff) == 'yeK') {         // REG KEY
                    if(!VMM_KADDR64(*(PQWORD)(u.O64.pb + 8))) { continue; }
                    if(ObMap_ExistsKey(pmObRegHelperMap, pe->vaObject)) { continue; }
                    if(!(pRegHelp = LocalAlloc(LMEM_ZEROINIT, sizeof(VMMWINHANDLE_REGHELPER)))) { continue; }
                    pRegHelp->vaCmKeyControlBlock = *(PQWORD)(u.O64.pb + 8);
                    ObMap_Push(pmObRegHelperMap, pe->vaObject, pRegHelp);           // map is responsible for free of pRegHelp
                    ObSet_Push(psObRegPrefetch, pRegHelp->vaCmKeyControlBlock);
                } else if((pe->dwPoolTag & 0x00ffffff) == 'orP') {  // PROCESS
                    pe->_Reserved.dw = *(PDWORD)(u.O64.pb + ctxVmm->offset.EPROCESS.PID);
                } else if(((pe->dwPoolTag & 0x00ffffff) == 'rhT') && fThreadingEnabled) {   // THREAD
                    if(ctxVmm->offset.ETHREAD.oCid && *(PDWORD)(u.O64.pb + ctxVmm->offset.ETHREAD.oCid + 8)) {
                        pe->_Reserved.dw = *(PDWORD)(u.O64.pb + ctxVmm->offset.ETHREAD.oCid + 8);
                    }
                } else if((pe->dwPoolTag & 0x00ffffff) == 'liF') {  // FILE HANDLE
                    pus64 = (PUNICODE_STRING64)(u.O64.pb + O64_FILE_OBJECT_FileName);
                    if((va = *(PQWORD)(u.O64.pb + O64_FILE_OBJECT_SectionObjectPointer)) && VMM_KADDR64_8(va)) {
                        pe->tpInfoEx = HANDLEENTRY_TP_INFO_PRE_1;
                        pe->_Reserved.qw2 = va;
                        ObSet_Push(psObPrefetch, va);
                    }

                } else if(pe->dwPoolTag && (oPoolHdr <= 0x38)) {
                    pus64 = &u.O64.String;
                }
                f = pus64 && (pus64->Length > 2) &&
                    !(pus64->Length & 1) && (pus64->Length < (2 * MAX_PATH)) && (pus64->Length <= pus64->MaximumLength) &&
                    VMM_KADDR64(pus64->Buffer);
                if(f) {
                    pe->_Reserved.dw = pus64->Length;
                    pe->_Reserved.qw = pus64->Buffer;
                    ObSet_Push(psObPrefetch, pus64->Buffer);
                }
            }
        }
    }
    // registry key retrieve names
    VmmCachePrefetchPages3(pSystemProcess, psObRegPrefetch, 0x30, 0);
    VmmWinHandle_InitializeText_DoWork_RegKeyHelper(pSystemProcess, pmObRegHelperMap);
    // create and fill text descriptions
    // also get potential _FILE_OBJECT->SectionObjectPointer->SharedCacheMap (if applicable)
    psmOb = ObStrMap_New(0);
    VmmCachePrefetchPages3(pSystemProcess, psObPrefetch, MAX_PATH * 2, 0);
    ObSet_Clear(psObPrefetch);
    for(i = 0; i < pHandleMap->cMap; i++) {
        pe = pHandleMap->pMap + i;
        if((pe->dwPoolTag & 0x00ffffff) == 'yeK') {         // REG KEY
            if((pRegHelp = ObMap_GetByKey(pmObRegHelperMap, pe->vaObject))) {
                if(pRegHelp->KeyInfo.uszName[0]) {
                    ObStrMap_PushUU_snprintf_s(psmOb, &pe->uszText, &pe->cbuText, "[%llx:%08x] %s", pRegHelp->vaHive, pRegHelp->KeyInfo.raKeyCell, pRegHelp->KeyInfo.uszName);
                } else {
                    ObStrMap_PushUU_snprintf_s(psmOb, &pe->uszText, &pe->cbuText, "[%llx:%08x]", pRegHelp->vaHive, pRegHelp->KeyInfo.raKeyCell);
                }
            }
        } else if((pe->dwPoolTag & 0x00ffffff) == 'orP') {  // PROCESS
            if((pe->_Reserved.dw < 99999) && (pObProcessHnd = VmmProcessGet(pe->_Reserved.dw))) {
                ObStrMap_PushUU_snprintf_s(psmOb, &pe->uszText, &pe->cbuText, "PID %i - %s", pObProcessHnd->dwPID, pObProcessHnd->szName);
                Ob_DECREF_NULL(&pObProcessHnd);
            }
        } else if((pe->dwPoolTag & 0x00ffffff) == 'rhT') {   // THREAD
            if(pe->_Reserved.dw && (pe->_Reserved.dw < 99999)) {
                ObStrMap_PushUU_snprintf_s(psmOb, &pe->uszText, &pe->cbuText, "TID %i", pe->_Reserved.dw);
            }
        } else if(pe->_Reserved.qw) {
            if(VmmReadWtoU(pSystemProcess, pe->_Reserved.qw, pe->_Reserved.dw, VMM_FLAG_FORCECACHE_READ, pbBuffer, sizeof(pbBuffer), &uszTMP, NULL, CHARUTIL_FLAG_TRUNCATE)) {
                ObStrMap_PushPtrUU(psmOb, uszTMP, &pe->uszText, &pe->cbuText);
            }
        }
        // Process _SECTION_OBJECT_POINTERS DataSectionObject&SharedCacheMap:
        if((pe->tpInfoEx == HANDLEENTRY_TP_INFO_PRE_1) && VmmRead2(pSystemProcess, pe->_Reserved.qw2, u.pb, 0x18, VMM_FLAG_FORCECACHE_READ)) {
            pe->_InfoFile.cb = 0;
            f = VMM_KADDR_4_8((va = VMM_PTR_OFFSET_DUAL(ctxVmm->f32, u.pb, O32_SECTION_OBJECT_POINTERS_SharedCacheMap, O64_SECTION_OBJECT_POINTERS_SharedCacheMap))) ||
                VMM_KADDR_4_8((va = VMM_PTR_OFFSET_DUAL(ctxVmm->f32, u.pb, O32_SECTION_OBJECT_POINTERS_DataSectionObject, O64_SECTION_OBJECT_POINTERS_DataSectionObject)));
            if(f) {
                pe->_Reserved.qw = va;
                pe->tpInfoEx = HANDLEENTRY_TP_INFO_FILE;
                ObSet_Push(psObPrefetch, va - 0x10);
            }
        }
    }
    // retrieve (if applicable) file sizes
    VmmWinHandle_InitializeText_DoWork_FileSizeHelper(pSystemProcess, psObPrefetch, pHandleMap);
    // finish
    ObStrMap_FinalizeAllocU_DECREF_NULL(&psmOb, &pHandleMap->pbMultiText, &pHandleMap->cbMultiText);
    for(i = 0; i < pHandleMap->cMap; i++) {
        pe = pHandleMap->pMap + i;
        if(!pe->uszText) {
            pe->cbuText = 1;
            pe->uszText = (LPSTR)pHandleMap->pbMultiText;
        }
    }
fail:
    Ob_DECREF(psObPrefetch);
    Ob_DECREF(psObRegPrefetch);
    Ob_DECREF(pmObRegHelperMap);
    Ob_DECREF(psmOb);
}

VOID VmmWinHandle_InitializeCore_DoWork(_In_ PVMM_PROCESS pSystemProcess, _In_ PVMM_PROCESS pProcess)
{
    BOOL fResult = FALSE;
    BOOL f32 = ctxVmm->f32;
    BYTE pb[0x20], iLevel;
    WORD oTableCode;
    DWORD i, cHandles, iHandleMap = 0;
    QWORD vaHandleTable = 0, vaTableCode = 0;
    VMMWIN_INITIALIZE_HANDLE_CONTEXT ctx = { 0 };
    PVMMOB_MAP_HANDLE pObHandleMap = NULL;
    ctx.pSystemProcess = pSystemProcess;
    ctx.pProcess = pProcess;
    vaHandleTable = VMM_PTR_OFFSET(f32, pProcess->win.EPROCESS.pb, ctxVmm->offset.EPROCESS.ObjectTable);
    if(!VMM_KADDR(vaHandleTable) || !VmmRead(pSystemProcess, vaHandleTable - 0x10, pb, 0x20)) { return; }
    if(!VMM_POOLTAG_PREPENDED(pb, 0x10, 'Obtb') && !VMM_KADDR_PAGE(vaHandleTable)) { return; }
    oTableCode = (ctxVmm->kernel.dwVersionBuild < 9200) ? 0 : 8;    // WinXP::Win7 -> 0, otherwise 8.
    vaTableCode = VMM_PTR_OFFSET(f32, pb + 0x10, oTableCode) & ~7;
    iLevel = VMM_PTR_OFFSET(f32, pb + 0x10, oTableCode) & 7;
    if((iLevel > 2) || !VMM_KADDR_PAGE(vaTableCode)) { return; }
    ctx.cTablesMax = f32 ? 1024 : 512;
    ctx.cTablesMax = iLevel ? ((iLevel == 1) ? (ctx.cTablesMax * ctx.cTablesMax) : ctx.cTablesMax) : 1;
    if(!(ctx.pvaTables = LocalAlloc(0, ctx.cTablesMax * sizeof(QWORD)))) { return; }
    if(iLevel) {
        VmmWinHandle_InitializeCore_SpiderTables(&ctx, vaTableCode, (iLevel == 2));
    } else {
        ctx.cTables = 1;
        ctx.pvaTables[0] = vaTableCode;
    }
    // count handles and allocate map
    if(!(cHandles = VmmWinHandle_InitializeCore_CountHandles(&ctx))) { goto fail; }
    cHandles = min(cHandles, 256 * 1024);
    ctx.pHandleMap = pObHandleMap = Ob_Alloc(OB_TAG_MAP_HANDLE, LMEM_ZEROINIT, sizeof(VMMOB_MAP_HANDLE) + cHandles * sizeof(VMM_MAP_HANDLEENTRY), (OB_CLEANUP_CB)VmmWinHandle_CloseObCallback, NULL);
    if(!pObHandleMap) { goto fail; }
    pObHandleMap->cMap = cHandles;
    // walk handle tables to fill map with core handle information
    for(i = 0; i < ctx.cTables; i++) {
        VmmWinHandle_InitializeCore_ReadHandleTable(&ctx, ctx.pvaTables[i], i * (f32 ? 2048 : 1024));
    }
    pObHandleMap->cMap = ctx.iMap;
    pProcess->Map.pObHandle = Ob_INCREF(pObHandleMap);
fail:
    LocalFree(ctx.pvaTables);
    Ob_DECREF(pObHandleMap);
}

_Success_(return)
BOOL VmmWinHandle_InitializeCore(_In_ PVMM_PROCESS pProcess)
{
    PVMM_PROCESS pObSystemProcess;
    if(pProcess->Map.pObHandle) { return TRUE; }
    EnterCriticalSection(&pProcess->LockUpdate);
    if(!pProcess->Map.pObHandle && (pObSystemProcess = VmmProcessGet(4))) {
        VmmWinHandle_InitializeCore_DoWork(pObSystemProcess, pProcess);
        if(!pProcess->Map.pObHandle) {
            pProcess->Map.pObHandle = Ob_Alloc(OB_TAG_MAP_HANDLE, LMEM_ZEROINIT, sizeof(VMMOB_MAP_HANDLE), (OB_CLEANUP_CB)VmmWinHandle_CloseObCallback, NULL);
        }
        Ob_DECREF(pObSystemProcess);
    }
    LeaveCriticalSection(&pProcess->LockUpdate);
    return pProcess->Map.pObHandle ? TRUE : FALSE;
}

_Success_(return)
BOOL VmmWinHandle_InitializeText(_In_ PVMM_PROCESS pProcess)
{
    PVMM_PROCESS pObSystemProcess;
    if(pProcess->Map.pObHandle->pbMultiText) { return TRUE; }
    EnterCriticalSection(&pProcess->Map.LockUpdateThreadExtendedInfo);
    if(!pProcess->Map.pObHandle->pbMultiText && (pObSystemProcess = VmmProcessGet(4))) {
        VmmWinHandle_InitializeText_DoWork(pObSystemProcess, pProcess->Map.pObHandle);
        Ob_DECREF(pObSystemProcess);
    }
    LeaveCriticalSection(&pProcess->Map.LockUpdateThreadExtendedInfo);
    return pProcess->Map.pObHandle->pbMultiText ? TRUE : FALSE;
}

/*
* Initialize Handles for a specific process. Extended information text may take
* extra time to initialize.
* -- pProcess
* -- fExtendedText = also fetch extended info such as handle paths/names.
* -- return
*/
_Success_(return)
BOOL VmmWinHandle_Initialize(_In_ PVMM_PROCESS pProcess, _In_ BOOL fExtendedText)
{
    if(pProcess->Map.pObHandle && (!fExtendedText || pProcess->Map.pObHandle->pbMultiText)) { return TRUE; }
    return VmmWinHandle_InitializeCore(pProcess) && (!fExtendedText || VmmWinHandle_InitializeText(pProcess));
}

// ----------------------------------------------------------------------------
// PHYSICAL MEMORY MAP FUNCTIONALITY BELOW:
//
// The physical memory map functionality is responsible for retrieving the
// physical memory map from the Windows registry (if possible).
// ----------------------------------------------------------------------------

#pragma pack(push, 1) /* DISABLE STRUCT PADDINGS (REENABLE AFTER STRUCT DEFINITIONS) */
typedef struct tdVMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE32 {
    UCHAR Type;
    UCHAR ShareDisposition;
    USHORT Flags;
    QWORD pa;
    DWORD cb;
} VMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE32, *PVMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE32;

typedef struct tdVMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE64 {
    UCHAR Type;
    UCHAR ShareDisposition;
    USHORT Flags;
    QWORD pa;
    QWORD cb;
} VMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE64, *PVMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE64;
#pragma pack(pop) /* RE-ENABLE STRUCT PADDINGS */

/*
* Retrieve the physical memory by parsing the registry. This is only used as a
* fallback in case it cannot be parsed from kernel due to the extra overhead
* by parsing the registry hardware hive.
* -- return
*/
_Success_(return != NULL)
PVMMOB_MAP_PHYSMEM VmmWinPhysMemMap_InitializeFromRegistry_DoWork()
{
    BOOL f32 = ctxVmm->f32;
    DWORD cMap, cbData = 0;
    PBYTE pbData = NULL;
    QWORD c1, i, o;
    PVMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE32 pMR32;
    PVMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE64 pMR64;
    PVMMOB_MAP_PHYSMEM pObPhysMemMap = NULL;
    // 1: fetch binary data from registry
    if(!VmmWinReg_ValueQuery2("HKLM\\HARDWARE\\RESOURCEMAP\\System Resources\\Physical Memory\\.Translated", NULL, NULL, 0, &cbData) || !cbData) { goto fail; }
    if(!(pbData = LocalAlloc(0, cbData))) { goto fail; }
    if(!VmmWinReg_ValueQuery2("HKLM\\HARDWARE\\RESOURCEMAP\\System Resources\\Physical Memory\\.Translated", NULL, pbData, cbData, &cbData)) { goto fail; }
    if(cbData < (DWORD)(f32 ? 0x18 : 0x28)) { goto fail; }
    // 2: fetch number of memory regions and allocate map object.
    c1 = *(PQWORD)pbData;
    if(!c1) { goto fail; }
    o = 0x10;
    cMap = *(PDWORD)(pbData + o); // this should be loop in case of c1 > 1, but works for now ...
    if(f32 && (!cMap || (cbData < cMap * sizeof(VMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE32) + 0x0c))) { goto fail; }
    if(!f32 && (!cMap || (cbData < cMap * sizeof(VMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE64) + 0x14))) { goto fail; }
    if(!(pObPhysMemMap = Ob_Alloc(OB_TAG_MAP_PHYSMEM, LMEM_ZEROINIT, sizeof(VMMOB_MAP_PHYSMEM) + cMap * sizeof(VMM_MAP_PHYSMEMENTRY), NULL, NULL))) { goto fail; }
    pObPhysMemMap->cMap = cMap;
    // 3: iterate over the memory regions.
    o += sizeof(DWORD);
    for(i = 0; i < cMap; i++) {
        if(f32) {
            pMR32 = (PVMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE32)(pbData + o + i * sizeof(VMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE32));
            pObPhysMemMap->pMap[i].pa = pMR32->pa;
            pObPhysMemMap->pMap[i].cb = pMR32->cb;
            if(pMR32->Flags & 0xff00) {
                pObPhysMemMap->pMap[i].cb = pObPhysMemMap->pMap[i].cb << 8;
            }
        } else {
            pMR64 = (PVMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE64)(pbData + o + i * sizeof(VMMWIN_PHYSMEMMAP_REGISTRY_MEMORY_RANGE64));
            pObPhysMemMap->pMap[i].pa = pMR64->pa;
            pObPhysMemMap->pMap[i].cb = pMR64->cb;
            if(pMR64->Flags & 0xff00) {
                pObPhysMemMap->pMap[i].cb = pObPhysMemMap->pMap[i].cb << 8;
            }
        }
        if((pObPhysMemMap->pMap[i].pa & 0xfff) || (pObPhysMemMap->pMap[i].cb & 0xfff)) { goto fail; }
    }
    LocalFree(pbData);
    return pObPhysMemMap;
fail:
    Ob_DECREF(pObPhysMemMap);
    LocalFree(pbData);
    return NULL;
}

/*
* Retrieve the physical memory map from the kernel by parsing the kernel symbol
* 'MmPhysicalMemoryBlock'. This is the preferred way of fetching the memory map
* due to better efficiency as compared to fallback - parsing from registry.
* -- return
*/
_Success_(return != NULL)
PVMMOB_MAP_PHYSMEM VmmWinPhysMemMap_InitializeFromKernel_DoWork()
{
    QWORD i, c, vaPhysicalMemoryBlock = 0;
    _PHYSICAL_MEMORY_DESCRIPTOR32 Md32;
    _PHYSICAL_MEMORY_DESCRIPTOR64 Md64;
    PVMM_PROCESS pObSystemProcess = NULL;
    PVMMOB_MAP_PHYSMEM pObMemMap = NULL;
    if(!(pObSystemProcess = VmmProcessGet(4))) { goto fail; }
    if(!PDB_GetSymbolPTR(PDB_HANDLE_KERNEL, "MmPhysicalMemoryBlock", pObSystemProcess, (PVOID)&vaPhysicalMemoryBlock)) { goto fail; }
    if(!VMM_KADDR_4_8(vaPhysicalMemoryBlock)) { goto fail; }
    if(ctxVmm->f32) {
        if(!VmmRead2(pObSystemProcess, vaPhysicalMemoryBlock, (PBYTE)&Md32, sizeof(_PHYSICAL_MEMORY_DESCRIPTOR32), VMMDLL_FLAG_ZEROPAD_ON_FAIL)) { goto fail; }
        if(!Md32.NumberOfRuns || (Md32.NumberOfRuns > _PHYSICAL_MEMORY_MAX_RUNS)) { goto fail; }
        if(!(pObMemMap = Ob_Alloc(OB_TAG_MAP_PHYSMEM, LMEM_ZEROINIT, sizeof(VMMOB_MAP_PHYSMEM) + Md32.NumberOfRuns * sizeof(VMM_MAP_PHYSMEMENTRY), NULL, NULL))) { goto fail; }
        pObMemMap->cMap = Md32.NumberOfRuns;
        for(i = 0, c = 0; i < Md32.NumberOfRuns; i++) {
            pObMemMap->pMap[i].pa = (QWORD)Md32.Run[i].BasePage << 12;
            pObMemMap->pMap[i].cb = (QWORD)Md32.Run[i].PageCount << 12;
            c += Md32.Run[i].PageCount;
            if(i && ((pObMemMap->pMap[i - 1].pa + pObMemMap->pMap[i - 1].cb) > pObMemMap->pMap[i].pa)) { goto fail; }
        }
        if(c != Md32.NumberOfPages) { goto fail; }
    } else {
        if(!VmmRead2(pObSystemProcess, vaPhysicalMemoryBlock, (PBYTE)&Md64, sizeof(_PHYSICAL_MEMORY_DESCRIPTOR64), VMMDLL_FLAG_ZEROPAD_ON_FAIL)) { goto fail; }
        if(!Md64.NumberOfRuns || (Md64.NumberOfRuns > _PHYSICAL_MEMORY_MAX_RUNS)) { goto fail; }
        if(!(pObMemMap = Ob_Alloc(OB_TAG_MAP_PHYSMEM, LMEM_ZEROINIT, sizeof(VMMOB_MAP_PHYSMEM) + Md64.NumberOfRuns * sizeof(VMM_MAP_PHYSMEMENTRY), NULL, NULL))) { goto fail; }
        pObMemMap->cMap = Md64.NumberOfRuns;
        for(i = 0, c = 0; i < Md64.NumberOfRuns; i++) {
            pObMemMap->pMap[i].pa = Md64.Run[i].BasePage << 12;
            pObMemMap->pMap[i].cb = Md64.Run[i].PageCount << 12;
            c += Md64.Run[i].PageCount;
            if(i && ((pObMemMap->pMap[i-1].pa + pObMemMap->pMap[i-1].cb) > pObMemMap->pMap[i].pa)) { goto fail; }
        }
        if(c != Md64.NumberOfPages) { goto fail; }
    }
    Ob_INCREF(pObMemMap);
fail:
    Ob_DECREF(pObSystemProcess);
    return Ob_DECREF(pObMemMap);
}

/*
* Create a physical memory map and assign to the global context upon success.
* CALLER DECREF: return
* -- return
*/
PVMMOB_MAP_PHYSMEM VmmWinPhysMemMap_Initialize()
{
    PVMMOB_MAP_PHYSMEM pObPhysMem;
    if((pObPhysMem = ObContainer_GetOb(ctxVmm->pObCMapPhysMem))) { return pObPhysMem; }
    EnterCriticalSection(&ctxVmm->LockUpdateMap);
    if((pObPhysMem = ObContainer_GetOb(ctxVmm->pObCMapPhysMem))) {
        LeaveCriticalSection(&ctxVmm->LockUpdateMap);
        return pObPhysMem;
    }
    pObPhysMem = VmmWinPhysMemMap_InitializeFromKernel_DoWork();
    if(!pObPhysMem) {     // fallback to parsing registry (if error on no loaded symbols)
        pObPhysMem = VmmWinPhysMemMap_InitializeFromRegistry_DoWork();
    }
    if(!pObPhysMem) {
        pObPhysMem = Ob_Alloc(OB_TAG_MAP_PHYSMEM, LMEM_ZEROINIT, sizeof(VMMOB_MAP_PHYSMEM), NULL, NULL);
    }
    ObContainer_SetOb(ctxVmm->pObCMapPhysMem, pObPhysMem);
    LeaveCriticalSection(&ctxVmm->LockUpdateMap);
    return pObPhysMem;
}

/*
* Refresh the physical memory map.
*/
VOID VmmWinPhysMemMap_Refresh()
{
    ObContainer_SetOb(ctxVmm->pObCMapPhysMem, NULL);
}

// ----------------------------------------------------------------------------
// USER FUNCTIONALITY BELOW:
//
// The user functionality is responsible for creating the user map consisting
// of non-built-in users and also for retrieving account names for SIDs - both
// well known and system-specific.
// ----------------------------------------------------------------------------

#ifdef _WIN32
/*
* Retrieve the account name of the user account given a SID.
* NB! Names for well known SIDs will be given in the language of the system
* running MemProcFS and not in the name of the analyzed system.
* -- pSID
* -- uszName
* -- cbuName
* -- fAccountWellKnown
* -- return
*/
_Success_(return)
BOOL VmmWinUser_GetName(_In_opt_ PSID pSID, _Out_writes_(cbuName) LPSTR uszName, _In_ DWORD cbuName, _Out_opt_ PBOOL pfAccountWellKnown)
{
    BOOL f;
    SID_NAME_USE eUse;
    DWORD i, cszNameBuffer = MAX_PATH, cszDomainBuffer = MAX_PATH, dwHashSID;
    CHAR szNameBuffer[MAX_PATH+1], szDomainBuffer[MAX_PATH+1];
    LPSTR szSID = NULL;
    PVMMOB_MAP_USER pObUser = NULL;
    if(!pSID) { return FALSE; }
    if(pfAccountWellKnown) { *pfAccountWellKnown = FALSE; }
    // 1: Try lookup name from User Map
    if(!ConvertSidToStringSidA(pSID, &szSID)) { return FALSE; }
    dwHashSID = Util_HashStringA(szSID);
    LocalFree(szSID);
    if(VmmMap_GetUser(&pObUser)) {
        for(i = 0; i < pObUser->cMap; i++) {
            if(dwHashSID != pObUser->pMap[i].dwHashSID) { continue; }
            // user entry located
            CharUtil_UtoU(pObUser->pMap[i].uszText, -1, (PBYTE)uszName, cbuName, NULL, NULL, CHARUTIL_FLAG_TRUNCATE_ONFAIL_NULLSTR | CHARUTIL_FLAG_STR_BUFONLY);
            Ob_DECREF(pObUser);
            return TRUE;
        }
        Ob_DECREF_NULL(&pObUser);
    }
    // 2: Try lookup name from Well Known SID
    f = LookupAccountSidA(NULL, pSID, szNameBuffer, &cszNameBuffer, szDomainBuffer, &cszDomainBuffer, &eUse);
    if(cszDomainBuffer != MAX_PATH) {
        return f && CharUtil_AtoU(szNameBuffer, -1, (PBYTE)uszName, cbuName, NULL, NULL, CHARUTIL_FLAG_TRUNCATE | CHARUTIL_FLAG_STR_BUFONLY);
    }
    return FALSE;
}

/*
* Object manager callback function for object cleanup tasks.
* -- pVmmUserMap
*/
VOID VmmWinUser_CloseObCallback(_In_ PVOID pVmmUserMap)
{
    PVMMOB_MAP_USER pOb = (PVMMOB_MAP_USER)pVmmUserMap;
    PVMM_MAP_USERENTRY pe;
    DWORD i;
    for(i = 0; i < pOb->cMap; i++) {
        if((pe = pOb->pMap + i)) {
            LocalFree(pe->pSID);
            LocalFree(pe->szSID);
        }
    }
    LocalFree(pOb->pbMultiText);
}

/*
* Create a user map and assign it to the ctxVmm global context upon success.
* NB! function must be called in single-threaded context.
* CALLER DECREF: return
* -- return
*/
PVMMOB_MAP_USER VmmWinUser_Initialize_DoWork()
{
    BOOL f;
    BYTE pbBuffer[MAX_PATH];
    typedef struct tdVMMWINUSER_CONTEXT_ENTRY {
        PSID pSID;
        DWORD cbSID;
        LPSTR szSID;
        DWORD dwHashSID;
        QWORD vaHive;
        CHAR uszUser[MAX_PATH];
    } VMMWINUSER_CONTEXT_ENTRY, *PVMMWINUSER_CONTEXT_ENTRY;
    DWORD i, dwType;
    LPSTR szNtdat, szUser;
    LPWSTR wszSymlinkSid, wszSymlinkUser;
    WCHAR wszSymlinkValue[MAX_PATH];
    POB_REGISTRY_HIVE pObHive = NULL;
    POB_SET pObSet = NULL;
    PVMMWINUSER_CONTEXT_ENTRY e = NULL;
    PVMMOB_MAP_USER pObMapUser = NULL;
    PVMM_MAP_USERENTRY pe;
    POB_STRMAP psmOb = NULL;
    if(!(pObSet = ObSet_New())) { goto fail; }
    // 1: user hive enumeration
    while((pObHive = VmmWinReg_HiveGetNext(pObHive))) {
        if(!e && !(e = LocalAlloc(0, sizeof(VMMWINUSER_CONTEXT_ENTRY)))) { continue; }
        ZeroMemory(e, sizeof(VMMWINUSER_CONTEXT_ENTRY));
        szUser = StrStrIA(pObHive->uszName, "-USER_S-");
        szNtdat = StrStrIA(pObHive->uszName, "-ntuserdat-");
        if(!szNtdat && !szUser) { continue; }
        if(!szUser && !StrStrIA(szNtdat, "-unknown")) { continue; }
        if(szUser && ((strlen(szUser) < 20) || StrStrIA(szUser, "Classes"))) { continue; }
        // get username
        f = VmmWinReg_ValueQuery1(pObHive, "ROOT\\Volatile Environment\\USERNAME", &dwType, NULL, pbBuffer, sizeof(pbBuffer) - 2, NULL, 0) &&
            (dwType == REG_SZ) &&
            CharUtil_WtoU((LPWSTR)pbBuffer, -1, e->uszUser, sizeof(e->uszUser), NULL, NULL, CHARUTIL_FLAG_TRUNCATE | CHARUTIL_FLAG_STR_BUFONLY);
        if(!f && (ctxVmm->kernel.dwVersionBuild > 2600)) { continue; }  // allow missing USERNAME only if WinXP
        // get sid
        if(szUser) {
            ConvertStringSidToSidA(szUser + 6, &e->pSID);
        }
        if(!e->pSID) {
            i = 0;
            ZeroMemory(wszSymlinkValue, sizeof(wszSymlinkValue));
            if(!VmmWinReg_ValueQuery1(pObHive, "ROOT\\Software\\Classes\\SymbolicLinkValue", &dwType, NULL, (PBYTE)wszSymlinkValue, sizeof(wszSymlinkValue) - 2, NULL, 0) || (dwType != REG_LINK)) { continue; }
            if(!(wszSymlinkSid = wcsstr(wszSymlinkValue, L"\\S-"))) { continue; }
            if(wcslen(wszSymlinkSid) < 20) { continue; }
            while(wszSymlinkSid[i] && (wszSymlinkSid[i] != L'_') && ++i);
            wszSymlinkSid[i] = 0;
            if(!ConvertStringSidToSidW(wszSymlinkSid + 1, &e->pSID) || !e->pSID) { continue; }
        }
        // get username - WinXP only
        if(!e->uszUser[0]) {
            i = 0;
            wszSymlinkUser = wszSymlinkValue + 10;
            while(wszSymlinkUser[i] && (wszSymlinkUser[i] != L'\\') && ++i);
            if(i == 0) { continue; }
            wszSymlinkUser[i] = 0;
            CharUtil_WtoU(wszSymlinkUser, -1, e->uszUser, sizeof(e->uszUser), NULL, NULL, CHARUTIL_FLAG_TRUNCATE | CHARUTIL_FLAG_STR_BUFONLY);
        }
        // get length and hash of sid string
        e->vaHive = pObHive->vaCMHIVE;
        e->cbSID = GetLengthSid(e->pSID);
        if(!e->cbSID || !ConvertSidToStringSidA(e->pSID, &e->szSID) || !e->szSID) {
            LocalFree(e->pSID);
            continue;
        }
        e->dwHashSID = Util_HashStringA(e->szSID);
        // store context in map
        ObSet_Push(pObSet, (QWORD)e);
        e = NULL;
    }
    LocalFree(e);
    // 2: create user map and assign data
    if(!(psmOb = ObStrMap_New(0))) { goto fail; }
    if(!(pObMapUser = Ob_Alloc(OB_TAG_MAP_USER, LMEM_ZEROINIT, sizeof(VMMOB_MAP_USER) + ObSet_Size(pObSet) * sizeof(VMM_MAP_USERENTRY), VmmWinUser_CloseObCallback, NULL))) { goto fail; }
    pObMapUser->cMap = ObSet_Size(pObSet);
    for(i = 0; i < pObMapUser->cMap; i++) {
        if(!(e = (PVMMWINUSER_CONTEXT_ENTRY)ObSet_Pop(pObSet))) { goto fail; }
        pe = pObMapUser->pMap + i;
        pe->pSID = e->pSID;
        pe->cbSID = e->cbSID;
        pe->szSID = e->szSID;
        pe->dwHashSID = e->dwHashSID;
        pe->vaRegHive = e->vaHive;
        // strmap below:
        ObStrMap_PushPtrUU(psmOb, e->uszUser, &pe->uszText, &pe->cbuText);
        LocalFree(e);
    }
    // finish & return
    ObStrMap_FinalizeAllocU_DECREF_NULL(&psmOb, &pObMapUser->pbMultiText, &pObMapUser->cbMultiText);
    Ob_DECREF(pObSet);
    return pObMapUser;
fail:
    Ob_DECREF(pObMapUser);
    Ob_DECREF(psmOb);
    if(pObSet) {
        while((e = (PVMMWINUSER_CONTEXT_ENTRY)ObSet_Pop(pObSet))) {
            LocalFree(e->pSID);
            LocalFree(e->szSID);
            LocalFree(e);
        }
        Ob_DECREF(pObSet);
    }
    return NULL;
}

/*
* Create a user map and assign to the global context upon success.
* CALLER DECREF: return
* -- return
*/
PVMMOB_MAP_USER VmmWinUser_Initialize()
{
    PVMMOB_MAP_USER pObUser;
    if((pObUser = ObContainer_GetOb(ctxVmm->pObCMapUser))) { return pObUser; }
    EnterCriticalSection(&ctxVmm->LockUpdateMap);
    if((pObUser = ObContainer_GetOb(ctxVmm->pObCMapUser))) {
        LeaveCriticalSection(&ctxVmm->LockUpdateMap);
        return pObUser;
    }
    pObUser = VmmWinUser_Initialize_DoWork();
    if(!pObUser) {
        pObUser = Ob_Alloc(OB_TAG_MAP_USER, LMEM_ZEROINIT, sizeof(VMMOB_MAP_USER), NULL, NULL);
    }
    ObContainer_SetOb(ctxVmm->pObCMapUser, pObUser);
    LeaveCriticalSection(&ctxVmm->LockUpdateMap);
    return pObUser;
}

#endif /* _WIN32 */
#ifdef LINUX
_Success_(return) BOOL VmmWinUser_GetName(_In_opt_ PSID pSID, _Out_writes_(cbuName) LPSTR uszName, _In_ DWORD cbuName, _Out_opt_ PBOOL pfAccountWellKnown) { return FALSE; }
PVMMOB_MAP_USER VmmWinUser_Initialize() { return NULL; }
#endif /* LINUX */

/*
* Refresh the user map.
*/
VOID VmmWinUser_Refresh()
{
    ObContainer_SetOb(ctxVmm->pObCMapUser, NULL);
}

// ----------------------------------------------------------------------------
// WINDOWS EPROCESS WALKING FUNCTIONALITY FOR 64/32 BIT BELOW:
// ----------------------------------------------------------------------------

#define VMMPROC_EPROCESS64_MAX_SIZE       0x800
#define VMMPROC_EPROCESS32_MAX_SIZE       0x480

VOID VmmWinProcess_OffsetLocator_Print()
{
    PVMM_OFFSET_EPROCESS po = &ctxVmm->offset.EPROCESS;
    VMMLOG_LEVEL dwLogLevel = po->fValid ? LOGLEVEL_DEBUG : LOGLEVEL_WARNING;
    VmmLog(MID_PROCESS, dwLogLevel, "OK: %s",
        (po->fValid ? "TRUE" : "FALSE"));
    VmmLog(MID_PROCESS, dwLogLevel, "    PID:  %03x PPID: %03x STAT: %03x DTB:  %03x DTBU: %03x NAME: %03x PEB: %03x",
        po->PID, po->PPID, po->State, po->DTB, po->DTB_User, po->Name, po->PEB);
    VmmLog(MID_PROCESS, dwLogLevel, "    FLnk: %03x BLnk: %03x oMax: %03x SeAu: %03x VadR: %03x ObjT: %03x WoW: %03x",
        po->FLink, po->BLink, po->cbMaxOffset, po->SeAuditProcessCreationInfo, po->VadRoot, po->ObjectTable, po->Wow64Process);
}

VOID VmmWinProcess_OffsetLocator_SetMaxOffset()
{
    PVMM_OFFSET_EPROCESS po = &ctxVmm->offset.EPROCESS;
    WORD o;
    o = max(po->opt.CreateTime, po->opt.ExitTime);
    o = max(max(o, po->State), max(po->DTB, po->DTB_User));
    o = max(max(o, po->Name), max(po->PID, po->PPID));
    o = max(max(o, po->PEB), max(po->FLink, po->BLink));
    o = max(max(o, po->SeAuditProcessCreationInfo), max(po->VadRoot, po->ObjectTable));
    po->cbMaxOffset = o + 0x80;
}

/*
* Fallback solution to use debug symbols to locate offsets within the EPROCESS struct.
* This is more resilient - but also add a slow dependency on the symbol server so only
* use this as a fallback for now.
*/
VOID VmmWinProcess_OffsetLocatorSYMSERV(_In_ PVMM_PROCESS pSystemProcess)
{
    PVMM_OFFSET_EPROCESS po = &ctxVmm->offset.EPROCESS;
    InfoDB_Initialize();
    PDB_Initialize(NULL, FALSE);
    PDB_GetTypeChildOffsetShort(PDB_HANDLE_KERNEL, "_DISPATCHER_HEADER", "SignalState", &po->State);
    PDB_GetTypeChildOffsetShort(PDB_HANDLE_KERNEL, "_KPROCESS", "DirectoryTableBase", &po->DTB);
    PDB_GetTypeChildOffsetShort(PDB_HANDLE_KERNEL, "_KPROCESS", "UserDirectoryTableBase", &po->DTB_User);
    PDB_GetTypeChildOffsetShort(PDB_HANDLE_KERNEL, "_EPROCESS", "ImageFileName", &po->Name);
    PDB_GetTypeChildOffsetShort(PDB_HANDLE_KERNEL, "_EPROCESS", "UniqueProcessId", &po->PID);
    PDB_GetTypeChildOffsetShort(PDB_HANDLE_KERNEL, "_EPROCESS", "InheritedFromUniqueProcessId", &po->PPID);
    PDB_GetTypeChildOffsetShort(PDB_HANDLE_KERNEL, "_EPROCESS", "ActiveProcessLinks", &po->FLink);
    po->BLink = po->FLink + ctxVmm->f32 ? 4 : 8;
    PDB_GetTypeChildOffsetShort(PDB_HANDLE_KERNEL, "_EPROCESS", "Peb", &po->PEB);
    PDB_GetTypeChildOffsetShort(PDB_HANDLE_KERNEL, "_EPROCESS", "SeAuditProcessCreationInfo", &po->SeAuditProcessCreationInfo);
    PDB_GetTypeChildOffsetShort(PDB_HANDLE_KERNEL, "_EPROCESS", "VadRoot", &po->VadRoot);
    PDB_GetTypeChildOffsetShort(PDB_HANDLE_KERNEL, "_EPROCESS", "ObjectTable", &po->ObjectTable);
    if(!ctxVmm->f32) {
        if(po->Name < po->PEB) {
            po->f64VistaOr7 = TRUE;
            po->Wow64Process = po->Name + 0x40;     // Vista, Win7
        } else {
            po->Wow64Process = po->PEB + 0x30;      // Win8, Win10
        }
    }
    PDB_GetTypeSizeShort(PDB_HANDLE_KERNEL, "_EPROCESS", &po->cbMaxOffset);
    po->fValid = po->State && po->DTB && po->Name && po->PPID && po->FLink && po->PEB && po->VadRoot && po->SeAuditProcessCreationInfo && po->ObjectTable;
}

/*
* Very ugly hack that tries to locate some offsets required within the EPROCESS struct.
*/
VOID VmmWinProcess_OffsetLocator64(_In_ PVMM_PROCESS pSystemProcess)
{
    PVMM_OFFSET_EPROCESS po = &ctxVmm->offset.EPROCESS;
    BOOL f;
    WORD i, j, cLoopProtect;
    QWORD va1, vaPEB, paPEB, vaP, oP;
    BYTE pbSYSTEM[VMMPROC_EPROCESS64_MAX_SIZE], pbSMSS[VMMPROC_EPROCESS64_MAX_SIZE], pb1[VMMPROC_EPROCESS64_MAX_SIZE], pbPage[0x1000];
    BYTE pbZero[0x800];
    QWORD paMax, paDTB_0, paDTB_1;
    POB_SET psObOff = NULL, psObVa = NULL;
    ZeroMemory(po, sizeof(VMM_OFFSET_EPROCESS));
    if(!VmmRead(pSystemProcess, pSystemProcess->win.EPROCESS.va, pbSYSTEM, VMMPROC_EPROCESS64_MAX_SIZE)) { return; }
    VmmLogHexAsciiEx(MID_PROCESS, LOGLEVEL_DEBUG, pbSYSTEM, VMMPROC_EPROCESS64_MAX_SIZE, 0, "SYSTEM DTB: %016llx EPROCESS: %016llx", pSystemProcess->paDTB, pSystemProcess->win.EPROCESS.va);
    // find offset State (static for now)
    if(*(PDWORD)(pbSYSTEM + 0x04)) { return; }
    po->State = 0x04;
    // find offset PML4 (static for now)
    if(pSystemProcess->paDTB != (0xfffffffffffff000 & *(PQWORD)(pbSYSTEM + 0x28))) { return; }
    po->DTB = 0x28;
    // find offset for Name
    for(i = 0, f = FALSE; i < VMMPROC_EPROCESS64_MAX_SIZE - 8; i += 8) {
        if(*(PQWORD)(pbSYSTEM + i) == 0x00006D6574737953) {
            po->Name = i;
            f = TRUE;
            break;
        }
    }
    if(!f) { return; }
    // find offset for PID, FLinkAll, BLink (assumed to be following eachother)
    for(i = 0, f = FALSE; i < VMMPROC_EPROCESS64_MAX_SIZE - 8; i += 8) {
        if(*(PQWORD)(pbSYSTEM + i) == 4) {
            // PID = correct, this is a candidate
            if(0xffff000000000000 != (0xffff000000000003 & *(PQWORD)(pbSYSTEM + i + 8))) { continue; }    // FLinkAll not valid kernel pointer
            va1 = *(PQWORD)(pbSYSTEM + i + 8) - i - 8;
            f = VmmRead(pSystemProcess, va1, pb1, VMMPROC_EPROCESS64_MAX_SIZE);
            if(!f) { continue; }
            f = FALSE;
            if((*(PQWORD)(pb1 + po->Name) != 0x6578652e73736d73) &&    // smss.exe
                (*(PQWORD)(pb1 + po->Name) != 0x7972747369676552) &&   // Registry
                (*(PQWORD)(pb1 + po->Name) != 0x5320657275636553))     // Secure System
            {
                continue;
            }
            if((*(PQWORD)(pb1 + i + 16) - i - 8) != pSystemProcess->win.EPROCESS.va) {
                continue;
            }
            po->PID = i;
            po->FLink = i + 8;
            po->BLink = i + 16;
            f = TRUE;
            break;
        }
    }
    if(!f) { return; }
    // find and read smss.exe
    {
        cLoopProtect = 0;
        memcpy(pbSMSS, pbSYSTEM, VMMPROC_EPROCESS64_MAX_SIZE);
        while(++cLoopProtect < 8) {
            va1 = *(PQWORD)(pbSMSS + po->FLink) - po->FLink;
            f = VmmRead(pSystemProcess, va1, pbSMSS, VMMPROC_EPROCESS64_MAX_SIZE) &&
                (*(PQWORD)(pbSMSS + po->Name) == 0x6578652e73736d73);
            if(f) { break; }
        }
        if(!f) { return; }
        VmmLogHexAsciiEx(MID_PROCESS, LOGLEVEL_DEBUG, pbSMSS, VMMPROC_EPROCESS64_MAX_SIZE, 0, "EPROCESS smss.exe BELOW:");
    }
    // find offset for ParentPid (_EPROCESS!InheritedFromUniqueProcessId)
    // (parent pid is assumed to be located between BLink and Name
    {
        for(i = po->BLink; i < po->Name; i += 8) {
            if((*(PQWORD)(pbSYSTEM + i) == 0) && (*(PQWORD)(pbSMSS + i) == 4)) {
                po->PPID = i;
                break;
            }
        }
        if(!po->PPID) { return; }
    }
    // find offset for PEB (in EPROCESS) by comparing SYSTEM and SMSS  [or other process on fail - max 4 tries]
    {
        for(j = 0; j < 4; j++) {
            for(i = 0x280, f = FALSE; i < 0x580; i += 8) {
                if(*(PQWORD)(pbSYSTEM + i)) { continue; }
                vaPEB = *(PQWORD)(pbSMSS + i);
                if(!vaPEB || (vaPEB & 0xffff800000000fff)) { continue; }
                // Verify potential PEB
                if(!VmmVirt2PhysEx(*(PQWORD)(pbSMSS + po->DTB), TRUE, vaPEB, &paPEB)) { continue; }
                if(!VmmReadPage(NULL, paPEB, pbPage)) { continue; }
                if(*(PWORD)pbPage == 0x5a4d) { continue; }  // MZ header -> likely entry point or something not PEB ...
                po->PEB = i;
                f = TRUE;
                break;
            }
            if(f) { break; }
            // failed locating PEB (paging?) -> try next process in EPROCESS list.
            va1 = *(PQWORD)(pbSMSS + po->FLink) - po->FLink;
            if(!VmmRead(pSystemProcess, va1, pbSMSS, VMMPROC_EPROCESS64_MAX_SIZE)) { return; }
        }
        if(!f) { return; }
    }
    // Wow64Process offset - "static" rule.
    {
        if(po->Name < po->PEB) {
            po->f64VistaOr7 = TRUE;
            po->Wow64Process = po->Name + 0x40;     // Vista, Win7
        } else {
            po->Wow64Process = po->PEB + 0x30;      // Win8, Win10
        }
    }
    // locate various offsets primarily by reading pointers and checking pool
    // headers in an efficient way (minimize number of reads).
    {
        if(!(psObVa = ObSet_New())) { goto fail; }
        if(!(psObOff = ObSet_New())) { goto fail; }
        // ObjectTable candidate pointers
        for(i = po->Name - 0x0e0; i < po->Name - 0x020; i += 8) {
            vaP = *(PQWORD)(pbSYSTEM + i);
            if(VMM_KADDR64_16(vaP) && !ObSet_Exists(psObVa, vaP - 0x10)) {
                ObSet_Push(psObOff, (i << 16) | 1);
                ObSet_Push(psObVa, vaP - 0x10);
            }
        }
        // SeAuditProcessCreationInfo candidate pointers by looking at SMSS.
        // Offset is located between PEB+0x058 and PEB+0x070 as observed so far.
        // Look at some extra offsets just in case for the future.
        for(i = 0x058 + po->PEB; i < 0x090 + po->PEB; i += 8) {
            vaP = *(PQWORD)(pbSMSS + i);
            if(VMM_KADDR64_8(vaP) && !ObSet_Exists(psObVa, vaP)) {
                ObSet_Push(psObOff, (i << 16) | 2);
                ObSet_Push(psObVa, vaP);
            }
        }
        // prefetch result into cache
        VmmCachePrefetchPages3(pSystemProcess, psObVa, 0x40, 0);
        // interpret result
        while(ObSet_Size(psObVa)) {
            oP = ObSet_Pop(psObOff);
            vaP = ObSet_Pop(psObVa);
            if(!VmmRead2(pSystemProcess, vaP, pbPage, 0x40, VMM_FLAG_FORCECACHE_READ)) {
                if(((vaP + 0x10) & 0xfff) || !VmmRead2(pSystemProcess, vaP + 0x10, pbPage + 0x10, 0x30, VMM_FLAG_FORCECACHE_READ)) {
                    continue;
                }
            }
            // ObjectTable
            f = (1 == (oP & 0xff)) && (*(PDWORD)(pbPage + 4) == 0x6274624f);  // Pool Header: Obtb
            if(f) { po->ObjectTable = (WORD)(oP >> 16); }
            f = (1 == (oP & 0xff)) && VMM_KADDR64_PAGE(vaP + 0x10) && !*(PQWORD)(pbPage + 0x10 + 0x10) && VMM_KADDR64_8(*(PQWORD)(pbPage + 0x10 + 0x18)) && VMM_KADDR64_8(*(PQWORD)(pbPage + 0x10 + 0x20));     // page-align (no pool hdr)
            if(f) { po->ObjectTable = (WORD)(oP >> 16); }
            // SeAuditProcessCreationInfo
            f = (2 == (oP & 0xff)) &&
                (*(PQWORD)(pbPage + 0x10) == 0x007600650044005C) && (*(PQWORD)(pbPage + 0x18) == 0x005C006500630069) && // L"\Device\"
                (*(PWORD)(pbPage + 0x00) < MAX_PATH) && (*(PWORD)(pbPage + 0x00) < *(PWORD)(pbPage + 0x02));            // _UNICODE_STRING length
            if(f) { po->SeAuditProcessCreationInfo = (WORD)(oP >> 16); }
        }
        // check validity
        if(!po->ObjectTable) { goto fail; }
        if(!po->SeAuditProcessCreationInfo) { goto fail; }
    }
    // find offset for VadRoot by searching for ExitStatus value assumed to be
    // set to: 0x00000103 and existing prior to VadRoot by -12(VISTA)/-4(Win7+)
    // Value may be the 'VadHint' on some systems; scan back 0x40 to find any
    // identical match (which will be assumed to be vadroot).
    {
        for(i = 0x140 + po->Name; i < 0x7f0; i += 8) {
            f = VMM_KADDR64(*(PQWORD)(pbSYSTEM + i)) && ((*(PDWORD)(pbSYSTEM + i - 4) == 0x00000103) || (*(PDWORD)(pbSYSTEM + i - 12) == 0x00000103));
            if(f) { break; }
        }
        if(!f) { goto fail; }
        po->VadRoot = i;
        // Scanback 0x40 (in case of 'VadHint' false positive.
        for(i = po->VadRoot - 8; i > po->VadRoot - 0x40; i -= 8) {
            if(*(PQWORD)(pbSYSTEM + i) == *(PQWORD)(pbSYSTEM + po->VadRoot)) {
                po->VadRoot = i;
                break;
            }
        }
    }
    // find "optional" offset for user cr3/pml4 (post meltdown only)
    // System have an entry pointing to a shadow PML4 which has empty user part
    // smss.exe do not have an entry since it's running as admin ...
    {
        ZeroMemory(pbZero, 0x800);
        paMax = ctxMain->dev.paMax;
        for(i = 0x240; i < VMMPROC_EPROCESS64_MAX_SIZE - 8; i += 8) {
            paDTB_0 = *(PQWORD)(pbSYSTEM + i);
            paDTB_1 = *(PQWORD)(pbSMSS + i);
            f = !(paDTB_1 & ~1) &&
                paDTB_0 &&
                !(paDTB_0 & 0xffe) &&
                (paDTB_0 < paMax) &&
                VmmReadPage(NULL, (paDTB_0 & ~0xfff), pbPage) &&
                !memcmp(pbPage, pbZero, 0x800) &&
                VmmTlbPageTableVerify(pbPage, (paDTB_0 & ~0xfff), TRUE);
            if(f) {
                po->DTB_User = i;
                break;
            }
        }
    }
    VmmWinProcess_OffsetLocator_SetMaxOffset();
    po->fValid = TRUE;
fail:
    Ob_DECREF(psObVa);
    Ob_DECREF(psObOff);
}

/*
* Post-process new process in the "new" process table before they are comitted VmmProcessCreateFinish()
* At this moment "only" the full path and name is retrieved by using 'SeAuditProcessCreationInfo'.
* -- pSystemProcess
*/
VOID VmmWinProcess_Enumerate_PostProcessing(_In_ PVMM_PROCESS pSystemProcess)
{
    DWORD i;
    LPSTR uszPathKernel;
    LPWSTR wszPathKernel;
    POB_SET pObPrefetchAddr = NULL;
    PVMM_PROCESS pObProcess = NULL;
    PVMMOB_PROCESS_TABLE ptObCurrent = NULL, ptObNew = NULL;
    PVMMOB_PROCESS_PERSISTENT pProcPers;
    if(!(pObPrefetchAddr = ObSet_New())) { goto fail; }
    if(!(ptObCurrent = (PVMMOB_PROCESS_TABLE)ObContainer_GetOb(ctxVmm->pObCPROC))) { goto fail; }
    if(!(ptObNew = (PVMMOB_PROCESS_TABLE)ObContainer_GetOb(ptObCurrent->pObCNewPROC))) { goto fail; }
    // 1: Iterate to gather memory locations of "SeAuditProcessCreationInfo" / "kernel path" for new processes
    while((pObProcess = VmmProcessGetNextEx(ptObNew, pObProcess, VMM_FLAG_PROCESS_SHOW_TERMINATED))) {
        if(!pObProcess->pObPersistent->fIsPostProcessingComplete) {
            ObSet_Push_PageAlign(pObPrefetchAddr, VMM_EPROCESS_PTR(pObProcess, ctxVmm->offset.EPROCESS.SeAuditProcessCreationInfo), 540);
        }
    }
    if(0 == ObSet_Size(pObPrefetchAddr)) { goto fail; }
    VmmCachePrefetchPages(pSystemProcess, pObPrefetchAddr, 0);
    // 2: Fetch "kernel path" and set "long name" for new processes.
    while((pObProcess = VmmProcessGetNextEx(ptObNew, pObProcess, VMM_FLAG_PROCESS_SHOW_TERMINATED))) {
        pProcPers = pObProcess->pObPersistent;
        if(!pProcPers->fIsPostProcessingComplete) {
            pProcPers->fIsPostProcessingComplete = TRUE;
            uszPathKernel = NULL;
            if(VmmReadAllocUnicodeString(pSystemProcess, ctxVmm->f32, VMM_FLAG_FORCECACHE_READ, VMM_EPROCESS_PTR(pObProcess, ctxVmm->offset.EPROCESS.SeAuditProcessCreationInfo), 0x400, &wszPathKernel, NULL)) {
                if(CharUtil_WtoU(wszPathKernel, 0x400, NULL, 0, &uszPathKernel, NULL, CHARUTIL_FLAG_ALLOC)) {
                    if(memcmp(uszPathKernel, "\\Device\\", 8)) {
                        LocalFree(uszPathKernel); uszPathKernel = NULL;
                    }
                }
                LocalFree(wszPathKernel); wszPathKernel = NULL;
            }
            if(!uszPathKernel) {
                // Fail - use EPROCESS name
                if(!(uszPathKernel = LocalAlloc(LMEM_ZEROINIT, 16))) { continue; }
                for(i = 0; i < 15; i++) {
                    uszPathKernel[i] = pObProcess->szName[i];
                }
            }
            pProcPers->uszPathKernel = uszPathKernel;
            pProcPers->cuszPathKernel = (WORD)strlen(pProcPers->uszPathKernel);
            // locate FullName by skipping to last \ character.
            pProcPers->uszNameLong = CharUtil_PathSplitLast(pProcPers->uszPathKernel);
            pProcPers->cuszNameLong = (WORD)strlen(pProcPers->uszNameLong);
        }
    }
fail:
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObPrefetchAddr);
    Ob_DECREF(ptObCurrent);
    Ob_DECREF(ptObNew);
}

typedef struct tdVMMWIN_ENUMERATE_EPROCESS_CONTEXT {
    DWORD cProc;
    BOOL fTotalRefresh;
    BOOL fNoLinkEPROCESS;
    DWORD cNewProcessCollision;
    POB_SET pObSetPrefetchDTB;
} VMMWIN_ENUMERATE_EPROCESS_CONTEXT, *PVMMWIN_ENUMERATE_EPROCESS_CONTEXT;

VOID VmmWinProcess_Enum64_Pre(_In_ PVMM_PROCESS pProcess, _In_opt_ PVMMWIN_ENUMERATE_EPROCESS_CONTEXT ctx, _In_ QWORD va, _In_ PBYTE pb, _In_ DWORD cb, _In_ QWORD vaFLink, _In_ QWORD vaBLink, _In_ POB_SET pVSetAddress, _Inout_ PBOOL pfValidEntry, _Inout_ PBOOL pfValidFLink, _Inout_ PBOOL pfValidBLink)
{
    if(!ctx || !VMM_KADDR64_16(va)) { return; }
    ObSet_Push(ctx->pObSetPrefetchDTB, *(PQWORD)(pb + ctxVmm->offset.EPROCESS.DTB) & ~0xfff);
    *pfValidFLink = VMM_KADDR64_8(vaFLink);
    *pfValidBLink = VMM_KADDR64_8(vaBLink);
    *pfValidEntry = *pfValidFLink || *pfValidBLink;
}

VOID VmmWinProcess_Enum64_Post(_In_ PVMM_PROCESS pSystemProcess, _In_opt_ PVMMWIN_ENUMERATE_EPROCESS_CONTEXT ctx, _In_ QWORD va, _In_ PBYTE pb, _In_ DWORD cb)
{
    PVMM_OFFSET_EPROCESS po = &ctxVmm->offset.EPROCESS;
    PQWORD pqwDTB, pqwDTB_User, pqwPEB, pqwWow64Process;
    PDWORD pdwState, pdwPID, pdwPPID;
    LPSTR szName;
    BOOL fUser;
    PVMM_PROCESS pObProcess = NULL;
    if(!ctx || !VMM_KADDR64_16(va)) { return; }
    pdwState = (PDWORD)(pb + po->State);
    pdwPID = (PDWORD)(pb + po->PID);
    pdwPPID = (PDWORD)(pb + po->PPID);
    pqwDTB = (PQWORD)(pb + po->DTB);
    pqwDTB_User = (PQWORD)(pb + po->DTB_User);
    szName = (LPSTR)(pb + po->Name);
    pqwPEB = (PQWORD)(pb + po->PEB);
    pqwWow64Process = (PQWORD)(pb + po->Wow64Process);
    if(*pqwDTB & 0xfffff00000000000) { return; }   // NB! Fail if target system have more than 16TB of memory (unlikely)
    if(ctx->pObSetPrefetchDTB) {    // prefetch any physical pages in ctx->pObSetPrefetchDTB on 1st run only
        VmmCachePrefetchPages(NULL, ctx->pObSetPrefetchDTB, 0);
        Ob_DECREF_NULL(&ctx->pObSetPrefetchDTB);
    }
    if(*pdwPID && *pqwDTB && *(PQWORD)szName) {
        // treat csrss.exe as 'kernel' due to win32k mapping missing in System Process _AND_ treat MemCompression as 'user'
        fUser =
            !((*pdwPID == 4) || ((*pdwState == 0) && (*pqwPEB == 0)) || (*(PQWORD)szName == 0x78652e7373727363)) ||     // csrss.exe
            ((*(PQWORD)(szName + 0x00) == 0x72706d6f436d654d) && (*(PDWORD)(szName + 0x08) == 0x69737365));             // MemCompression "process"
        pObProcess = VmmProcessCreateEntry(
            ctx->fTotalRefresh,
            *pdwPID,
            *pdwPPID,
            *pdwState,
            ~0xfff & *pqwDTB,
            po->DTB_User ? (~0xfff & *pqwDTB_User) : 0,
            szName,
            fUser,
            pb,
            cb);
        if(!pObProcess) {
            VmmLog(MID_PROCESS, LOGLEVEL_VERBOSE, "WARNING: PID '%i' already exists or bad DTB", *pdwPID);
            if(++ctx->cNewProcessCollision >= 8) {
                return;
            }
        }
    }
    if(pObProcess) {
        pObProcess->win.EPROCESS.va = va;
        pObProcess->win.EPROCESS.fNoLink = ctx->fNoLinkEPROCESS;
        // PEB
        if(*pqwPEB & 0xfff) {
            VmmLog(MID_PROCESS, LOGLEVEL_VERBOSE, "WARNING: Bad PEB alignment for PID: '%i' (0x%016llx)", *pdwPID, *pqwPEB);
        } else {
            pObProcess->win.vaPEB = *pqwPEB;
        }
        // WoW64 and PEB32
        if(*pqwWow64Process) {
            pObProcess->win.fWow64 = TRUE;
            if(*pqwWow64Process & 0xffffffff00000fff) {
                pObProcess->win.vaPEB32 = (DWORD)*pqwPEB + (po->f64VistaOr7 ? -0x1000 : +0x1000);
            } else {
                pObProcess->win.vaPEB32 = (DWORD)*pqwWow64Process;
            }
        }
    } else {
        szName[14] = 0; // in case of bad string data ...
    }
    VmmLog(MID_PROCESS, LOGLEVEL_DEBUG, "%04i (%s) %08x %012llx %016llx %012llx %s",
        ctx->cProc,
        !pObProcess ? "skip" : (pObProcess->dwState ? "exit" : "list"),
        *pdwPID,
        ~0xfff & *pqwDTB,
        va,
        *pqwPEB,
        szName);
    Ob_DECREF_NULL(&pObProcess);
    ctx->cProc++;
}

/*
* Process an optional set of no-link eprocess into the process list which is
* undergoing a fetch.
*/
VOID VmmWinProcess_Enum_AddNoLink(
    _In_ PVMM_PROCESS pSystemProcess,
    _In_opt_ POB_SET psvaNoLinkEPROCESS,
    _In_ PVMMWIN_ENUMERATE_EPROCESS_CONTEXT ctx,
    _In_ VOID(*pfnCallback_Post)(_In_ PVMM_PROCESS pProcess, _In_opt_ PVOID ctx, _In_ QWORD va, _In_reads_(cb) PBYTE pb, _In_ DWORD cb)
) {
    QWORD va;
    BYTE pb[0x1000];
    DWORD cb = ctxVmm->offset.EPROCESS.cbMaxOffset;
    ctx->fNoLinkEPROCESS = TRUE;
    while((va = ObSet_Pop(psvaNoLinkEPROCESS))) {
        if(VmmRead(pSystemProcess, va, pb, cb)) {
            pfnCallback_Post(pSystemProcess, ctx, va, pb, cb);
        }
    }
}

/*
* Try walk the EPROCESS list in the Windows kernel to enumerate processes into
* the VMM/PROC file system.
* NB! This may be done to refresh an existing PID cache hence migration code.
* -- pSystemProcess
* -- fTotalRefresh
* -- psvaNoLinkEPROCESS = optional list of non-linked EPROCESS va's.
* -- return
*/
BOOL VmmWinProcess_Enum64(_In_ PVMM_PROCESS pSystemProcess, _In_ BOOL fTotalRefresh, _In_opt_ POB_SET psvaNoLinkEPROCESS)
{
    PVMM_OFFSET_EPROCESS po = &ctxVmm->offset.EPROCESS;
    VMMWIN_ENUMERATE_EPROCESS_CONTEXT ctx = { 0 };
    // retrieve offsets
    if(!po->fValid) {
        VmmWinProcess_OffsetLocator64(pSystemProcess);
        VmmWinProcess_OffsetLocator_Print();
        if(!po->fValid) {
            VmmLog(MID_PROCESS, LOGLEVEL_INFO, "Unable to fuzz EPROCESS offsets - trying debug symbols");
            VmmWinProcess_OffsetLocatorSYMSERV(pSystemProcess);
        }
        if(!po->fValid) {
            VmmLog(MID_PROCESS, LOGLEVEL_CRITICAL, "Unable to locate EPROCESS offsets");
            return FALSE;
        }
    }
    VmmLog(MID_PROCESS, LOGLEVEL_DEBUG, "SYSTEM DTB: %016llx EPROCESS: %016llx", pSystemProcess->paDTB, pSystemProcess->win.EPROCESS.va);
    // set up context
    ctx.fTotalRefresh = fTotalRefresh;
    if(!(ctx.pObSetPrefetchDTB = ObSet_New())) { return FALSE; }
    // traverse EPROCESS linked list
    VmmLog(MID_PROCESS, LOGLEVEL_DEBUG, "   # STATE  PID      DTB          EPROCESS         PEB          NAME");
    VmmWin_ListTraversePrefetch(
        pSystemProcess,
        FALSE,
        &ctx,
        1,
        &pSystemProcess->win.EPROCESS.va,
        ctxVmm->offset.EPROCESS.FLink,
        ctxVmm->offset.EPROCESS.cbMaxOffset,
        (VMMWIN_LISTTRAVERSE_PRE_CB)VmmWinProcess_Enum64_Pre,
        (VMMWIN_LISTTRAVERSE_POST_CB)VmmWinProcess_Enum64_Post,
        ctxVmm->pObCCachePrefetchEPROCESS);
    // add no-link entries (if any)
    VmmWinProcess_Enum_AddNoLink(
        pSystemProcess,
        psvaNoLinkEPROCESS,
        &ctx,
        (VOID(*)(PVMM_PROCESS, PVOID, QWORD, PBYTE, DWORD))VmmWinProcess_Enum64_Post);
    // set resulting prefetch cache
    Ob_DECREF_NULL(&ctx.pObSetPrefetchDTB);
    VmmWinProcess_Enumerate_PostProcessing(pSystemProcess);
    VmmProcessCreateFinish();
    return (ctx.cProc > 10);
}

/*
* Very ugly hack that tries to locate some offsets required withn the EPROCESS struct.
*/
VOID VmmWinProcess_OffsetLocator32(_In_ PVMM_PROCESS pSystemProcess)
{
    PVMM_OFFSET_EPROCESS po = &ctxVmm->offset.EPROCESS;
    BOOL f;
    WORD i, j, cLoopProtect;
    DWORD va1, vaPEB, vaP, oP;
    QWORD paPEB;
    BYTE pbSYSTEM[VMMPROC_EPROCESS32_MAX_SIZE], pbSMSS[VMMPROC_EPROCESS32_MAX_SIZE], pb1[VMMPROC_EPROCESS32_MAX_SIZE], pbPage[0x1000];
    //BYTE pbZero[0x800]
    //QWORD paMax, paDTB_0, paDTB_1;
    POB_SET psObOff = NULL, psObVa = NULL;
    ZeroMemory(po, sizeof(VMM_OFFSET_EPROCESS));
    if(!VmmRead(pSystemProcess, pSystemProcess->win.EPROCESS.va, pbSYSTEM, VMMPROC_EPROCESS32_MAX_SIZE)) { return; }
    VmmLogHexAsciiEx(MID_PROCESS, LOGLEVEL_DEBUG, pbSYSTEM, VMMPROC_EPROCESS32_MAX_SIZE, 0, "SYSTEM DTB: %016llx EPROCESS: %016llx", pSystemProcess->paDTB, pSystemProcess->win.EPROCESS.va);
    // find offset State (static for now)
    if(*(PDWORD)(pbSYSTEM + 0x04)) { return; }
    po->State = 0x04;
    // find offset PML4 (static for now)
    po->DTB = 0x18;
    // find offset for Name
    for(i = 0, f = FALSE; i < VMMPROC_EPROCESS32_MAX_SIZE - 4; i += 4) {
        if(*(PQWORD)(pbSYSTEM + i) == 0x00006D6574737953) {
            po->Name = i;
            f = TRUE;
            break;
        }
    }
    if(!f) { return; }
    // find offset for PID, FLinkAll, BLink (assumed to be following eachother)
    for(i = 0, f = FALSE; i < VMMPROC_EPROCESS32_MAX_SIZE - 4; i += 4) {
        if(*(PDWORD)(pbSYSTEM + i) == 4) {
            // PID = correct, this is a candidate
            if(0x80000000 != (0x80000003 & *(PDWORD)(pbSYSTEM + i + 4))) { continue; }    // FLinkAll not valid kernel pointer
            va1 = *(PDWORD)(pbSYSTEM + i + 4) - i - 4;
            f = VmmRead(pSystemProcess, va1, pb1, VMMPROC_EPROCESS32_MAX_SIZE);
            if(!f) { continue; }
            f = FALSE;
            if((*(PQWORD)(pb1 + po->Name) != 0x6578652e73736d73) &&    // smss.exe
                (*(PQWORD)(pb1 + po->Name) != 0x7972747369676552) &&   // Registry
                (*(PQWORD)(pb1 + po->Name) != 0x5320657275636553))     // Secure System
            {
                continue;
            }
            if((*(PDWORD)(pb1 + i + 8) - i - 4) != pSystemProcess->win.EPROCESS.va) {
                continue;
            }
            po->PID = i;
            po->FLink = i + 4;
            po->BLink = i + 8;
            f = TRUE;
            break;
        }
    }
    if(!f) { return; }
    // find and read smss.exe
    {
        cLoopProtect = 0;
        memcpy(pbSMSS, pbSYSTEM, VMMPROC_EPROCESS32_MAX_SIZE);
        while(++cLoopProtect < 8) {
            va1 = *(PDWORD)(pbSMSS + po->FLink) - po->FLink;
            f = VmmRead(pSystemProcess, va1, pbSMSS, VMMPROC_EPROCESS32_MAX_SIZE) &&
                (*(PQWORD)(pbSMSS + po->Name) == 0x6578652e73736d73);
            if(f) { break; }
        }
        if(!f) { return; }
        VmmLogHexAsciiEx(MID_PROCESS, LOGLEVEL_DEBUG, pbSMSS, VMMPROC_EPROCESS32_MAX_SIZE, 0, "EPROCESS smss.exe BELOW:");
    }
    // find offset for ParentPid (_EPROCESS!InheritedFromUniqueProcessId)
    // (parent pid is assumed to be located between BLink and Name
    {
        for(i = po->BLink; i < po->Name; i += 4) {
            if((*(PDWORD)(pbSYSTEM + i) == 0) && (*(PDWORD)(pbSMSS + i) == 4)) {
                po->PPID = i;
                break;
            }
        }
        if(!po->PPID) { return; }
    }
    // find offset for PEB (in EPROCESS) by comparing SYSTEM and SMSS  [or other process on fail - max 4 tries]
    {
        for(j = 0; j < 4; j++) {
            for(i = 0x100, f = FALSE; i < 0x240; i += 4) {
                if(*(PDWORD)(pbSYSTEM + i)) { continue; }
                vaPEB = *(PDWORD)(pbSMSS + i);
                if(!vaPEB || (vaPEB & 0x80000fff)) { continue; }
                // Verify potential PEB
                if(!VmmVirt2PhysEx(*(PDWORD)(pbSMSS + po->DTB), TRUE, vaPEB, &paPEB)) { continue; }
                if(!VmmReadPage(NULL, paPEB, pbPage)) { continue; }
                if(*(PWORD)pbPage == 0x5a4d) { continue; }  // MZ header -> likely entry point or something not PEB ...
                po->PEB = i;
                f = TRUE;
                break;
            }
            if(f) { break; }
            // failed locating PEB (paging?) -> try next process in EPROCESS list.
            va1 = *(PDWORD)(pbSMSS + po->FLink) - po->FLink;
            if(!VmmRead(pSystemProcess, va1, pbSMSS, VMMPROC_EPROCESS32_MAX_SIZE)) { return; }
        }
        if(!f) { return; }
    }
    // locate various offsets primarily by reading pointers and checking pool
    // headers in an efficient way (minimize number of reads).
    {
        if(!(psObVa = ObSet_New())) { goto fail; }
        if(!(psObOff = ObSet_New())) { goto fail; }
        // ObjectTable candidate pointers
        for(i = po->Name - 0x0c0; i < po->Name - 0x010; i += 4) {
            vaP = *(PDWORD)(pbSYSTEM + i);
            if(VMM_KADDR32_8(vaP) && !ObSet_Exists(psObVa, vaP - 0x10)) {
                ObSet_Push(psObOff, (i << 16) | 1);
                ObSet_Push(psObVa, vaP - 0x10);
            }
        }
        // SeAuditProcessCreationInfo candidate pointers by looking at SMSS.
        // Offset is located between PEB+0x040 and PEB+0x058 as observed so far.
        // Look at some extra offsets just in case for the future.
        for(i = po->PEB + 0x040; i < po->PEB + 0x058; i += 4) {
            vaP = *(PDWORD)(pbSMSS + i);
            if(VMM_KADDR32_4(vaP) && !ObSet_Exists(psObVa, vaP)) {
                ObSet_Push(psObOff, (i << 16) | 2);
                ObSet_Push(psObVa, vaP);
            }
        }
        // prefetch result into cache
        VmmCachePrefetchPages3(pSystemProcess, psObVa, 0x40, 0);
        // interpret result
        while(ObSet_Size(psObVa)) {
            oP = (DWORD)ObSet_Pop(psObOff);
            vaP = (DWORD)ObSet_Pop(psObVa);
            if(!VmmRead2(pSystemProcess, vaP, pbPage, 0x40, VMM_FLAG_FORCECACHE_READ)) {
                if(((vaP + 0x10) & 0xfff) || !VmmRead2(pSystemProcess, vaP + 0x10ULL, pbPage + 0x10, 0x30, VMM_FLAG_FORCECACHE_READ)) {
                    continue;
                }
            }
            // ObjectTable
            f = (1 == (oP & 0xff)) && (*(PDWORD)(pbPage + 12) == 0x6274624f);     // Pool Header: Obtb
            if(f) { po->ObjectTable = (WORD)(oP >> 16); }
            f = (1 == (oP & 0xff)) && VMM_KADDR32_PAGE(vaP + 0x10) && !*(PDWORD)(pbPage + 0x10 + 0x0c) && VMM_KADDR32_4(*(PDWORD)(pbPage + 0x10 + 0x10)) && VMM_KADDR32_4(*(PDWORD)(pbPage + 0x10 + 0x14));     // page-align (no pool hdr)
            if(f) { po->ObjectTable = (WORD)(oP >> 16); }
            // SeAuditProcessCreationInfo
            f = (2 == (oP & 0xff)) && 
                (*(PQWORD)(pbPage + 0x08) == 0x007600650044005C) && (*(PQWORD)(pbPage + 0x10) == 0x005C006500630069) && // L"\Device\"
                (*(PWORD)(pbPage + 0x00) < MAX_PATH) && (*(PWORD)(pbPage + 0x00) < *(PWORD)(pbPage + 0x02));            // _UNICODE_STRING length
            if(f) { po->SeAuditProcessCreationInfo = (WORD)(oP >> 16); }
        }
        // check validity
        if(!po->ObjectTable) { goto fail; }
        if(!po->SeAuditProcessCreationInfo) { goto fail; }
    }
    // find offset for VadRoot by searching for ExitStatus value assumed to be
    // set to: 0x00000103 and existing prior to VadRoot by -12(VISTA)/-4(Win7+)
    // Value may be the 'VadHint' on some systems; scan back 0x30 to find any
    // identical match (which will be assumed to be vadroot).
    {
        for(i = 0x0e0 + po->Name; i < 0x380; i += 4) {
            f = VMM_KADDR32(*(PDWORD)(pbSYSTEM + i)) && ((*(PDWORD)(pbSYSTEM + i - 4) == 0x00000103) || (*(PDWORD)(pbSYSTEM + i - 12) == 0x00000103));
            if(f) { break; }
        }
        if(!f && (*(PDWORD)(pbSYSTEM + 0x11c) == *(PDWORD)(pbSYSTEM + +0x120))) {   // WINXP
            i = 0x11c;
            f = TRUE;
        }
        if(!f) { goto fail; }
        po->VadRoot = i;
        // Scanback 0x30 (in case of 'VadHint' false positive.
        for(i = po->VadRoot - 8; i > po->VadRoot - 0x30; i -= 4) {
            if(*(PDWORD)(pbSYSTEM + i) == *(PDWORD)(pbSYSTEM + po->VadRoot)) {
                po->VadRoot = i;
                break;
            }
        }
    }
    // DTB_USER not searched for in 32-bit EPROCESS
    VmmWinProcess_OffsetLocator_SetMaxOffset();
    po->fValid = TRUE;
fail:
    Ob_DECREF(psObVa);
    Ob_DECREF(psObOff);
}

VOID VmmWinProcess_Enum32_Pre(_In_ PVMM_PROCESS pProcess, _In_opt_ PVMMWIN_ENUMERATE_EPROCESS_CONTEXT ctx, _In_ QWORD va, _In_ PBYTE pb, _In_ DWORD cb, _In_ QWORD vaFLink, _In_ QWORD vaBLink, _In_ POB_SET pVSetAddress, _Inout_ PBOOL pfValidEntry, _Inout_ PBOOL pfValidFLink, _Inout_ PBOOL pfValidBLink)
{
    if(!ctx || !VMM_KADDR32_8(va)) { return; }
    ObSet_Push(ctx->pObSetPrefetchDTB, *(PDWORD)(pb + ctxVmm->offset.EPROCESS.DTB) & ~0xfff);
    *pfValidFLink = VMM_KADDR32_4(vaFLink);
    *pfValidBLink = VMM_KADDR32_4(vaBLink);
    *pfValidEntry = *pfValidFLink || *pfValidBLink;
}

VOID VmmWinProcess_Enum32_Post(_In_ PVMM_PROCESS pSystemProcess, _In_opt_ PVMMWIN_ENUMERATE_EPROCESS_CONTEXT ctx, _In_ QWORD va, _In_ PBYTE pb, _In_ DWORD cb)
{
    PVMM_OFFSET_EPROCESS po = &ctxVmm->offset.EPROCESS;
    PDWORD pdwDTB, pdwDTB_User, pdwPEB;
    PDWORD pdwState, pdwPID, pdwPPID;
    LPSTR szName;
    BOOL fUser;
    PVMM_PROCESS pObProcess = NULL;
    if(!ctx || !VMM_KADDR32_8(va)) { return; }
    pdwState = (PDWORD)(pb + po->State);
    pdwPID = (PDWORD)(pb + po->PID);
    pdwPPID = (PDWORD)(pb + po->PPID);
    pdwDTB = (PDWORD)(pb + po->DTB);
    pdwDTB_User = (PDWORD)(pb + po->DTB_User);
    szName = (LPSTR)(pb + po->Name);
    pdwPEB = (PDWORD)(pb + po->PEB);
    if(ctx->pObSetPrefetchDTB) {    // prefetch any physical pages in ctx->pObSetPrefetchDTB on 1st run only
        VmmCachePrefetchPages(NULL, ctx->pObSetPrefetchDTB, 0);
        Ob_DECREF_NULL(&ctx->pObSetPrefetchDTB);
    }
    if(*pdwPID && *pdwDTB && *(PQWORD)szName) {
        // treat csrss.exe as 'kernel' due to win32k mapping missing in System Process _AND_ treat MemCompression as 'user'
        fUser =
            !((*pdwPID == 4) || ((*pdwState == 0) && (*pdwPEB == 0)) || (*(PQWORD)szName == 0x78652e7373727363)) ||     // csrss.exe
            ((*(PQWORD)(szName + 0x00) == 0x72706d6f436d654d) && (*(PDWORD)(szName + 0x08) == 0x69737365));             // MemCompression "process"
        pObProcess = VmmProcessCreateEntry(
            ctx->fTotalRefresh,
            *pdwPID,
            *pdwPPID,
            *pdwState,
            *pdwDTB & 0xffffffe0,
            po->DTB_User ? (~0xfff & *pdwDTB_User) : 0,
            szName,
            fUser,
            pb,
            cb);
        if(!pObProcess) {
            VmmLog(MID_PROCESS, LOGLEVEL_VERBOSE, "WARNING: PID '%i' already exists or bad DTB", *pdwPID);
            if(++ctx->cNewProcessCollision >= 8) {
                return;
            }
        }
    }
    if(pObProcess) {
        pObProcess->win.EPROCESS.va = (DWORD)va;
        pObProcess->win.EPROCESS.fNoLink = ctx->fNoLinkEPROCESS;
        // PEB
        if(*pdwPEB & 0xfff) {
            VmmLog(MID_PROCESS, LOGLEVEL_VERBOSE, "WARNING: Bad PEB alignment for PID: '%i' (0x%08x)", *pdwPID, *pdwPEB);
        } else {
            pObProcess->win.vaPEB = *pdwPEB;
            pObProcess->win.vaPEB32 = *pdwPEB;
        }
    } else {
        szName[14] = 0; // in case of bad string data ...
    }
    VmmLog(MID_PROCESS, LOGLEVEL_DEBUG, "%04i (%s) %08x %08x %08x %08x %s",
        ctx->cProc,
        !pObProcess ? "skip" : (pObProcess->dwState ? "exit" : "list"),
        *pdwPID,
        *pdwDTB & 0xffffffe0,
        (DWORD)va,
        *pdwPEB,
        szName);
    Ob_DECREF_NULL(&pObProcess);
    ctx->cProc++;
}

BOOL VmmWinProcess_Enum32(_In_ PVMM_PROCESS pSystemProcess, _In_ BOOL fTotalRefresh, _In_opt_ POB_SET psvaNoLinkEPROCESS)
{
    PVMM_OFFSET_EPROCESS po = &ctxVmm->offset.EPROCESS;
    VMMWIN_ENUMERATE_EPROCESS_CONTEXT ctx = { 0 };
    // retrieve offsets
    if(!po->fValid) {
        VmmWinProcess_OffsetLocator32(pSystemProcess);
        VmmWinProcess_OffsetLocator_Print();
        if(!po->fValid) {
            VmmLog(MID_PROCESS, LOGLEVEL_INFO, "Unable to fuzz EPROCESS offsets - trying debug symbols");
            VmmWinProcess_OffsetLocatorSYMSERV(pSystemProcess);
        }
        if(!po->fValid) {
            VmmLog(MID_PROCESS, LOGLEVEL_CRITICAL, "Unable to locate EPROCESS offsets");
            return FALSE;
        }
    }
    VmmLog(MID_PROCESS, LOGLEVEL_DEBUG, "SYSTEM DTB: %016llx EPROCESS: %08x", pSystemProcess->paDTB, (DWORD)pSystemProcess->win.EPROCESS.va);
    // set up context
    ctx.fTotalRefresh = fTotalRefresh;
    if(!(ctx.pObSetPrefetchDTB = ObSet_New())) { return FALSE; }
    // traverse EPROCESS linked list
    VmmLog(MID_PROCESS, LOGLEVEL_DEBUG, "   # STATE  PID      DTB      EPROCESS PEB      NAME");
    VmmWin_ListTraversePrefetch(
        pSystemProcess,
        TRUE,
        &ctx,
        1,
        &pSystemProcess->win.EPROCESS.va,
        ctxVmm->offset.EPROCESS.FLink,
        ctxVmm->offset.EPROCESS.cbMaxOffset,
        (VMMWIN_LISTTRAVERSE_PRE_CB)VmmWinProcess_Enum32_Pre,
        (VMMWIN_LISTTRAVERSE_POST_CB)VmmWinProcess_Enum32_Post,
        ctxVmm->pObCCachePrefetchEPROCESS);
    // add no-link entries (if any)
    VmmWinProcess_Enum_AddNoLink(
        pSystemProcess,
        psvaNoLinkEPROCESS,
        &ctx,
        (VOID(*)(PVMM_PROCESS, PVOID, QWORD, PBYTE, DWORD))VmmWinProcess_Enum32_Post);
    // set resulting prefetch cache
    Ob_DECREF_NULL(&ctx.pObSetPrefetchDTB);
    VmmWinProcess_Enumerate_PostProcessing(pSystemProcess);
    VmmProcessCreateFinish();
    return (ctx.cProc > 10);
}

/*
* Locate EPROCESS objects not linked by the EPROCESS list.
* This is achieved by analyzing the object table for the SYSTEM process.
* CALLER DECREF: return
* -- return = Set of vaEPROCESS if no-link addresses exist. NULL otherwise.
*/
POB_SET VmmWinProcess_Enumerate_FindNoLinkProcesses()
{
    BOOL f32 = ctxVmm->f32;
    BYTE tpProcess, tpObjectEncrypted;
    DWORD i, cbHdr;
    POB_SET psOb = NULL, psObNoLink = NULL;
    PVMM_PROCESS pObSystemProcess = NULL, pObProcess = NULL;
    PVMMOB_MAP_HANDLE pObHandleMap = NULL;
    PVMM_MAP_HANDLEENTRY pe;
    BYTE pbHdr[0x30];
    POBJECT_HEADER32 pHdr32 = (POBJECT_HEADER32)pbHdr;
    POBJECT_HEADER64 pHdr64 = (POBJECT_HEADER64)pbHdr;
    // 1: Initialize
    cbHdr = f32 ? sizeof(OBJECT_HEADER32) : sizeof(OBJECT_HEADER64);
    if(!(psOb = ObSet_New())) { goto fail; }
    if(!(pObSystemProcess = VmmProcessGet(4))) { goto fail; }
    if(!VmmWin_ObjectTypeGet(2) || !(tpProcess = ctxVmm->ObjectTypeTable.tpProcess)) { goto fail; }
    if(!VmmMap_GetHandle(pObSystemProcess, &pObHandleMap, FALSE)) { goto fail; }
    // 2: Prefetch object headers
    for(i = 0; i < pObHandleMap->cMap; i++) {
        ObSet_Push_PageAlign(psOb, pObHandleMap->pMap[i].vaObject - cbHdr, cbHdr);
    }
    VmmCachePrefetchPages(pObSystemProcess, psOb, 0);
    ObSet_Clear(psOb);
    // 3: Index processes by EPROCESS va
    while((pObProcess = VmmProcessGetNext(pObProcess, VMM_FLAG_PROCESS_SHOW_TERMINATED))) {
        ObSet_Push(psOb, pObProcess->win.EPROCESS.va);
    }
    // 4: Check handles for process not in EPROCESS set
    for(i = 0; i < pObHandleMap->cMap; i++) {
        pe = pObHandleMap->pMap + i;
        if(!VmmRead2(pObSystemProcess, pe->vaObject - cbHdr, pbHdr, cbHdr, VMM_FLAG_FORCECACHE_READ | VMM_FLAG_NOPAGING)) { continue; }
        tpObjectEncrypted = f32 ? pHdr32->TypeIndex : pHdr64->TypeIndex;
        if(tpProcess == VmmWin_ObjectTypeGetIndexFromEncoded(pe->vaObject - cbHdr, tpObjectEncrypted)) {
            if(ObSet_Exists(psOb, pe->vaObject)) { continue; }
            // process object not in process list found
            if(!psObNoLink && !(psObNoLink = ObSet_New())) { goto fail; }
            ObSet_Push(psOb, pe->vaObject);
            ObSet_Push(psObNoLink, pe->vaObject);
            VmmLog(MID_PROCESS, LOGLEVEL_DEBUG, "NOLINK_EPROCESS: %016llx", pe->vaObject);
        }
    }
fail:
    Ob_DECREF(pObSystemProcess);
    Ob_DECREF(pObHandleMap);
    Ob_DECREF(psOb);
    return psObNoLink;
}

BOOL VmmWinProcess_Enumerate(_In_ PVMM_PROCESS pSystemProcess, _In_ BOOL fRefreshTotal, _In_opt_ POB_SET psvaNoLinkEPROCESS)
{
    // spider TLB and set up initial system process and enumerate EPROCESS
    VmmTlbSpider(pSystemProcess);
    switch(ctxVmm->tpMemoryModel) {
        case VMM_MEMORYMODEL_X64:
            return VmmWinProcess_Enum64(pSystemProcess, fRefreshTotal, psvaNoLinkEPROCESS);
        case VMM_MEMORYMODEL_X86:
        case VMM_MEMORYMODEL_X86PAE:
            return VmmWinProcess_Enum32(pSystemProcess, fRefreshTotal, psvaNoLinkEPROCESS);
        default:
            return FALSE;
    }
}

// ----------------------------------------------------------------------------
// WINDOWS LIST WALKING FUNCTIONALITY BELOW:
// Walk a Windows Linked List in an efficient way that minimizes the number of
// IO requests to the LeechCore/Device sub-system. This is done by prefetching
// as much as possible before the main functionality is performed. This is done
// by first calling a callback to add additional memory addresses to prefetch
// (pfnCallback_Pre). Then a prefetch into cache is done, and then a callback
// into the main analysis functionality is done (pfnCallback_Post).
// ----------------------------------------------------------------------------

#define VMMWIN_LISTTRAVERSEPREFETCH_LOOPPROTECT_MAX         0x1000

/*
* Walk a windows linked list in an efficient way that minimize IO requests to
* the the device. This is advantageous for latency reasons. The function return
* a set of the addresses used - this may be used to prefetch pages in advance
* if the list should be walked again at a later time.
* The callback function must only return FALSE on severe errors when the list
* should no longer be continued to be walked in the direction.
* CALLER_DECREF: return
* -- pProcess
* -- f32
* -- ctx = ctx to pass along to callback function (if any)
* -- cvaDataStart
* -- pvaDataStart
* -- oListStart = offset (in bytes) to _LIST_ENTRY from vaDataStart
* -- cbData
* -- pfnCallback_Pre = optional callback function to gather additional addresses.
* -- pfnCallback_Post = optional callback function called after all pages fetched into cache.
* -- pPrefetchAddressContainer = optional pointer to a PVMMOBCONTAINER containing a POB_VSET of prefetch addresses to use/update.
*/
VOID VmmWin_ListTraversePrefetch(
    _In_ PVMM_PROCESS pProcess,
    _In_ BOOL f32,
    _In_opt_ PVOID ctx,
    _In_ DWORD cvaDataStart,
    _In_ PQWORD pvaDataStart,
    _In_ DWORD oListStart,
    _In_ DWORD cbData,
    _In_opt_ VMMWIN_LISTTRAVERSE_PRE_CB pfnCallback_Pre,
    _In_opt_ VMMWIN_LISTTRAVERSE_POST_CB pfnCallback_Post,
    _In_opt_ POB_CONTAINER pPrefetchAddressContainer
) {
    QWORD vaData;
    DWORD cbReadData;
    PBYTE pbData = NULL;
    QWORD vaFLink, vaBLink;
    POB_SET pObSet_vaAll = NULL, pObSet_vaTry1 = NULL, pObSet_vaTry2 = NULL, pObSet_vaValid = NULL;
    BOOL fValidEntry, fValidFLink, fValidBLink, fTry1;
    // 1: Prefetch any addresses stored in optional address container
    pObSet_vaAll = ObContainer_GetOb(pPrefetchAddressContainer);
    VmmCachePrefetchPages3(pProcess, pObSet_vaAll, cbData, 0);
    Ob_DECREF_NULL(&pObSet_vaAll);
    // 2: Prepare/Allocate and set up initial entry
    if(!(pObSet_vaAll = ObSet_New())) { goto fail; }
    if(!(pObSet_vaTry1 = ObSet_New())) { goto fail; }
    if(!(pObSet_vaTry2 = ObSet_New())) { goto fail; }
    if(!(pObSet_vaValid = ObSet_New())) { goto fail; }
    if(!(pbData = LocalAlloc(0, cbData))) { goto fail; }
    while(cvaDataStart) {
        cvaDataStart--;
        ObSet_Push(pObSet_vaAll, pvaDataStart[cvaDataStart]);
        ObSet_Push(pObSet_vaTry1, pvaDataStart[cvaDataStart]);
    }
    // 3: Initial list walk
    fTry1 = TRUE;
    while(TRUE) {
        if(fTry1) {
            vaData = ObSet_Pop(pObSet_vaTry1);
            if(!vaData && (0 == ObSet_Size(pObSet_vaTry2))) { break; }
            if(!vaData) {
                VmmCachePrefetchPages3(pProcess, pObSet_vaAll, cbData, 0);
                fTry1 = FALSE;
                continue;
            }
            VmmReadEx(pProcess, vaData, pbData, cbData, &cbReadData, VMM_FLAG_FORCECACHE_READ);
            if(cbReadData != cbData) {
                ObSet_Push(pObSet_vaTry2, vaData);
                continue;
            }
        } else {
            vaData = ObSet_Pop(pObSet_vaTry2);
            if(!vaData && (0 == ObSet_Size(pObSet_vaTry1))) { break; }
            if(!vaData) { fTry1 = TRUE; continue; }
            if(!VmmRead(pProcess, vaData, pbData, cbData)) { continue; }
        }
        vaFLink = f32 ? *(PDWORD)(pbData + oListStart + 0) : *(PQWORD)(pbData + oListStart + 0);
        vaBLink = f32 ? *(PDWORD)(pbData + oListStart + 4) : *(PQWORD)(pbData + oListStart + 8);
        if(pfnCallback_Pre) {
            fValidEntry = FALSE; fValidFLink = FALSE; fValidBLink = FALSE;
            pfnCallback_Pre(pProcess, ctx, vaData, pbData, cbData, vaFLink, vaBLink, pObSet_vaAll, &fValidEntry, &fValidFLink, &fValidBLink);
        } else {
            if(f32) {
                fValidFLink = !(vaFLink & 0x03);
                fValidBLink = !(vaBLink & 0x03);
            } else {
                fValidFLink = VMM_KADDR64_8(vaFLink) || VMM_UADDR64_8(vaFLink);
                fValidBLink = VMM_KADDR64_8(vaBLink) || VMM_UADDR64_8(vaBLink);
            }
            fValidEntry = fValidFLink || fValidBLink;
        }
        if(fValidEntry) {
            ObSet_Push(pObSet_vaValid, vaData);
        }
        vaFLink -= oListStart;
        vaBLink -= oListStart;
        if(fValidFLink && !ObSet_Exists(pObSet_vaAll, vaFLink)) {
            ObSet_Push(pObSet_vaAll, vaFLink);
            ObSet_Push(pObSet_vaTry1, vaFLink);
        }
        if(fValidBLink && !ObSet_Exists(pObSet_vaAll, vaBLink)) {
            ObSet_Push(pObSet_vaAll, vaBLink);
            ObSet_Push(pObSet_vaTry1, vaBLink);
        }
    }
    // 4: Prefetch additional gathered addresses into cache.
    VmmCachePrefetchPages3(pProcess, pObSet_vaAll, cbData, 0);
    // 5: 2nd main list walk. Call into optional pfnCallback_Post to do the main
    //    processing of the list items.
    if(pfnCallback_Post) {
        while((vaData = ObSet_Pop(pObSet_vaValid))) {
            if(VmmRead(pProcess, vaData, pbData, cbData)) {
                pfnCallback_Post(pProcess, ctx, vaData, pbData, cbData);
            }
        }
    }
    // 6: Store/Update the optional container with the newly prefetch addresses (if possible and desirable).
    if(pPrefetchAddressContainer && ctxMain->dev.fVolatile && ctxVmm->ThreadProcCache.fEnabled) {
        ObContainer_SetOb(pPrefetchAddressContainer, pObSet_vaAll);
    }
fail:
    // 7: Cleanup
    Ob_DECREF_NULL(&pObSet_vaAll);
    Ob_DECREF_NULL(&pObSet_vaTry1);
    Ob_DECREF_NULL(&pObSet_vaTry2);
    Ob_DECREF_NULL(&pObSet_vaValid);
    LocalFree(pbData);
}
