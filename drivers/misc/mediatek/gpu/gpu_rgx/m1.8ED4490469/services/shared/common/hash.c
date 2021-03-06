/*************************************************************************/ /*!
@File
@Title          Self scaling hash tables.
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description
   Implements simple self scaling hash tables. Hash collisions are
   handled by chaining entries together. Hash tables are increased in
   size when they become more than (50%?) full and decreased in size
   when less than (25%?) full. Hash tables are never decreased below
   their initial size.
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/

/* include/ */
#include "img_defs.h"
#include "img_types.h"
#include "pvr_debug.h"
#include "pvrsrv_error.h"

/* services/shared/include/ */
#include "hash.h"
#include "lock.h"

/* services/client/include/ or services/server/include/ */
#include "osfunc.h"
#include "allocmem.h"

#if defined(__KERNEL__)
#include "pvrsrv.h"
#else
#include "debug_utils.h"
#include <stdlib.h>
#include <stdio.h>
#endif

#define PRIVATE_MAX(a,b) ((a)>(b)?(a):(b))

#define	KEY_TO_INDEX(pHash, key, uSize) \
	((pHash)->pfnHashFunc((pHash)->uKeySize, (key), (uSize)) % (uSize))

#define	KEY_COMPARE(pHash, pKey1, pKey2) \
	((pHash)->pfnKeyComp((pHash)->uKeySize, (pKey1), (pKey2)))

/* Each entry in a hash table is placed into a bucket */
struct _BUCKET_
{
	IMG_UINT32 ui32Sig;

	/* the next bucket on the same chain */
	struct _BUCKET_ *pNext;

	/* entry value */
	uintptr_t v;

	/* entry key */
#if defined (WIN32)
	uintptr_t k[1];
#else
	uintptr_t k[];		/* PRQA S 0642 */ /* override dynamic array declaration warning */
#endif
};
typedef struct _BUCKET_ BUCKET;

struct _HASH_TABLE_
{
	/* current size of the hash table */
	IMG_UINT32 uSize;

	/* number of entries currently in the hash table */
	IMG_UINT32 uCount;

	/* the minimum size that the hash table should be re-sized to */
	IMG_UINT32 uMinimumSize;

	/* size of key in bytes */
	IMG_UINT32 uKeySize;

	/* hash function */
	HASH_FUNC *pfnHashFunc;

	/* key comparison function */
	HASH_KEY_COMP *pfnKeyComp;

	/* the hash table array */
	BUCKET **ppBucketTable;

	ATOMIC_T hRefCount;
};

#if !defined(__KERNEL__)
static inline void OSDumpStack(void)
{
	PVRSRVNativeDumpStackTrace(2, "IMG_HASH");
}
#endif

static void _inc_ref(HASH_TABLE * pHash)
{
	IMG_INT iRef = OSAtomicRead(&pHash->hRefCount);
	OSAtomicIncrement(&pHash->hRefCount);
	if (iRef)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s with %d references",
			__FUNCTION__, iRef));
		OSDumpStack();
	}
}

static void _dec_ref(HASH_TABLE * pHash)
{
	IMG_INT iRef = OSAtomicRead(&pHash->hRefCount);
	OSAtomicDecrement(&pHash->hRefCount);
	if (1 != iRef)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s with %d references",
			__FUNCTION__, iRef));
#if !defined(__KERNEL__)
		fflush(stderr);
		fflush(stdout);
#endif
		/*
		 * abort only on component exit
		 * to give chance of dumping both thread's call stacks
		 */
#if defined(__KERNEL__)
		BUG();
#else
		abort();
#endif
	}
}

#define BUCKET_SIG  0xBEA57FED
#define BUCKET_FREE 0xBCE7DEAD

static void _assert_bucket(BUCKET *pBucket, IMG_UINT32 uBucket, IMG_UINT32 uChain, const IMG_CHAR * pszFunc)
{
	if (BUCKET_SIG != pBucket->ui32Sig)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s invalid bucket %p [%d,%d] sig %0x08x",
			pszFunc, pBucket, uBucket, uChain, pBucket->ui32Sig));
		OSDumpStack();
	}
}

