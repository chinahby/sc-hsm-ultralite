/**
 * SmartCard-HSM PKCS#11 Module
 *
 * Copyright (c) 2013, CardContact Systems GmbH, Minden, Germany
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of CardContact Systems GmbH nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL CardContact Systems GmbH BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file    p11generic.c
 * @author  Frank Thater, Andreas Schwier
 * @brief   General module functions at the PKCS#11 interface
 */

#include <string.h>

#include <pkcs11/cryptoki.h>

#include <pkcs11/p11generic.h>
#include <pkcs11/session.h>
#include <pkcs11/slotpool.h>
#include <pkcs11/strbpcpy.h>

#ifdef DEBUG
#include <pkcs11/debug.h>
#endif

/*
 * Set up the global context structure.
 *
 */
struct p11Context_t *context = NULL;



/*
 * Initialize the PKCS#11 function list.
 *
 */
CK_FUNCTION_LIST pkcs11_function_list = {
		{ 2, 20 },
		C_Initialize,
		C_Finalize,
		C_GetInfo,
		C_GetFunctionList,
		C_GetSlotList,
		C_GetSlotInfo,
		C_GetTokenInfo,
		C_GetMechanismList,
		C_GetMechanismInfo,
		C_InitToken,
		C_InitPIN,
		C_SetPIN,
		C_OpenSession,
		C_CloseSession,
		C_CloseAllSessions,
		C_GetSessionInfo,
		C_GetOperationState,
		C_SetOperationState,
		C_Login,
		C_Logout,
		C_CreateObject,
		C_CopyObject,
		C_DestroyObject,
		C_GetObjectSize,
		C_GetAttributeValue,
		C_SetAttributeValue,
		C_FindObjectsInit,
		C_FindObjects,
		C_FindObjectsFinal,
		C_EncryptInit,
		C_Encrypt,
		C_EncryptUpdate,
		C_EncryptFinal,
		C_DecryptInit,
		C_Decrypt,
		C_DecryptUpdate,
		C_DecryptFinal,
		C_DigestInit,
		C_Digest,
		C_DigestUpdate,
		C_DigestKey,
		C_DigestFinal,
		C_SignInit,
		C_Sign,
		C_SignUpdate,
		C_SignFinal,
		C_SignRecoverInit,
		C_SignRecover,
		C_VerifyInit,
		C_Verify,
		C_VerifyUpdate,
		C_VerifyFinal,
		C_VerifyRecoverInit,
		C_VerifyRecover,
		C_DigestEncryptUpdate,
		C_DecryptDigestUpdate,
		C_SignEncryptUpdate,
		C_DecryptVerifyUpdate,
		C_GenerateKey,
		C_GenerateKeyPair,
		C_WrapKey,
		C_UnwrapKey,
		C_DeriveKey,
		C_SeedRandom,
		C_GenerateRandom,
		C_GetFunctionStatus,
		C_CancelFunction,
		C_WaitForSlotEvent
};



/**
 * C_Initialize initializes the Cryptoki library.
 *
 */
CK_DECLARE_FUNCTION(CK_RV, C_Initialize)
(
		CK_VOID_PTR   pInitArgs  /* if this is not NULL_PTR, it gets
		 * cast to CK_C_INITIALIZE_ARGS_PTR
		 * and dereferenced */
)
{
	/* Make sure the cryptoki has not been initialized */
	if (context != NULL) {
		return CKR_CRYPTOKI_ALREADY_INITIALIZED;
	}

#ifdef CreateMutex /* windows defines it */
#undef CreateMutex
#endif

	if (pInitArgs) {
		CK_C_INITIALIZE_ARGS_PTR p = (CK_C_INITIALIZE_ARGS_PTR)pInitArgs;
		int mutexSupplied =	p->CreateMutex && p->DestroyMutex && p->LockMutex && p->UnlockMutex;
		int osLockingOk = (p->flags & CKF_OS_LOCKING_OK) != 0;
		/* according to the spec */
		if (p->pReserved != NULL) {
			return CKR_ARGUMENTS_BAD;
		}
		if (!mutexSupplied && (p->CreateMutex || p->DestroyMutex || p->LockMutex && p->UnlockMutex)) {
			return CKR_ARGUMENTS_BAD;
		}
		if (!osLockingOk && mutexSupplied) {
			return CKR_CANT_LOCK;
		}
	}

	context = (struct p11Context_t *)calloc(1, sizeof(struct p11Context_t));

	if (context == NULL) {
		return CKR_HOST_MEMORY;
	}

#ifdef DEBUG
	initDebug(context);
#endif

	{
		FUNC_CALLED();

		initSessionPool(&context->sessionPool);

		initSlotPool(&context->slotPool);

		FUNC_RETURNS(CKR_OK);
	}
}



/**
 * C_Finalize indicates that an application is done with the
 * Cryptoki library.
 *
 */
CK_DECLARE_FUNCTION(CK_RV, C_Finalize)
(
		CK_VOID_PTR   pReserved  /* reserved.  Should be NULL_PTR */
)
{
	FUNC_CALLED();

	if (context != NULL) {

		terminateSessionPool(&context->sessionPool);

		terminateSlotPool(&context->slotPool);

#ifdef DEBUG
		termDebug(context);
#endif

		free(context);
		context = NULL;

	}

	FUNC_RETURNS(CKR_OK);
}



/**
 * C_GetInfo returns general information about Cryptoki.
 *
 * @param pInfo     Pointer to structure for the information.
 *
 * @return
 *                   <P><TABLE>
 *                   <TR><TD>Code</TD><TD>Meaning</TD></TR>
 *                   <TR>
 *                   <TD>CKR_OK         </TD>
 *                   <TD>Success</TD>
 *                   </TR>
 *                   </TABLE></P>
 */
CK_DECLARE_FUNCTION(CK_RV, C_GetInfo)
(
		CK_INFO_PTR   pInfo  /* location that receives information */
)
{
	FUNC_CALLED();

	if (context == NULL) {
		FUNC_FAILS(CKR_CRYPTOKI_NOT_INITIALIZED, "C_Initialize not called");
	}

	if (!isValidPtr(pInfo)) {
		FUNC_FAILS(CKR_ARGUMENTS_BAD, "Invalid pointer argument");
	}

	memset(pInfo, 0, sizeof(CK_INFO));

	pInfo->cryptokiVersion.major = 2;
	pInfo->cryptokiVersion.minor = 20;

	strbpcpy(pInfo->manufacturerID,
			"CardContact (www.cardcontact.de)",
			sizeof(pInfo->manufacturerID));
	strbpcpy(pInfo->libraryDescription,
			"SmartCard-HSM R/O with CT-API",
			sizeof(pInfo->libraryDescription));
	pInfo->libraryVersion.major = 1;
	pInfo->libraryVersion.minor = 0;

	FUNC_RETURNS(CKR_OK);
}



/**
 * C_GetFunctionList returns the function list.
 *
 */
CK_DECLARE_FUNCTION(CK_RV, C_GetFunctionList)
(
		CK_FUNCTION_LIST_PTR_PTR ppFunctionList  /* receives pointer to
		 * function list       */
)
{
	FUNC_CALLED();

	if (!isValidPtr(ppFunctionList)) {
		FUNC_RETURNS(CKR_ARGUMENTS_BAD);
	}

	*ppFunctionList = &pkcs11_function_list;

	FUNC_RETURNS(CKR_OK);
}
