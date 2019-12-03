// ob_map.c : implementation of object manager hashed map functionality.
//
// The map is a key-value map that may, as an option, contain object manager
// objects in its value field. They key may be user-defined, generated by a
// function or absent. The ObMap may hold a maximum capacity of 0x02000000
// (~32M) entries which are UNIQUE and non-NULL.
//
// The map (ObMap) is thread safe and implement efficient access to the data
// via internal hashing functionality.
// The map (ObMap) guarantees order amongst values unless the ObMap_Remove*
// functions are called - in which order may change and on-going iterations
// of the set with ObMap_Get/ObMap_GetNext may fail.
// The ObMap is an object manager object and must be DECREF'ed when required.
//
// (c) Ulf Frisk, 2019
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "ob.h"

#define OB_MAP_ENTRIES_DIRECTORY    0x100
#define OB_MAP_ENTRIES_TABLE        0x200
#define OB_MAP_ENTRIES_STORE        0x100
#define OB_MAP_IS_VALID(p)          (p && (p->ObHdr._magic == OB_HEADER_MAGIC) && (p->ObHdr._tag == OB_TAG_CORE_MAP))
#define OB_MAP_TABLE_MAX_CAPACITY   OB_MAP_ENTRIES_DIRECTORY * OB_MAP_ENTRIES_TABLE * OB_MAP_ENTRIES_STORE
#define OB_MAP_HASH_FUNCTION(v)     (13 * (v + _rotr16((WORD)v, 13) + _rotr((DWORD)v, 17) + _rotr64(v, 23)))

#define OB_MAP_INDEX_DIRECTORY(i)   ((i >> 17) & (OB_MAP_ENTRIES_DIRECTORY - 1))
#define OB_MAP_INDEX_TABLE(i)       ((i >> 8) & (OB_MAP_ENTRIES_TABLE - 1))
#define OB_MAP_INDEX_STORE(i)       (i & (OB_MAP_ENTRIES_STORE - 1))

typedef struct tdOB_MAP_ENTRY {
    QWORD k;
    PVOID v;
} OB_MAP_ENTRY, *POB_MAP_ENTRY, **PPOB_MAP_ENTRY;

typedef struct tdOB_MAP {
    OB ObHdr;
    SRWLOCK LockSRW;
    DWORD c;
    DWORD cHashMax;
    DWORD cHashGrowThreshold;
    BOOL fLargeMode;
    BOOL fKey;
    BOOL fObjectsOb;
    BOOL fObjectsLocalFree;
    PDWORD pHashMapKey;
    PDWORD pHashMapValue;
    union {
        PPOB_MAP_ENTRY Directory[OB_MAP_ENTRIES_DIRECTORY];
        struct {
            PPOB_MAP_ENTRY _SmallDirectory[1];
            DWORD _SmallHashMap[0x200];
        };
    };
    POB_MAP_ENTRY _SmallTable[1];
    OB_MAP_ENTRY Store00[OB_MAP_ENTRIES_STORE];
} OB_MAP, *POB_MAP;

#define OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_WRITE(pm, RetTp, RetValFail, fn) {      \
    if(!OB_MAP_IS_VALID(pm)) { return RetValFail; }                                     \
    RetTp retVal;                                                                       \
    AcquireSRWLockExclusive(&pm->LockSRW);                                              \
    retVal = fn;                                                                        \
    ReleaseSRWLockExclusive(&pm->LockSRW);                                              \
    return retVal;                                                                      \
}

#define OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_READ(pm, RetTp, RetValFail, fn) {       \
    if(!OB_MAP_IS_VALID(pm)) { return RetValFail; }                                     \
    RetTp retVal;                                                                       \
    AcquireSRWLockShared(&pm->LockSRW);                                                 \
    retVal = fn;                                                                        \
    ReleaseSRWLockShared(&pm->LockSRW);                                                 \
    return retVal;                                                                      \
}