/*************************************************************************/ /*!
@Function       HASH_Func_Default
@Description    Hash function intended for hashing keys composed of
                uintptr_t arrays.
@Input          uKeySize     The size of the hash key, in bytes.
@Input          pKey         A pointer to the key to hash.
@Input          uHashTabLen  The length of the hash table.
@Return         The hash value.
*/ /**************************************************************************/
IMG_INTERNAL IMG_UINT32
HASH_Func_Default (size_t uKeySize, void *pKey, IMG_UINT32 uHashTabLen)
{
	uintptr_t *p = (uintptr_t *)pKey;
	IMG_UINT32 uKeyLen = uKeySize / sizeof(uintptr_t);
	IMG_UINT32 ui;
	IMG_UINT32 uHashKey = 0;

	PVR_UNREFERENCED_PARAMETER(uHashTabLen);

	PVR_ASSERT((uKeySize % sizeof(uintptr_t)) == 0);

	for (ui = 0; ui < uKeyLen; ui++)
	{
		IMG_UINT32 uHashPart = (IMG_UINT32)*p++;

		uHashPart += (uHashPart << 12);
		uHashPart ^= (uHashPart >> 22);
		uHashPart += (uHashPart << 4);
		uHashPart ^= (uHashPart >> 9);
		uHashPart += (uHashPart << 10);
		uHashPart ^= (uHashPart >> 2);
		uHashPart += (uHashPart << 7);
		uHashPart ^= (uHashPart >> 12);

		uHashKey += uHashPart;
	}

	return uHashKey;
}

/*************************************************************************/ /*!
@Function       HASH_Key_Comp_Default
@Description    Compares keys composed of uintptr_t arrays.
@Input          uKeySize    The size of the hash key, in bytes.
@Input          pKey1       Pointer to first hash key to compare.
@Input          pKey2       Pointer to second hash key to compare.
@Return         IMG_TRUE    The keys match.
                IMG_FALSE   The keys don't match.
*/ /**************************************************************************/
IMG_INTERNAL IMG_BOOL
HASH_Key_Comp_Default (size_t uKeySize, void *pKey1, void *pKey2)
{
	uintptr_t *p1 = (uintptr_t *)pKey1;
	uintptr_t *p2 = (uintptr_t *)pKey2;
	IMG_UINT32 uKeyLen = uKeySize / sizeof(uintptr_t);
	IMG_UINT32 ui;

	PVR_ASSERT((uKeySize % sizeof(uintptr_t)) == 0);

	for (ui = 0; ui < uKeyLen; ui++)
	{
		if (*p1++ != *p2++)
			return IMG_FALSE;
	}

	return IMG_TRUE;
}

/*************************************************************************/ /*!
@Function       _ChainInsert
@Description    Insert a bucket into the appropriate hash table chain.
@Input          pBucket       The bucket
@Input          ppBucketTable The hash table
@Input          uSize         The size of the hash table
@Return         PVRSRV_ERROR
*/ /**************************************************************************/
static void
_ChainInsert (HASH_TABLE *pHash, BUCKET *pBucket, BUCKET **ppBucketTable, IMG_UINT32 uSize)
{
	IMG_UINT32 uIndex;

	/* We assume that all parameters passed by the caller are valid. */
	PVR_ASSERT (pBucket != NULL);
	PVR_ASSERT (ppBucketTable != NULL);
	PVR_ASSERT (uSize != 0);

	uIndex = KEY_TO_INDEX(pHash, pBucket->k, uSize);	/* PRQA S 0432,0541 */ /* ignore dynamic array warning */
	pBucket->pNext = ppBucketTable[uIndex];
	ppBucketTable[uIndex] = pBucket;
}

/*************************************************************************/ /*!
@Function       _Rehash
@Description    Iterate over every entry in an old hash table and
                rehash into the new table.
@Input          ppOldTable   The old hash table
@Input          uOldSize     The size of the old hash table
@Input          ppNewTable   The new hash table
@Input          uNewSize     The size of the new hash table
@Return         None
*/ /**************************************************************************/
static void
_Rehash (HASH_TABLE *pHash,
         BUCKET **ppOldTable, IMG_UINT32 uOldSize,
         BUCKET **ppNewTable, IMG_UINT32 uNewSize)
{
	IMG_UINT32 uIndex;
	for (uIndex=0; uIndex< uOldSize; uIndex++)
    {
		IMG_UINT32 uCount = 0;
		BUCKET *pBucket;
		pBucket = ppOldTable[uIndex];
		while (pBucket != NULL)
		{
			BUCKET *pNextBucket = pBucket->pNext;
			_assert_bucket(pBucket, uIndex, uCount, __func__);
			_ChainInsert (pHash, pBucket, ppNewTable, uNewSize);
			pBucket = pNextBucket;
			uCount++;
		}
    }
}

