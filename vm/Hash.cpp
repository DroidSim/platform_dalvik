/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Hash table.  The dominant calls are add and lookup, with removals
 * happening very infrequently.  We use probing, and don't worry much
 * about tombstone removal.
 */
#include "Dalvik.h"

#include <stdlib.h>

/* table load factor, i.e. how full can it get before we resize */
//#define LOAD_NUMER  3       // 75%
//#define LOAD_DENOM  4
#define LOAD_NUMER  5       // 62.5%
#define LOAD_DENOM  8
//#define LOAD_NUMER  1       // 50%
//#define LOAD_DENOM  2

/*
 * Compute the capacity needed for a table to hold "size" elements.
 */
size_t dvmHashSize(size_t size) {
    return (size * LOAD_DENOM) / LOAD_NUMER +1;
}


/*
 * Create and initialize a hash table.
 */
HashTable* dvmHashTableCreate(size_t initialSize, HashFreeFunc freeFunc)
{
    HashTable* pHashTable;

    assert(initialSize > 0);

    pHashTable = (HashTable*) malloc(sizeof(*pHashTable));
    if (pHashTable == NULL)
        return NULL;

    dvmInitMutex(&pHashTable->lock);

    pHashTable->tableSize = dexRoundUpPower2(initialSize);
    pHashTable->numEntries = pHashTable->numDeadEntries = 0;
    pHashTable->freeFunc = freeFunc;
    pHashTable->pEntries =
        (HashEntry*) calloc(pHashTable->tableSize, sizeof(HashEntry));
    if (pHashTable->pEntries == NULL) {
        free(pHashTable);
        return NULL;
    }

    return pHashTable;
}

/*
 * Clear out all entries.
 */
void dvmHashTableClear(HashTable* pHashTable)
{
    HashEntry* pEnt;
    int i;

    pEnt = pHashTable->pEntries;
    for (i = pHashTable->tableSize; --i >= 0; pEnt++) {
        if (pEnt->data == HASH_TOMBSTONE) {
            // nuke entry
            pEnt->data = NULL;
        } else if (pEnt->data != NULL) {
            // call free func then nuke entry
            if (pHashTable->freeFunc != NULL)
                (*pHashTable->freeFunc)(pEnt->data);
            pEnt->data = NULL;
        }
    }

    pHashTable->numEntries = 0;
    pHashTable->numDeadEntries = 0;
}

/*
 * Free the table.
 */
void dvmHashTableFree(HashTable* pHashTable)
{
    if (pHashTable == NULL)
        return;
    dvmHashTableClear(pHashTable);
    free(pHashTable->pEntries);
    free(pHashTable);
}

#ifndef NDEBUG
/*
 * Count up the number of tombstone entries in the hash table.
 */
static int countTombStones(HashTable* pHashTable)
{
    int i, count = 0;

    for (i = pHashTable->tableSize; --i >= 0;) {
        if (pHashTable->pEntries[i].data == HASH_TOMBSTONE)
            count++;
    }
    return count;
}
#endif

/*
 * Resize a hash table.  We do this when adding an entry increased the
 * size of the table beyond its comfy limit.
 *
 * This essentially requires re-inserting all elements into the new storage.
 *
 * If multiple threads can access the hash table, the table's lock should
 * have been grabbed before issuing the "lookup+add" call that led to the
 * resize, so we don't have a synchronization problem here.
 */
static bool resizeHash(HashTable* pHashTable, int newSize)
{
    HashEntry* pNewEntries;
    int i;

    assert(countTombStones(pHashTable) == pHashTable->numDeadEntries);
    //ALOGI("before: dead=%d", pHashTable->numDeadEntries);

    pNewEntries = (HashEntry*) calloc(newSize, sizeof(HashEntry));
    if (pNewEntries == NULL)
        return false;

    for (i = pHashTable->tableSize; --i >= 0;) {
        void* data = pHashTable->pEntries[i].data;
        if (data != NULL && data != HASH_TOMBSTONE) {
            int hashValue = pHashTable->pEntries[i].hashValue;
            int newIdx;

            /* probe for new spot, wrapping around */
            newIdx = hashValue & (newSize-1);
            while (pNewEntries[newIdx].data != NULL)
                newIdx = (newIdx + 1) & (newSize-1);

            pNewEntries[newIdx].hashValue = hashValue;
            pNewEntries[newIdx].data = data;
        }
    }

    free(pHashTable->pEntries);
    pHashTable->pEntries = pNewEntries;
    pHashTable->tableSize = newSize;
    pHashTable->numDeadEntries = 0;

    assert(countTombStones(pHashTable) == 0);
    return true;
}