/*
* Ob_DECREF / LocalFree all objects in the map (if required)
* -- pObMap
*/
VOID _ObMap_ObFreeAllObjects(_In_ POB_MAP pObMap)
{
    DWORD i;
    POB_MAP_ENTRY pe;
    if(pObMap->fObjectsOb) {
        for(i = 1; i < pObMap->c; i++) {
            pe = &pObMap->Directory[OB_MAP_INDEX_DIRECTORY(i)][OB_MAP_INDEX_TABLE(i)][OB_MAP_INDEX_STORE(i)];
            Ob_DECREF(pe->v);
        }
    } else if(pObMap->fObjectsLocalFree) {
        for(i = 1; i < pObMap->c; i++) {
            pe = &pObMap->Directory[OB_MAP_INDEX_DIRECTORY(i)][OB_MAP_INDEX_TABLE(i)][OB_MAP_INDEX_STORE(i)];
            LocalFree(pe->v);
        }
    }
}

/*
* Object Container object manager cleanup function to be called when reference
* count reaches zero.
* -- pObMap
*/
VOID _ObMap_ObCloseCallback(_In_ POB_MAP pObMap)
{
    DWORD iDirectory, iTable;
    _ObMap_ObFreeAllObjects(pObMap);
    if(pObMap->fLargeMode) {
        for(iDirectory = 0; iDirectory < OB_MAP_ENTRIES_DIRECTORY; iDirectory++) {
            if(!pObMap->Directory[iDirectory]) { break; }
            for(iTable = 0; iTable < OB_MAP_ENTRIES_TABLE; iTable++) {
                if(!pObMap->Directory[iDirectory][iTable]) { break; }
                if(iDirectory || iTable) {
                    LocalFree(pObMap->Directory[iDirectory][iTable]);
                }
            }
            LocalFree(pObMap->Directory[iDirectory]);
        }
        LocalFree(pObMap->pHashMapValue);
    }
}

inline POB_MAP_ENTRY _ObMap_GetFromIndex(_In_ POB_MAP pm, _In_ DWORD iEntry)
{
    if(!iEntry || (iEntry >= pm->c)) { return NULL; }
    return &pm->Directory[OB_MAP_INDEX_DIRECTORY(iEntry)][OB_MAP_INDEX_TABLE(iEntry)][OB_MAP_INDEX_STORE(iEntry)];
}

inline QWORD _ObMap_GetFromEntryIndex(_In_ POB_MAP pm, _In_ BOOL fValueHash, _In_ DWORD iEntry)
{
    POB_MAP_ENTRY pe = _ObMap_GetFromIndex(pm, iEntry);
    return pe ? (fValueHash ? (QWORD)pe->v : pe->k) : 0;
}

inline VOID _ObMap_SetHashIndex(_In_ POB_MAP pm, _In_ BOOL fValueHash, _In_ DWORD iHash, _In_ DWORD iEntry)
{
    if(fValueHash) {
        pm->pHashMapValue[iHash] = iEntry;
    } else if(pm->fKey) {
        pm->pHashMapKey[iHash] = iEntry;
    }
}

VOID _ObMap_InsertHash(_In_ POB_MAP pm, _In_ BOOL fValueHash, _In_ DWORD iEntry)
{
    QWORD qwValueToHash;
    DWORD iHash, dwHashMask = pm->cHashMax - 1;
    if(!fValueHash && !pm->fKey) { return; }
    qwValueToHash = _ObMap_GetFromEntryIndex(pm, fValueHash, iEntry);
    iHash = OB_MAP_HASH_FUNCTION(qwValueToHash) & dwHashMask;
    while(fValueHash ? pm->pHashMapValue[iHash] : pm->pHashMapKey[iHash]) {
        iHash = (iHash + 1) & dwHashMask;
    }
    _ObMap_SetHashIndex(pm, fValueHash, iHash, iEntry);
}