/*************************************************************************/ /*!
@Function       _Resize
@Description    Attempt to resize a hash table, failure to allocate a
                new larger hash table is not considered a hard failure.
                We simply continue and allow the table to fill up, the
                effect is to allow hash chains to become longer.
@Input          pHash      Hash table to resize.
@Input          uNewSize   Required table size.
@Return         IMG_TRUE Success
                IMG_FALSE Failed
*/ /**************************************************************************/
static IMG_BOOL
_Resize (HASH_TABLE *pHash, IMG_UINT32 uNewSize)
{
	if (uNewSize != pHash->uSize)
    {
		BUCKET **ppNewTable;
        IMG_UINT32 uIndex;

#if defined(__linux__) && defined(__KERNEL__)
		ppNewTable = OSAllocMemNoStats(sizeof (BUCKET *) * uNewSize);
#else
		ppNewTable = OSAllocMem(sizeof (BUCKET *) * uNewSize);
#endif
		if (ppNewTable == NULL)
        {
			PVR_DPF((PVR_DBG_ERROR, "%s: call to OSAllocMem failed", __func__));
            return IMG_FALSE;
        }

        for (uIndex=0; uIndex<uNewSize; uIndex++)
            ppNewTable[uIndex] = NULL;

        _Rehash(pHash, pHash->ppBucketTable, pHash->uSize, ppNewTable, uNewSize);

#if defined(__linux__) && defined(__KERNEL__)
        OSFreeMemNoStats(pHash->ppBucketTable);
#else
        OSFreeMem(pHash->ppBucketTable);
#endif
        /*not nulling pointer, being reassigned just below*/
        pHash->ppBucketTable = ppNewTable;
        pHash->uSize = uNewSize;
    }
    return IMG_TRUE;
}


/*************************************************************************/ /*!
@Function       HASH_Create_Extended
@Description    Create a self scaling hash table, using the supplied
                key size, and the supplied hash and key comparsion
                functions.
@Input          uInitialLen   Initial and minimum length of the
                              hash table, where the length refers to the number
                              of entries in the hash table, not its size in
                              bytes.
@Input          uKeySize      The size of the key, in bytes.
@Input          pfnHashFunc   Pointer to hash function.
@Input          pfnKeyComp    Pointer to key comparsion function.
@Return         NULL or hash table handle.
*/ /**************************************************************************/
IMG_INTERNAL
HASH_TABLE * HASH_Create_Extended (IMG_UINT32 uInitialLen, size_t uKeySize, HASH_FUNC *pfnHashFunc, HASH_KEY_COMP *pfnKeyComp)
{
	HASH_TABLE *pHash;
	IMG_UINT32 uIndex;

	if (uInitialLen == 0 || uKeySize == 0 || pfnHashFunc == NULL || pfnKeyComp == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "HASH_Create_Extended: invalid input parameters"));
		return NULL;
	}

	PVR_DPF ((PVR_DBG_MESSAGE, "HASH_Create_Extended: InitialSize=0x%x", uInitialLen));

#if defined(__linux__) && defined(__KERNEL__)
	pHash = OSAllocMemNoStats(sizeof(HASH_TABLE));
#else
	pHash = OSAllocMem(sizeof(HASH_TABLE));
#endif
    if (pHash == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: call to OSAllocMem failed", __func__));
		return NULL;
	}

	pHash->uCount = 0;
	pHash->uSize = uInitialLen;
	pHash->uMinimumSize = uInitialLen;
	pHash->uKeySize = uKeySize;
	pHash->pfnHashFunc = pfnHashFunc;
	pHash->pfnKeyComp = pfnKeyComp;
	OSAtomicWrite(&pHash->hRefCount, 0);

#if defined(__linux__) && defined(__KERNEL__)
    pHash->ppBucketTable = OSAllocMemNoStats(sizeof (BUCKET *) * pHash->uSize);
#else
    pHash->ppBucketTable = OSAllocMem(sizeof (BUCKET *) * pHash->uSize);
#endif
    if (pHash->ppBucketTable == NULL)
    {
		PVR_DPF((PVR_DBG_ERROR, "%s: call to OSAllocMem for bucket table failed", __func__));
#if defined(__linux__) && defined(__KERNEL__)
		OSFreeMemNoStats(pHash);
#else
		OSFreeMem(pHash);
#endif
		/*not nulling pointer, out of scope*/
		return NULL;
    }

	for (uIndex=0; uIndex<pHash->uSize; uIndex++)
		pHash->ppBucketTable[uIndex] = NULL;
	return pHash;
}