/*
 * Look up an entry.
 *
 * We probe on collisions, wrapping around the table.
 */
void* dvmHashTableLookup(HashTable* pHashTable, u4 itemHash, void* item,
    HashCompareFunc cmpFunc, bool doAdd)
{
    HashEntry* pEntry;
    HashEntry* pEnd;
    void* result = NULL;

    assert(pHashTable->tableSize > 0);
    assert(item != HASH_TOMBSTONE);
    assert(item != NULL);

    /* jump to the first entry and probe for a match */
    pEntry = &pHashTable->pEntries[itemHash & (pHashTable->tableSize-1)];
    pEnd = &pHashTable->pEntries[pHashTable->tableSize];
    while (pEntry->data != NULL) {
        if (pEntry->data != HASH_TOMBSTONE &&
            pEntry->hashValue == itemHash &&
            (*cmpFunc)(pEntry->data, item) == 0)
        {
            /* match */
            //ALOGD("+++ match on entry %d", pEntry - pHashTable->pEntries);
            break;
        }

        pEntry++;
        if (pEntry == pEnd) {     /* wrap around to start */
            if (pHashTable->tableSize == 1)
                break;      /* edge case - single-entry table */
            pEntry = pHashTable->pEntries;
        }

        //ALOGI("+++ look probing %d...", pEntry - pHashTable->pEntries);
    }

    if (pEntry->data == NULL) {
        if (doAdd) {
            pEntry->hashValue = itemHash;
            pEntry->data = item;
            pHashTable->numEntries++;

            /*
             * We've added an entry.  See if this brings us too close to full.
             */
            if ((pHashTable->numEntries+pHashTable->numDeadEntries) * LOAD_DENOM
                > pHashTable->tableSize * LOAD_NUMER)
            {
                if (!resizeHash(pHashTable, pHashTable->tableSize * 2)) {
                    /* don't really have a way to indicate failure */
                    ALOGE("Dalvik hash resize failure");
                    dvmAbort();
                }
                /* note "pEntry" is now invalid */
            } else {
                //ALOGW("okay %d/%d/%d",
                //    pHashTable->numEntries, pHashTable->tableSize,
                //    (pHashTable->tableSize * LOAD_NUMER) / LOAD_DENOM);
            }

            /* full table is bad -- search for nonexistent never halts */
            assert(pHashTable->numEntries < pHashTable->tableSize);
            result = item;
        } else {
            assert(result == NULL);
        }
    } else {
        result = pEntry->data;
    }

    return result;
}

/*
 * Remove an entry from the table.
 *
 * Does NOT invoke the "free" function on the item.
 */
bool dvmHashTableRemove(HashTable* pHashTable, u4 itemHash, void* item)
{
    HashEntry* pEntry;
    HashEntry* pEnd;

    assert(pHashTable->tableSize > 0);

    /* jump to the first entry and probe for a match */
    pEntry = &pHashTable->pEntries[itemHash & (pHashTable->tableSize-1)];
    pEnd = &pHashTable->pEntries[pHashTable->tableSize];
    while (pEntry->data != NULL) {
        if (pEntry->data == item) {
            //ALOGI("+++ stepping on entry %d", pEntry - pHashTable->pEntries);
            pEntry->data = HASH_TOMBSTONE;
            pHashTable->numEntries--;
            pHashTable->numDeadEntries++;
            return true;
        }

        pEntry++;
        if (pEntry == pEnd) {     /* wrap around to start */
            if (pHashTable->tableSize == 1)
                break;      /* edge case - single-entry table */
            pEntry = pHashTable->pEntries;
        }

        //ALOGI("+++ del probing %d...", pEntry - pHashTable->pEntries);
    }

    return false;
}