VOID _ObMap_RemoveHash(_In_ POB_MAP pm, _In_ BOOL fValueHash, _In_ QWORD kv, _In_ DWORD iEntry)
{
    DWORD iHash, dwHashMask = pm->cHashMax - 1;
    DWORD iNextHash, iNextEntry, iNextHashPreferred;
    QWORD qwNextEntry;
    if(!fValueHash && !pm->fKey) { return; }
    // search for hash index and clear
    iHash = OB_MAP_HASH_FUNCTION(kv) & dwHashMask;
    while(TRUE) {
        if(iEntry == (fValueHash ? pm->pHashMapValue[iHash] : pm->pHashMapKey[iHash])) { break; }
        iHash = (iHash + 1) & dwHashMask;
    }
    _ObMap_SetHashIndex(pm, fValueHash, iHash, 0);
    // re-hash any entries following (value)
    iNextHash = iHash;
    while(TRUE) {
        iNextHash = (iNextHash + 1) & dwHashMask;
        iNextEntry = fValueHash ? pm->pHashMapValue[iHash] : pm->pHashMapKey[iHash];
        if(0 == iNextEntry) { return; }
        qwNextEntry = _ObMap_GetFromEntryIndex(pm, fValueHash, iNextEntry);
        iNextHashPreferred = OB_MAP_HASH_FUNCTION(qwNextEntry) & dwHashMask;
        if(iNextHash == iNextHashPreferred) { return; }
        _ObMap_SetHashIndex(pm, fValueHash, iNextHash, 0);
        _ObMap_InsertHash(pm, fValueHash, iNextEntry);
    }
}

_Success_(return)
BOOL _ObMap_GetEntryIndexFromKeyOrValue(_In_ POB_MAP pm, _In_ BOOL fValueHash, _In_ QWORD kv, _Out_opt_ PDWORD piEntry)
{
    DWORD iEntry;
    DWORD dwHashMask = pm->cHashMax - 1;
    DWORD iHash = OB_MAP_HASH_FUNCTION(kv) & dwHashMask;
    if(!fValueHash && !pm->fKey) { return FALSE; }
    // scan hash table to find entry
    while(TRUE) {
        iEntry = fValueHash ? pm->pHashMapValue[iHash] : pm->pHashMapKey[iHash];
        if(0 == iEntry) { return FALSE; }
        if(kv == _ObMap_GetFromEntryIndex(pm, fValueHash, iEntry)) {
            if(piEntry) { *piEntry = iEntry; }
            return TRUE;
        }
        iHash = (iHash + 1) & dwHashMask;
    }
}

//-----------------------------------------------------------------------------
// RETRIEVE/GET FUNCTIONALITY BELOW:
// ObMap_Size, ObMap_Exists,  ObMap_ExistsKey, ObMap_GetByIndex,
// ObMap_Peek, ObMap_PeekKey, ObMap_GetByKey,  ObMap_GetNext, 
//-----------------------------------------------------------------------------

inline BOOL _ObMap_Exists(_In_ POB_MAP pm, _In_ BOOL fValueHash, _In_ QWORD kv)
{
    if((!fValueHash && !pm->fKey) || (fValueHash && !kv)) { return FALSE; }
    return _ObMap_GetEntryIndexFromKeyOrValue(pm, fValueHash, kv, NULL);
}

/*
* Retrieve the number of objects in the ObMap.
* -- pm
* -- return
*/
DWORD ObMap_Size(_In_opt_ POB_MAP pm)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_READ(pm, DWORD, 0, pm->c - 1)
}

/*
* Check if an object exists in the ObMap.
* -- pm
* -- qwKey/pvObject
* -- return
*/
BOOL ObMap_Exists(_In_opt_ POB_MAP pm, _In_ PVOID pvObject)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_READ(pm, BOOL, FALSE, _ObMap_Exists(pm, TRUE, (QWORD)pvObject))
}

/*
* Check if a key exists in the ObMap.
* -- pm
* -- qwKey/pvObject
* -- return
*/
BOOL ObMap_ExistsKey(_In_opt_ POB_MAP pm, _In_ QWORD qwKey)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_READ(pm, BOOL, FALSE, _ObMap_Exists(pm, FALSE, qwKey))
}

PVOID _ObMap_GetByEntryIndex(_In_ POB_MAP pm, _In_ DWORD iEntry)
{
    PVOID pvObObject = (PVOID)_ObMap_GetFromEntryIndex(pm, TRUE, iEntry);
    if(pm->fObjectsOb) { Ob_INCREF(pvObObject); }
    return pvObObject;
}

PVOID _ObMap_GetByKey(_In_ POB_MAP pm, _In_ QWORD qwKey)
{
    DWORD iEntry;
    return _ObMap_GetEntryIndexFromKeyOrValue(pm, FALSE, qwKey, &iEntry) ? _ObMap_GetByEntryIndex(pm, iEntry) : NULL;
}