/*************************************************************************/ /*!
@Function       HASH_Create
@Description    Create a self scaling hash table with a key
                consisting of a single uintptr_t, and using
                the default hash and key comparison functions.
@Input          uInitialLen   Initial and minimum length of the
                              hash table, where the length refers to the
                              number of entries in the hash table, not its size
                              in bytes.
@Return         NULL or hash table handle.
*/ /**************************************************************************/
IMG_INTERNAL
HASH_TABLE * HASH_Create (IMG_UINT32 uInitialLen)
{
	return HASH_Create_Extended(uInitialLen, sizeof(uintptr_t),
		&HASH_Func_Default, &HASH_Key_Comp_Default);
}

/*************************************************************************/ /*!
@Function       HASH_Delete
@Description    Delete a hash table created by HASH_Create_Extended or
                HASH_Create.  All entries in the table must have been
                removed before calling this function.
@Input          pHash     Hash table
@Return         None
*/ /**************************************************************************/
IMG_INTERNAL void
HASH_Delete (HASH_TABLE *pHash)
{
	IMG_BOOL bDoCheck = IMG_TRUE;
#if defined(__KERNEL__) && !defined(__QNXNTO__)
	PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();

	if (psPVRSRVData != NULL)
	{
		if (psPVRSRVData->eServicesState != PVRSRV_SERVICES_STATE_OK)
		{
			bDoCheck = IMG_FALSE;
		}
	}
#if defined(PVRSRV_FORCE_UNLOAD_IF_BAD_STATE)
	else
	{
		bDoCheck = IMG_FALSE;
	}
#endif
#endif
	if (pHash != NULL)
    {
		PVR_DPF ((PVR_DBG_MESSAGE, "HASH_Delete"));

		_inc_ref(pHash);
		if (bDoCheck)
		{
			PVR_ASSERT (pHash->uCount==0);
		}
		if(pHash->uCount != 0)
		{
			IMG_UINT32 uiEntriesLeft = pHash->uCount;
			IMG_UINT32 i;
			PVR_DPF ((PVR_DBG_ERROR, "%s: Leak detected in hash table!", __func__));
			PVR_DPF ((PVR_DBG_ERROR, "%s: Likely Cause: client drivers not freeing allocations before destroying devmemcontext", __func__));
			PVR_DPF ((PVR_DBG_ERROR, "%s: Removing remaining %u hash entries.", __func__, uiEntriesLeft));

			for (i = 0; i < uiEntriesLeft; i++)
			{
				pHash->ppBucketTable[i]->ui32Sig = BUCKET_FREE;
#if defined(__linux__) && defined(__KERNEL__)
				OSFreeMemNoStats(pHash->ppBucketTable[i]);
#else
				OSFreeMem(pHash->ppBucketTable[i]);
#endif
			}
		}
#if defined(__linux__) && defined(__KERNEL__)
		OSFreeMemNoStats(pHash->ppBucketTable);
#else
		OSFreeMem(pHash->ppBucketTable);
#endif
		pHash->ppBucketTable = NULL;
#if defined(__linux__) && defined(__KERNEL__)
		OSFreeMemNoStats(pHash);
#else
		OSFreeMem(pHash);
#endif
		/*not nulling pointer, copy on stack*/
    }
}

/*************************************************************************/ /*!
@Function       HASH_Insert_Extended
@Description    Insert a key value pair into a hash table created
                with HASH_Create_Extended.
@Input          pHash     Hash table
@Input          pKey      Pointer to the key.
@Input          v         The value associated with the key.
@Return         IMG_TRUE  - success
                IMG_FALSE  - failure
*/ /**************************************************************************/
IMG_INTERNAL IMG_BOOL
HASH_Insert_Extended (HASH_TABLE *pHash, void *pKey, uintptr_t v)
{
	BUCKET *pBucket;

	PVR_ASSERT (pHash != NULL);

	if (pHash == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "HASH_Insert_Extended: invalid parameter"));
		return IMG_FALSE;
	}

	_inc_ref(pHash);
#if defined(__linux__) && defined(__KERNEL__)
	pBucket = OSAllocMemNoStats(sizeof(BUCKET) + pHash->uKeySize);
#else
	pBucket = OSAllocMem(sizeof(BUCKET) + pHash->uKeySize);