/*
 * Scan every entry in the hash table and evaluate it with the specified
 * indirect function call. If the function returns 1, remove the entry from
 * the table.
 *
 * Does NOT invoke the "free" function on the item.
 *
 * Returning values other than 0 or 1 will abort the routine.
 */
int dvmHashForeachRemove(HashTable* pHashTable, HashForeachRemoveFunc func)
{
    int i, val;

    for (i = pHashTable->tableSize; --i >= 0;) {
        HashEntry* pEnt = &pHashTable->pEntries[i];

        if (pEnt->data != NULL && pEnt->data != HASH_TOMBSTONE) {
            val = (*func)(pEnt->data);
            if (val == 1) {
                pEnt->data = HASH_TOMBSTONE;
                pHashTable->numEntries--;
                pHashTable->numDeadEntries++;
            }
            else if (val != 0) {
                return val;
            }
        }
    }
    return 0;
}


/*
 * Execute a function on every entry in the hash table.
 *
 * If "func" returns a nonzero value, terminate early and return the value.
 */
int dvmHashForeach(HashTable* pHashTable, HashForeachFunc func, void* arg)
{
    int i, val;

    for (i = pHashTable->tableSize; --i >= 0;) {
        HashEntry* pEnt = &pHashTable->pEntries[i];

        if (pEnt->data != NULL && pEnt->data != HASH_TOMBSTONE) {
            val = (*func)(pEnt->data, arg);
            if (val != 0)
                return val;
        }
    }

    return 0;
}


/*
 * Look up an entry, counting the number of times we have to probe.
 *
 * Returns -1 if the entry wasn't found.
 */
static int countProbes(HashTable* pHashTable, u4 itemHash, const void* item,
    HashCompareFunc cmpFunc)
{
    HashEntry* pEntry;
    HashEntry* pEnd;
    int count = 0;

    assert(pHashTable->tableSize > 0);
    assert(item != HASH_TOMBSTONE);
    assert(item != NULL);

    /* jump to the first entry and probe for a match */
    pEntry = &pHashTable->pEntries[itemHash & (pHashTable->tableSize-1)];
    pEnd = &pHashTable->pEntries[pHashTable->tableSize];
    while (pEntry->data != NULL) {
        if (pEntry->data != HASH_TOMBSTONE &&
            pEntry->hashValue == itemHash &&
            (*cmpFunc)(pEntry->data, item) == 0)
        {
            /* match */
            break;
        }

        pEntry++;
        if (pEntry == pEnd) {     /* wrap around to start */
            if (pHashTable->tableSize == 1)
                break;      /* edge case - single-entry table */
            pEntry = pHashTable->pEntries;
        }

        count++;
    }
    if (pEntry->data == NULL)
        return -1;

    return count;
}

/*
 * Evaluate the amount of probing required for the specified hash table.
 *
 * We do this by running through all entries in the hash table, computing
 * the hash value and then doing a lookup.
 *
 * The caller should lock the table before calling here.
 */
void dvmHashTableProbeCount(HashTable* pHashTable, HashCalcFunc calcFunc,
    HashCompareFunc cmpFunc)
{
    int numEntries, minProbe, maxProbe, totalProbe;
    HashIter iter;

    numEntries = maxProbe = totalProbe = 0;
    minProbe = 65536*32767;

    for (dvmHashIterBegin(pHashTable, &iter); !dvmHashIterDone(&iter);
        dvmHashIterNext(&iter))
    {
        const void* data = (const void*)dvmHashIterData(&iter);
        int count;

        count = countProbes(pHashTable, (*calcFunc)(data), data, cmpFunc);

        numEntries++;

        if (count < minProbe)
            minProbe = count;
        if (count > maxProbe)
            maxProbe = count;
        totalProbe += count;
    }

    ALOGI("Probe: min=%d max=%d, total=%d in %d (%d), avg=%.3f",
        minProbe, maxProbe, totalProbe, numEntries, pHashTable->tableSize,
        (float) totalProbe / (float) numEntries);
}