PVOID _ObMap_GetNext(_In_ POB_MAP pm, _In_opt_ PVOID pvObject)
{
    DWORD iEntry;
    if(!pvObject) {
        return _ObMap_GetByEntryIndex(pm, 1);
    }
    if(pm->fObjectsOb) { Ob_DECREF(pvObject); }
    if(!_ObMap_GetEntryIndexFromKeyOrValue(pm, TRUE, (QWORD)pvObject, &iEntry)) { return NULL; }
    return _ObMap_GetByEntryIndex(pm, iEntry + 1);
}

PVOID _ObMap_GetNextByKey(_In_ POB_MAP pm, _In_ QWORD qwKey, _In_opt_ PVOID pvObject)
{
    DWORD iEntry;
    if(!pvObject) {
        return _ObMap_GetByEntryIndex(pm, 1);
    }
    if(pm->fObjectsOb) { Ob_DECREF(pvObject); }
    if(!_ObMap_GetEntryIndexFromKeyOrValue(pm, FALSE, qwKey, &iEntry)) { return NULL; }
    return _ObMap_GetByEntryIndex(pm, iEntry + 1);
}

// NB: CALLER LOCALFREE: return
_Success_(return != NULL)
POB_DATA _ObMap_GetTableKeys(_In_ POB_MAP pm)
{
    QWORD iEntry;
    POB_DATA pObData;
    if((pm->c <= 1) || !(pObData = Ob_Alloc(OB_TAG_CORE_DATA, 0, sizeof(OB) + pm->c * sizeof(QWORD), NULL, NULL))) { return NULL; }
    for(iEntry = 1; iEntry < pm->c; iEntry++) {
        pObData->pqw[iEntry] = pm->Directory[OB_MAP_INDEX_DIRECTORY(iEntry)][OB_MAP_INDEX_TABLE(iEntry)][OB_MAP_INDEX_STORE(iEntry)].k;
    }
    pObData->pqw[0] = pm->c - 1;
    return pObData;
}

/*
* Retrieve an object given an index (which is less than the amount of items
* in the ObMap).
* NB! Correctness of the Get/GetNext functionality is _NOT- guaranteed if the
* ObMap_Remove* functions are called while iterating over the ObVSet - items
* may be skipped or iterated over multiple times!
* CALLER DECREF(if OB): return
* -- pm
* -- index
* -- return
*/
PVOID ObMap_GetByIndex(_In_opt_ POB_MAP pm, _In_ DWORD index)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_READ(pm, PVOID, NULL, _ObMap_GetByEntryIndex(pm, index + 1))  // (+1 == account/adjust for index 0 (reserved))
}

/*
* Retrieve a value given a key.
* CALLER DECREF(if OB): return
* -- pm
* -- qwKey
* -- return
*/
PVOID ObMap_GetByKey(_In_opt_ POB_MAP pm, _In_ QWORD qwKey)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_READ(pm, PVOID, NULL, _ObMap_GetByKey(pm, qwKey))
}

/*
* Retrieve the next object given an object. Start and end objects are NULL.
* NB! Correctness of the Get/GetNext functionality is _NOT_ guaranteed if the
* ObMap_Remove* functions are called while iterating over the ObMap - items may
* be skipped or iterated over multiple times!
* FUNCTION DECREF(if OB): pvObject
* CALLER DECREF(if OB): return
* -- pm
* -- pvObject
* -- return
*/
PVOID ObMap_GetNext(_In_opt_ POB_MAP pm, _In_opt_ PVOID pvObject)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_READ(pm, PVOID, NULL, _ObMap_GetNext(pm, pvObject))
}

/*
* Retrieve the next object given a key. To start iterating supply NULL in the
* pvObject parameter (this overrides qwKey). When no more objects are found
* NULL will be returned. This function may ideally be used when object maps
* may be refreshed between function calls. Key may be more stable than object.
* NB! Correctness of the Get/GetNext functionality is _NOT_ guaranteed if the
* ObMap_Remove* functions are called while iterating over the ObMap - items may
* be skipped or iterated over multiple times!
* FUNCTION DECREF(if OB): pvObject
* CALLER DECREF(if OB): return
* -- pm
* -- qwKey
* -- pvObject
* -- return
*/
PVOID ObMap_GetNextByKey(_In_opt_ POB_MAP pm, _In_ QWORD qwKey, _In_opt_ PVOID pvObject)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_READ(pm, PVOID, NULL, _ObMap_GetNextByKey(pm, qwKey, pvObject))
}