#endif
    if (pBucket == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: call to OSAllocMem failed", __func__));
		return IMG_FALSE;
	}

	pBucket->ui32Sig = BUCKET_SIG;
	pBucket->v = v;
	/* PRQA S 0432,0541 1 */ /* ignore warning about dynamic array k (linux)*/
	OSCachedMemCopy(pBucket->k, pKey, pHash->uKeySize);

	_ChainInsert (pHash, pBucket, pHash->ppBucketTable, pHash->uSize);

	pHash->uCount++;

	/* check if we need to think about re-balancing */
	if (pHash->uCount << 1 > pHash->uSize)
    {
        /* Ignore the return code from _Resize because the hash table is
           still in a valid state and although not ideally sized, it is still
           functional */
        _Resize (pHash, pHash->uSize << 1);
    }

	_dec_ref(pHash);
	return IMG_TRUE;
}

/*************************************************************************/ /*!
@Function       HASH_Insert
@Description    Insert a key value pair into a hash table created with
                HASH_Create.
@Input          pHash     Hash table
@Input          k         The key value.
@Input          v         The value associated with the key.
@Return         IMG_TRUE - success.
                IMG_FALSE - failure.
*/ /**************************************************************************/
IMG_INTERNAL IMG_BOOL
HASH_Insert (HASH_TABLE *pHash, uintptr_t k, uintptr_t v)
{
	return HASH_Insert_Extended(pHash, &k, v);
}

/*************************************************************************/ /*!
@Function       HASH_Remove_Extended
@Description    Remove a key from a hash table created with
                HASH_Create_Extended.
@Input          pHash     Hash table
@Input          pKey      Pointer to key.
@Return         0 if the key is missing, or the value associated with the key.
*/ /**************************************************************************/
IMG_INTERNAL uintptr_t
HASH_Remove_Extended(HASH_TABLE *pHash, void *pKey)
{
	BUCKET **ppBucket;
	IMG_UINT32 uIndex;
	IMG_UINT32 uCount = 0;

	PVR_ASSERT (pHash != NULL);

	if (pHash == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "HASH_Remove_Extended: Null hash table"));
		return 0;
	}

	_inc_ref(pHash);
	uIndex = KEY_TO_INDEX(pHash, pKey, pHash->uSize);

	for (ppBucket = &(pHash->ppBucketTable[uIndex]); *ppBucket != NULL; ppBucket = &((*ppBucket)->pNext))
	{
		_assert_bucket(*ppBucket, uIndex, uCount, __func__);
		/* PRQA S 0432,0541 1 */ /* ignore warning about dynamic array k */
		if (KEY_COMPARE(pHash, (*ppBucket)->k, pKey))
		{
			BUCKET *pBucket = *ppBucket;
			uintptr_t v = pBucket->v;
			(*ppBucket) = pBucket->pNext;

			pBucket->ui32Sig = BUCKET_FREE;
#if defined(__linux__) && defined(__KERNEL__)
			OSFreeMemNoStats(pBucket);
#else
			OSFreeMem(pBucket);
#endif
			/*not nulling original pointer, already overwritten*/

			pHash->uCount--;

			/* check if we need to think about re-balancing */
			if (pHash->uSize > (pHash->uCount << 2) &&
                pHash->uSize > pHash->uMinimumSize)
            {
                /* Ignore the return code from _Resize because the
                   hash table is still in a valid state and although
                   not ideally sized, it is still functional */
				_Resize (pHash,
                         PRIVATE_MAX (pHash->uSize >> 1,
                                      pHash->uMinimumSize));
            }

			_dec_ref(pHash);
			return v;
		}
		uCount++;
	}
	PVR_DPF((PVR_DBG_ERROR, "HASH_Remove_Extended: key not found"));
	_dec_ref(pHash);
	return 0;
}

/*************************************************************************/ /*!
@Function       HASH_Remove
@Description    Remove a key value pair from a hash table created
                with HASH_Create.
@Input          pHash     Hash table
@Input          k         The key
@Return         0 if the key is missing, or the value associated with the key.
*/ /**************************************************************************/
IMG_INTERNAL uintptr_t
HASH_Remove (HASH_TABLE *pHash, uintptr_t k)
{
	return HASH_Remove_Extended(pHash, &k);
}