/*
* Peek the "last" object.
* CALLER DECREF(if OB): return
* -- pm
* -- return = success: object, fail: NULL.
*/
PVOID ObMap_Peek(_In_opt_ POB_MAP pm)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_READ(pm, PVOID, NULL, _ObMap_GetByEntryIndex(pm, pm->c - 1))
}

/*
* Peek the key of the "last" object.
* -- pm
* -- return = the key, otherwise 0.
*/
QWORD ObMap_PeekKey(_In_opt_ POB_MAP pm)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_READ(pm, QWORD, 0, _ObMap_GetFromEntryIndex(pm, FALSE, pm->c - 1))
}

/*
* Return all keys in the map in a POB_DATA object consisting of a QWORD array.
* Item 0 contains the number of items in the array (not including the 0th item)
* CALLER DECREF: return
* -- pm
* -- return = POB_DATA consisting of QWORD array, NULL if map is empty.
*/
_Success_(return != NULL)
POB_DATA ObMap_GetTableKeys(_In_opt_ POB_MAP pm)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_READ(pm, POB_DATA, NULL, _ObMap_GetTableKeys(pm));
}



//-----------------------------------------------------------------------------
// REMOVAL FUNCTIONALITY BELOW:
// ObMap_Pop, ObMap_Remove, ObMap_RemoveByKey
//-----------------------------------------------------------------------------

/*
* CALLER DECREF: return
*/
_Success_(return != NULL)
PVOID _ObMap_RetrieveAndRemoveByEntryIndex(_In_ POB_MAP pm, _In_ DWORD iEntry, _Out_opt_ PQWORD pKey)
{
    POB_MAP_ENTRY pRemoveEntry, pLastEntry;
    QWORD qwRemoveKey, qwRemoveValue;
    if(!(pRemoveEntry = _ObMap_GetFromIndex(pm, iEntry))) { return NULL; }
    qwRemoveKey = pRemoveEntry->k;
    qwRemoveValue = (QWORD)pRemoveEntry->v;
    _ObMap_RemoveHash(pm, FALSE, qwRemoveKey, iEntry);
    _ObMap_RemoveHash(pm, TRUE,  qwRemoveValue, iEntry);
    if(iEntry < pm->c - 1) {
        // not last item removed -> move last item into empty bucket
        pLastEntry = _ObMap_GetFromIndex(pm, pm->c - 1);
        _ObMap_RemoveHash(pm, FALSE, pLastEntry->k, pm->c - 1);
        _ObMap_RemoveHash(pm, TRUE, (QWORD)pLastEntry->v, pm->c - 1);
        pRemoveEntry->k = pLastEntry->k;
        pRemoveEntry->v = pLastEntry->v;
        _ObMap_InsertHash(pm, FALSE, iEntry);
        _ObMap_InsertHash(pm, TRUE, iEntry);
    }
    pm->c--;
    if(pKey) { *pKey = qwRemoveKey; }
    return (PVOID)qwRemoveValue;
}

PVOID _ObMap_RemoveOrRemoveByKey(_In_ POB_MAP pm, _In_ BOOL fValueHash, _In_ QWORD kv)
{
    DWORD iEntry;
    if(fValueHash && !kv) { return NULL; }
    if(!_ObMap_GetEntryIndexFromKeyOrValue(pm, fValueHash, kv, &iEntry)) { return NULL; }
    return _ObMap_RetrieveAndRemoveByEntryIndex(pm, iEntry, NULL);
}

/*
* Remove the "last" object.
* CALLER DECREF(if OB): return
* -- pm
* -- return = success: object, fail: NULL.
*/
_Success_(return != NULL)
PVOID ObMap_Pop(_In_opt_ POB_MAP pm)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_WRITE(pm, PVOID, NULL, _ObMap_RetrieveAndRemoveByEntryIndex(pm, pm->c - 1, NULL))
}

/*
* Remove the "last" object and return it and its key.
* CALLER DECREF(if OB): return
* -- pm
* -- pKey
* -- return = success: object, fail: NULL.
*/
_Success_(return != NULL)
PVOID ObMap_PopWithKey(_In_opt_ POB_MAP pm, _Out_ PQWORD pKey)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_WRITE(pm, PVOID, NULL, _ObMap_RetrieveAndRemoveByEntryIndex(pm, pm->c - 1, pKey))
}

/*
* Remove an object from the ObMap.
* NB! must not be called simultaneously while iterating with ObMap_GetByIndex/ObMap_GetNext.
* -- pm
* -- value
* -- return = success: object, fail: NULL.
*/
PVOID ObMap_Remove(_In_opt_ POB_MAP pm, _In_ PVOID pvObject)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_WRITE(pm, PVOID, NULL, _ObMap_RemoveOrRemoveByKey(pm, TRUE, (QWORD)pvObject))
}

/*
* Remove an object from the ObMap by using its key.
* NB! must not be called simultaneously while iterating with ObMap_GetByIndex/ObMap_GetNext.
* -- pm
* -- qwKey
* -- return = success: object, fail: NULL.
*/
PVOID ObMap_RemoveByKey(_In_opt_ POB_MAP pm, _In_ QWORD qwKey)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_WRITE(pm, PVOID, NULL, _ObMap_RemoveOrRemoveByKey(pm, FALSE, qwKey))
}

/*
* Clear the ObMap by removing all objects and their keys.
* NB! underlying allocated memory will remain unchanged.
* -- pm
* -- return = clear was successful - always true.
*/
VOID ObMap_Clear(_In_opt_ POB_MAP pm)
{
    if(!OB_MAP_IS_VALID(pm) || (pm->c <= 1)) { return; }
    AcquireSRWLockExclusive(&pm->LockSRW);
    if(pm->c <= 1) {
        ReleaseSRWLockExclusive(&pm->LockSRW);
        return;
    }
    _ObMap_ObFreeAllObjects(pm);
    ZeroMemory(pm->pHashMapValue, 4ULL * pm->cHashMax);
    if(pm->pHashMapKey) { ZeroMemory(pm->pHashMapKey, 4ULL * pm->cHashMax); }
    pm->c = 1;  // item zero is reserved - hence the initialization of count to 1
    ReleaseSRWLockExclusive(&pm->LockSRW);
}



//-----------------------------------------------------------------------------
// CREATE / INSERT FUNCTIONALITY BELOW:
// ObMap_New, ObMap_Push
//-----------------------------------------------------------------------------

/*
* Grow the Tables for hash lookups by a factor of *2.
* -- pvs
* -- pm
*/
_Success_(return)
BOOL _ObMap_Grow(_In_ POB_MAP pm)
{
    DWORD iEntry;
    PDWORD pdwNewAllocHashMap;
    if(!(pdwNewAllocHashMap = LocalAlloc(LMEM_ZEROINIT, 2 * sizeof(DWORD) * pm->cHashMax * (pm->fKey ? 2 : 1)))) { return FALSE; }
    if(!pm->fLargeMode) {
        if(!(pm->Directory[0] = LocalAlloc(LMEM_ZEROINIT, sizeof(POB_MAP_ENTRY) * OB_MAP_ENTRIES_TABLE))) { return FALSE; }
        pm->Directory[0][0] = pm->Store00;
        ZeroMemory(pm->_SmallHashMap, sizeof(pm->_SmallHashMap));
        pm->pHashMapKey = NULL;
        pm->pHashMapValue = NULL;
        pm->fLargeMode = TRUE;
    }
    pm->cHashMax *= 2;
    pm->cHashGrowThreshold *= 2;
    LocalFree(pm->pHashMapValue);
    pm->pHashMapValue = pdwNewAllocHashMap;
    if(pm->fKey) {
        pm->pHashMapKey = pm->pHashMapValue + pm->cHashMax;
    }
    for(iEntry = 1; iEntry < pm->c; iEntry++) {
        _ObMap_InsertHash(pm, TRUE, iEntry);
        _ObMap_InsertHash(pm, FALSE, iEntry);
    }
    return TRUE;
}