/*************************************************************************/ /*!
@Function       HASH_Retrieve_Extended
@Description    Retrieve a value from a hash table created with
                HASH_Create_Extended.
@Input          pHash     Hash table
@Input          pKey      Pointer to the key.
@Return         0 if the key is missing, or the value associated with the key.
*/ /**************************************************************************/
IMG_INTERNAL uintptr_t
HASH_Retrieve_Extended (HASH_TABLE *pHash, void *pKey)
{
	BUCKET **ppBucket;
	IMG_UINT32 uIndex;
	IMG_UINT32 uCount = 0;

	PVR_ASSERT (pHash != NULL);

	if (pHash == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "HASH_Retrieve_Extended: Null hash table"));
		return 0;
	}

	_inc_ref(pHash);
	uIndex = KEY_TO_INDEX(pHash, pKey, pHash->uSize);

	for (ppBucket = &(pHash->ppBucketTable[uIndex]); *ppBucket != NULL; ppBucket = &((*ppBucket)->pNext))
	{
		_assert_bucket(*ppBucket, uIndex, uCount, __func__);
		/* PRQA S 0432,0541 1 */ /* ignore warning about dynamic array k */
		if (KEY_COMPARE(pHash, (*ppBucket)->k, pKey))
		{
			BUCKET *pBucket = *ppBucket;
			uintptr_t v = pBucket->v;

			_dec_ref(pHash);
			return v;
		}
		uCount++;
	}
	/* No error here, as checking for a key that is not present is a valid operation */
	_dec_ref(pHash);
	return 0;
}

/*************************************************************************/ /*!
@Function       HASH_Retrieve
@Description    Retrieve a value from a hash table created with
                HASH_Create.
@Input          pHash     Hash table
@Input          k         The key
@Return         0 if the key is missing, or the value associated with the key.
*/ /**************************************************************************/
IMG_INTERNAL uintptr_t
HASH_Retrieve (HASH_TABLE *pHash, uintptr_t k)
{
	return HASH_Retrieve_Extended(pHash, &k);
}

/*************************************************************************/ /*!
@Function       HASH_Iterate
@Description    Iterate over every entry in the hash table
@Input          pHash - Hash table to iterate
@Input          pfnCallback - Callback to call with the key and data for each
							  entry in the hash table
@Return         Callback error if any, otherwise PVRSRV_OK
*/ /**************************************************************************/
IMG_INTERNAL PVRSRV_ERROR
HASH_Iterate(HASH_TABLE *pHash, HASH_pfnCallback pfnCallback)
{
    IMG_UINT32 uIndex;
    IMG_UINT32 uCount = 0;
    _inc_ref(pHash);
    for (uIndex=0; uIndex < pHash->uSize; uIndex++)
    {
        BUCKET *pBucket;
        pBucket = pHash->ppBucketTable[uIndex];
        while (pBucket != NULL)
        {
            PVRSRV_ERROR eError;
            BUCKET *pNextBucket = pBucket->pNext;

            _assert_bucket(pBucket, uIndex, uCount, __func__);
            eError = pfnCallback((uintptr_t) ((void *) *(pBucket->k)), (uintptr_t) pBucket->v);

            /* The callback might want us to break out early */
            if (eError != PVRSRV_OK)
            {
                _dec_ref(pHash);
                return eError;
            }

            pBucket = pNextBucket;
            uCount++;
        }
    }
    _dec_ref(pHash);
    return PVRSRV_OK;
}

#ifdef HASH_TRACE
/*************************************************************************/ /*!
@Function       HASH_Dump
@Description    To dump the contents of a hash table in human readable
                form.
@Input          pHash     Hash table
*/ /**************************************************************************/
void
HASH_Dump (HASH_TABLE *pHash)
{
	IMG_UINT32 uIndex;
	IMG_UINT32 uMaxLength=0;
	IMG_UINT32 uEmptyCount=0;

	PVR_ASSERT (pHash != NULL);
	for (uIndex=0; uIndex<pHash->uSize; uIndex++)
	{
		BUCKET *pBucket;
		IMG_UINT32 uLength = 0;
		if (pHash->ppBucketTable[uIndex] == NULL)
		{
			uEmptyCount++;
		}
		for (pBucket=pHash->ppBucketTable[uIndex];
				pBucket != NULL;
				pBucket = pBucket->pNext)
		{
			uLength++;
		}
		uMaxLength = PRIVATE_MAX (uMaxLength, uLength);
	}

	PVR_TRACE(("hash table: uMinimumSize=%d  size=%d  count=%d",
			pHash->uMinimumSize, pHash->uSize, pHash->uCount));
	PVR_TRACE(("  empty=%d  max=%d", uEmptyCount, uMaxLength));
}
#endif