_Success_(return)
BOOL _ObMap_Push(_In_ POB_MAP pm, _In_ QWORD qwKey, _In_ PVOID pvObject)
{
    POB_MAP_ENTRY pe;
    DWORD iEntry = pm->c;
    if(!pvObject || _ObMap_Exists(pm, TRUE, (QWORD)pvObject) || _ObMap_Exists(pm, FALSE, qwKey)) { return FALSE; }
    if(iEntry == OB_MAP_ENTRIES_DIRECTORY * OB_MAP_ENTRIES_TABLE * OB_MAP_ENTRIES_STORE) { return FALSE; }
    if(iEntry == pm->cHashGrowThreshold) {
        if(!_ObMap_Grow(pm)) {
            return FALSE;
        }
    }
    if(!pm->Directory[OB_MAP_INDEX_DIRECTORY(iEntry)]) {    // allocate "table" if required
        if(!(pm->Directory[OB_MAP_INDEX_DIRECTORY(iEntry)] = LocalAlloc(LMEM_ZEROINIT, sizeof(POB_MAP_ENTRY) * OB_MAP_ENTRIES_TABLE))) { return FALSE; }
    }
    if(!pm->Directory[OB_MAP_INDEX_DIRECTORY(iEntry)][OB_MAP_INDEX_TABLE(iEntry)]) {    // allocate "store" if required
        if(!(pm->Directory[OB_MAP_INDEX_DIRECTORY(iEntry)][OB_MAP_INDEX_TABLE(iEntry)] = LocalAlloc(LMEM_ZEROINIT, sizeof(OB_MAP_ENTRY) * OB_MAP_ENTRIES_STORE))) { return FALSE; }
    }
    if(pm->fObjectsOb) {
        Ob_INCREF(pvObject);
    }
    pm->c++;
    pe = _ObMap_GetFromIndex(pm, iEntry);
    pe->k = qwKey;
    pe->v = pvObject;
    _ObMap_InsertHash(pm, TRUE, iEntry);
    _ObMap_InsertHash(pm, FALSE, iEntry);
    return TRUE;
}

/*
* Push / Insert into the ObMap.
* -- pm
* -- qwKey
* -- pvObject
* -- return = TRUE on insertion, FALSE otherwise - i.e. if the key or object
*             already exists or if the max capacity of the map is reached.
*/
_Success_(return)
BOOL ObMap_Push(_In_opt_ POB_MAP pm, _In_ QWORD qwKey, _In_ PVOID pvObject)
{
    OB_MAP_CALL_SYNCHRONIZED_IMPLEMENTATION_WRITE(pm, BOOL, FALSE, _ObMap_Push(pm, qwKey, pvObject))
}

/*
* Create a new map. A map (ObMap) provides atomic map operations and ways
* to optionally map key values to values, pointers or object manager objects.
* The ObVSet is an object manager object and must be DECREF'ed when required.
* CALLER DECREF: return
* -- flags
* -- return
*/
POB_MAP ObMap_New(_In_ QWORD flags)
{
    POB_MAP pObMap;
    if((flags & OB_MAP_FLAGS_OBJECT_OB) && (flags & OB_MAP_FLAGS_OBJECT_LOCALFREE)) { return NULL; }
    pObMap = Ob_Alloc(OB_TAG_CORE_MAP, LMEM_ZEROINIT, sizeof(OB_MAP), _ObMap_ObCloseCallback, NULL);
    if(!pObMap) { return NULL; }
    InitializeSRWLock(&pObMap->LockSRW);
    pObMap->c = 1;      // item zero is reserved - hence the initialization of count to 1
    pObMap->fKey = (flags & OB_MAP_FLAGS_NOKEY) ? FALSE : TRUE;
    pObMap->fObjectsOb = (flags & OB_MAP_FLAGS_OBJECT_OB) ? TRUE : FALSE;
    pObMap->fObjectsLocalFree = (flags & OB_MAP_FLAGS_OBJECT_LOCALFREE) ? TRUE : FALSE;
    pObMap->_SmallTable[0] = pObMap->Store00;
    pObMap->Directory[0] = pObMap->_SmallTable;
    pObMap->pHashMapValue = pObMap->_SmallHashMap;
    pObMap->cHashMax = 0x100;
    pObMap->cHashGrowThreshold = 0xc0;
    pObMap->pHashMapKey = pObMap->pHashMapValue + pObMap->cHashMax;
    return pObMap;
}
