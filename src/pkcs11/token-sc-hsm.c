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
 * @file    token-sc-hsm.c
 * @author  Andreas Schwier
 * @brief   Token implementation for a SmartCard-HSM
 */

#include <string.h>
#include <assert.h>
#include "token-sc-hsm.h"

#include <pkcs11/slot.h>
#include <pkcs11/object.h>
#include <pkcs11/token.h>
#include <pkcs11/certificateobject.h>
#include <pkcs11/privatekeyobject.h>
#include <pkcs11/strbpcpy.h>
#include <pkcs11/asn1.h>
#include <pkcs11/pkcs15.h>
#include <pkcs11/debug.h>


static unsigned char aid[] = { 0xE8,0x2B,0x06,0x01,0x04,0x01,0x81,0xC3,0x1F,0x02,0x01 };



static token_sc_hsm_t *getPrivateData(struct p11Token_t *token)
{
	return (token_sc_hsm_t *)(token + 1);
}



static int checkPINStatus(struct p11Slot_t *slot)
{
	int rc;
	unsigned short SW1SW2;
	FUNC_CALLED();

	rc = transmitAPDU(slot, 0x00, 0x20, 0x00, 0x81,
			0, NULL,
			0, NULL, 0, &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	FUNC_RETURNS(SW1SW2);
}



static int selectApplet(struct p11Slot_t *slot)
{
	int rc;
	unsigned short SW1SW2;
	FUNC_CALLED();

	rc = transmitAPDU(slot, 0x00, 0xA4, 0x04, 0x0C,
			sizeof(aid), aid,
			0, NULL, 0, &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		FUNC_FAILS(-1, "Token is not a SmartCard-HSM");
	}

	FUNC_RETURNS(CKR_OK);
}



static int enumerateObjects(struct p11Slot_t *slot, unsigned char *filelist, size_t len)
{
	int rc;
	unsigned short SW1SW2;
	FUNC_CALLED();

	rc = transmitAPDU(slot, 0x80, 0x58, 0x00, 0x00,
			0, NULL,
			65536, filelist, len, &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		FUNC_FAILS(-1, "Token did not enumerate objects");
	}

	FUNC_RETURNS(rc);
}



static int readEF(struct p11Slot_t *slot, unsigned short fid, unsigned char *content, size_t len)
{
	int rc;
	unsigned short SW1SW2;
	FUNC_CALLED();

	rc = transmitAPDU(slot, 0x00, 0xB1, fid >> 8, fid & 0xFF,
			4, (unsigned char*)"\x54\x02\x00\x00",
			65536, content, len, &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		FUNC_FAILS(-1, "Read EF failed");
	}

	FUNC_RETURNS(rc);
}



static int addEECertificateObject(struct p11Token_t *token, unsigned char id)
{
	CK_OBJECT_CLASS class = CKO_CERTIFICATE;
	CK_CERTIFICATE_TYPE certType = CKC_X_509;
	CK_UTF8CHAR label[10];
	CK_BBOOL true = CK_TRUE;
	CK_BBOOL false = CK_FALSE;
	CK_BYTE certValue[MAX_CERTIFICATE_SIZE];
	CK_ATTRIBUTE template[] = {
			{ CKA_CLASS, &class, sizeof(class) },
			{ CKA_CERTIFICATE_TYPE, &certType, sizeof(certType) },
			{ CKA_TOKEN, &true, sizeof(true) },
			{ CKA_PRIVATE, &false, sizeof(false) },
			{ CKA_LABEL, label, sizeof(label) - 1 },
			{ CKA_ID, &id, sizeof(id) },
			{ CKA_VALUE, certValue, sizeof(certValue) }
	};
	struct p11Object_t *object;
	token_sc_hsm_t *sc;
	struct p15PrivateKeyDescription *p15 = NULL;
	unsigned char prkd[MAX_P15_SIZE], *spk;
	int rc;

	FUNC_CALLED();

	rc = readEF(token->slot, (PRKD_PREFIX << 8) | id, prkd, sizeof(prkd));

	if (rc < 0) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "Error reading private key description");
	}

	rc = decodePrivateKeyDescription(prkd, rc, &p15);

	if (rc < 0) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "Error decoding private key description");
	}

	rc = readEF(token->slot, (EE_CERTIFICATE_PREFIX << 8) | id, certValue, sizeof(certValue));

	if (rc < 0) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "Error reading certificate");
	}
	template[6].ulValueLen = rc;

	if (certValue[0] != ASN1_SEQUENCE) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "Error not a certificate");
	}

	object = calloc(sizeof(struct p11Object_t), 1);

	if (object == NULL) {
		freePrivateKeyDescription(&p15);
		FUNC_FAILS(CKR_HOST_MEMORY, "Out of memory");
	}

	if (p15->coa.label) {
		template[4].pValue = p15->coa.label;
	} else {
		sprintf((char *)label, "Cert#%d", id);
	}
	template[4].ulValueLen = strlen(template[4].pValue);

	if (p15->id) {
		template[5].pValue = p15->id;
		template[5].ulValueLen = p15->idlen;
	}

	rc = createCertificateObject(template, 7, object);

	if (rc != CKR_OK) {
		freePrivateKeyDescription(&p15);
		free(object);
		FUNC_FAILS(rc, "Could not create certificate key object");
	}

	rc = populateIssuerSubjectSerial(object);

	if (rc != CKR_OK) {
#ifdef DEBUG
		debug("populateIssuerSubjectSerial() failed\n");
#endif
	}

	if (getSubjectPublicKeyInfo(object, &spk) == CKR_OK) {
		sc = getPrivateData(token);
		sc->publickeys[id] = spk;
	}

	object->tokenid = (int)id;
	object->keysize = p15->keysize;

	addTokenObject(token, object, TRUE);
	freePrivateKeyDescription(&p15);
	FUNC_RETURNS(CKR_OK);
}



static int getSignatureSize(CK_MECHANISM_TYPE mech, struct p11Object_t *object)
{
	switch(mech) {
	case CKM_RSA_X_509:
	case CKM_RSA_PKCS:
	case CKM_SHA1_RSA_PKCS:
	case CKM_SHA256_RSA_PKCS:
	case CKM_SHA1_RSA_PKCS_PSS:
	case CKM_SHA256_RSA_PKCS_PSS:
		return object->keysize >> 3;
	case CKM_ECDSA:
	case CKM_ECDSA_SHA1:
		return object->keysize >> 2;
	default:
		return -1;
	}
}



static int getAlgorithmIdForSigning(CK_MECHANISM_TYPE mech)
{
	switch(mech) {
	case CKM_RSA_X_509:
	case CKM_RSA_PKCS:
		return ALGO_RSA_RAW;
	case CKM_SHA1_RSA_PKCS:
		return ALGO_RSA_PKCS1_SHA1;
	case CKM_SHA256_RSA_PKCS:
		return ALGO_RSA_PKCS1_SHA256;
	case CKM_SHA1_RSA_PKCS_PSS:
		return ALGO_RSA_PSS_SHA1;
	case CKM_SHA256_RSA_PKCS_PSS:
		return ALGO_RSA_PSS_SHA256;
	case CKM_ECDSA:
		return ALGO_EC_RAW;
	case CKM_ECDSA_SHA1:
		return ALGO_EC_SHA1;
	default:
		return -1;
	}
}



static int getAlgorithmIdForDecryption(CK_MECHANISM_TYPE mech)
{
	switch(mech) {
	case CKM_RSA_X_509:
	case CKM_RSA_PKCS:
		return ALGO_RSA_DECRYPT;
	default:
		return -1;
	}
}



static int decodeECDSASignature(unsigned char *data, int datalen,
								unsigned char *out, int outlen)
{
	int fieldsizebytes, i, r, taglen;
	unsigned char *po, *value;

	FUNC_CALLED();

	r = asn1Validate(data, datalen);

	if (r != 0) {
		FUNC_FAILS(-1, "Signature is not a valid TLV structure");
	}

	// Determine field size from length of signature
	if (datalen <= 58) {			// 192 bit curve = 24 * 2 + 10 byte maximum DER signature
		fieldsizebytes = 24;
	} else if (datalen <= 66) {		// 224 bit curve = 28 * 2 + 10 byte maximum DER signature
		fieldsizebytes = 28;
	} else if (datalen <= 74) {		// 256 bit curve = 32 * 2 + 10 byte maximum DER signature
		fieldsizebytes = 32;
	} else if (datalen <= 90) {		// 320 bit curve = 40 * 2 + 10 byte maximum DER signature
		fieldsizebytes = 40;
	} else {
		fieldsizebytes = 64;
	}

#ifdef DEBUG
	debug("Field size %d, signature buffer size %d\n", fieldsizebytes, outlen);
#endif

	if (outlen < (fieldsizebytes * 2)) {
		FUNC_FAILS(-1, "output too small for EC signature");
	}

	memset(out, 0, outlen);

	po = data;
	if (asn1Tag(&po) != ASN1_SEQUENCE) {
		FUNC_FAILS(-1, "Signature not encapsulated in SEQUENCE");
	}

	r = asn1Length(&po);
	if ((r < 8) || (r > 137)) {
		FUNC_FAILS(-1, "Invalid signature size");
	}

	for (i = 0; i < 2; i++) {
		if (asn1Tag(&po) != ASN1_INTEGER) {
			FUNC_FAILS(-1, "Coordinate not encapsulated in INTEGER");
		}

		taglen = asn1Length(&po);
		value = po;
		po += taglen;

		if (taglen > fieldsizebytes) { /* drop leading 00 if present */
			if (*value != 0x00) {
				FUNC_FAILS(-1, "Invalid value in coordinate");
			}
			value++;
			taglen--;
		}
		memcpy(out + fieldsizebytes * i + fieldsizebytes - taglen , value, taglen);
	}
	FUNC_RETURNS(fieldsizebytes << 1);
}



static int sc_hsm_C_SignInit(struct p11Object_t *object, CK_MECHANISM_PTR mech)
{
	int algo;

	FUNC_CALLED();

	algo = getAlgorithmIdForSigning(mech->mechanism);
	if (algo < 0) {
		FUNC_FAILS(CKR_MECHANISM_INVALID, "Mechanism not supported");
	}

	FUNC_RETURNS(CKR_OK);
}



static void applyPKCSPadding(unsigned char *di, int dilen, unsigned char *buff, int bufflen)
{
	int i;

	if (dilen + 4 > bufflen) {
		return;
	}

	*buff++ = 0x00;
	*buff++ = 0x01;
	for (i = bufflen - dilen - 3; i > 0; i--) {
		*buff++ = 0xFF;
	}

	*buff++ = 0x00;
	memcpy(buff, di, dilen);
}



static int sc_hsm_C_Sign(struct p11Object_t *object, CK_MECHANISM_TYPE mech, CK_BYTE_PTR pData, CK_ULONG ulDataLen, CK_BYTE_PTR pSignature, CK_ULONG_PTR pulSignatureLen)
{
	int rc, algo, len;
	unsigned short SW1SW2;
	unsigned char scr[256];
	FUNC_CALLED();

	if (pSignature == NULL) {
		*pulSignatureLen = getSignatureSize(mech, object);
		FUNC_RETURNS(CKR_OK);
	}

	algo = getAlgorithmIdForSigning(mech);
	if (algo < 0) {
		FUNC_FAILS(CKR_MECHANISM_INVALID, "Mechanism not supported");
	}

	if ((algo == ALGO_EC_RAW) || (algo == ALGO_EC_SHA1)) {
		rc = transmitAPDU(object->token->slot, 0x80, 0x68, (unsigned char)object->tokenid, (unsigned char)algo,
				ulDataLen, pData,
				0, scr, sizeof(scr), &SW1SW2);
	} else {
		if (mech == CKM_RSA_PKCS) {
			len = getSignatureSize(mech, object);
			if (len > sizeof(scr)) {
				FUNC_FAILS(CKR_BUFFER_TOO_SMALL, "Signature length is larger than buffer");
			}
			applyPKCSPadding(pData, ulDataLen, scr, len);
			rc = transmitAPDU(object->token->slot, 0x80, 0x68, (unsigned char)object->tokenid, (unsigned char)algo,
				len, scr,
				0, pSignature, *pulSignatureLen, &SW1SW2);
		} else {
			rc = transmitAPDU(object->token->slot, 0x80, 0x68, (unsigned char)object->tokenid, (unsigned char)algo,
				ulDataLen, pData,
				0, pSignature, *pulSignatureLen, &SW1SW2);
		}
	}

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		FUNC_FAILS(-1, "Signature operation failed");
	}

	if ((algo == ALGO_EC_RAW) || (algo == ALGO_EC_SHA1)) {
		rc = decodeECDSASignature(scr, rc, pSignature, *pulSignatureLen);
		if (rc < 0) {
			FUNC_FAILS(CKR_BUFFER_TOO_SMALL, "supplied buffer too small");
		}
	}

	*pulSignatureLen = rc;
	FUNC_RETURNS(CKR_OK);
}



static int sc_hsm_C_DecryptInit(struct p11Object_t *object, CK_MECHANISM_PTR mech)
{
	int algo;

	FUNC_CALLED();

	algo = getAlgorithmIdForSigning(mech->mechanism);
	if (algo < 0) {
		FUNC_FAILS(CKR_MECHANISM_INVALID, "Mechanism not supported");
	}

	FUNC_RETURNS(CKR_OK);
}



static int stripPKCS15Padding(unsigned char *scr, int len, CK_BYTE_PTR pData, CK_ULONG_PTR pulDataLen)
{
	int c1,c2,c3;

	c1 = *scr++ == 0x00;
	c2 = *scr++ == 0x02;
	len -= 2;
	while ((len > 0) && *scr) {
		scr++;
		len--;
	}
	c3 = len > 0;

	if (!(c1 && c2 && c3)) {
		return CKR_ENCRYPTED_DATA_INVALID;
	}

	scr++;
	len--;

	if (len > *pulDataLen) {
		return CKR_BUFFER_TOO_SMALL;
	}

	memcpy(pData, scr, len);
	*pulDataLen = len;

	return CKR_OK;
}



static int sc_hsm_C_Decrypt(struct p11Object_t *object, CK_MECHANISM_TYPE mech, CK_BYTE_PTR pEncryptedData, CK_ULONG ulEncryptedDataLen, CK_BYTE_PTR pData, CK_ULONG_PTR pulDataLen)
{
	int rc, algo;
	unsigned short SW1SW2;
	unsigned char scr[256];

	FUNC_CALLED();

	if (pData == NULL) {
		*pulDataLen = object->keysize >> 3;
		FUNC_RETURNS(CKR_OK);
	}

	algo = getAlgorithmIdForDecryption(mech);
	if (algo < 0) {
		FUNC_FAILS(CKR_MECHANISM_INVALID, "Mechanism not supported");
	}

	rc = transmitAPDU(object->token->slot, 0x80, 0x62, (unsigned char)object->tokenid, (unsigned char)algo,
			ulEncryptedDataLen, pEncryptedData,
			0, scr, sizeof(scr), &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		FUNC_FAILS(CKR_ENCRYPTED_DATA_INVALID, "Decryption operation failed");
	}

	if (mech == CKM_RSA_X_509) {
		if (rc > *pulDataLen) {
			FUNC_FAILS(CKR_BUFFER_TOO_SMALL, "supplied buffer too small");
		}
		*pulDataLen = rc;
		memcpy(pData, scr, rc);
	} else {
		rc = stripPKCS15Padding(scr, rc, pData, pulDataLen);
		if (rc < 0) {
			FUNC_FAILS(CKR_ENCRYPTED_DATA_INVALID, "Invalid PKCS#1 padding");
		}
	}

	FUNC_RETURNS(CKR_OK);
}



static int addPrivateKeyObject(struct p11Token_t *token, unsigned char id)
{
	CK_OBJECT_CLASS class = CKO_PRIVATE_KEY;
	CK_KEY_TYPE keyType = CKK_RSA;
	CK_UTF8CHAR label[10];
	CK_MECHANISM_TYPE genMechType = CKM_RSA_PKCS_KEY_PAIR_GEN;
	CK_BBOOL true = CK_TRUE;
	CK_BBOOL false = CK_FALSE;
	CK_ATTRIBUTE template[] = {
			{ CKA_CLASS, &class, sizeof(class) },
			{ CKA_KEY_TYPE, &keyType, sizeof(keyType) },
			{ CKA_TOKEN, &true, sizeof(true) },
			{ CKA_PRIVATE, &true, sizeof(true) },
			{ CKA_LABEL, label, sizeof(label) - 1 },
			{ CKA_ID, &id, sizeof(id) },
			{ CKA_LOCAL, &true, sizeof(true) },
			{ CKA_KEY_GEN_MECHANISM, &genMechType, sizeof(genMechType) },
			{ CKA_SENSITIVE, &true, sizeof(true) },
			{ CKA_DECRYPT, &true, sizeof(true) },
			{ CKA_SIGN, &true, sizeof(true) },
			{ CKA_SIGN_RECOVER, &true, sizeof(true) },
			{ CKA_UNWRAP, &false, sizeof(false) },
			{ CKA_EXTRACTABLE, &false, sizeof(false) },
			{ CKA_ALWAYS_SENSITIVE, &true, sizeof(true) },
			{ CKA_NEVER_EXTRACTABLE, &true, sizeof(true) },
			{ 0, NULL, 0 },
			{ 0, NULL, 0 }
	};
	token_sc_hsm_t *sc;
	struct p11Object_t *object;
	struct p15PrivateKeyDescription *p15 = NULL;
	unsigned char prkd[MAX_P15_SIZE];
	int rc,attributes;

	FUNC_CALLED();

	rc = readEF(token->slot, (PRKD_PREFIX << 8) | id, prkd, sizeof(prkd));

	if (rc < 0) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "Error reading private key description");
	}

	rc = decodePrivateKeyDescription(prkd, rc, &p15);

	if (rc < 0) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "Error decoding private key description");
	}

	object = calloc(sizeof(struct p11Object_t), 1);

	if (object == NULL) {
		freePrivateKeyDescription(&p15);
		FUNC_FAILS(CKR_HOST_MEMORY, "Out of memory");
	}

	if (p15->coa.label) {
		template[4].pValue = p15->coa.label;
	} else {
		sprintf((char *)label, "Key#%d", id);
	}
	template[4].ulValueLen = strlen(template[4].pValue);

	if (p15->id) {
		template[5].pValue = p15->id;
		template[5].ulValueLen = p15->idlen;

		template[9].pValue = p15->usage & P15_DECIPHER ? &true : &false;
		template[10].pValue = p15->usage & P15_SIGN ? &true : &false;
		template[11].pValue = p15->usage & P15_SIGNRECOVER ? &true : &false;
	}

	attributes = sizeof(template) / sizeof(CK_ATTRIBUTE) - 2;

	switch(p15->keytype) {
	case P15_KEYTYPE_RSA:
		keyType = CKK_RSA;
		sc = getPrivateData(token);
		if (sc->publickeys[id]) {
			decodeModulusExponentFromSPKI(sc->publickeys[id], &template[attributes], &template[attributes + 1]);
			attributes += 2;
		}
		break;
	case P15_KEYTYPE_ECC:
		keyType = CKK_ECDSA;
		sc = getPrivateData(token);
		if (sc->publickeys[id]) {
			decodeECParamsFromSPKI(sc->publickeys[id], &template[attributes]);
			attributes += 1;
		}
		break;
	default:
		freePrivateKeyDescription(&p15);
		free(object);
		FUNC_FAILS(CKR_DEVICE_ERROR, "Unknown key type in PRKD");
	}

	// ToDo: Set CKA_EXTRACTABLE based on KCV

	rc = createPrivateKeyObject(template, attributes, object);

	if (rc != CKR_OK) {
		freePrivateKeyDescription(&p15);
		free(object);
		FUNC_FAILS(rc, "Could not create private key object");
	}

	object->C_SignInit = sc_hsm_C_SignInit;
	object->C_Sign = sc_hsm_C_Sign;
	object->C_DecryptInit = sc_hsm_C_DecryptInit;
	object->C_Decrypt = sc_hsm_C_Decrypt;

	object->tokenid = (int)id;
	object->keysize = p15->keysize;
	addTokenObject(token, object, FALSE);

	freePrivateKeyDescription(&p15);
	FUNC_RETURNS(CKR_OK);
}



static int sc_hsm_loadObjects(struct p11Token_t *token, int publicObjects)
{
	unsigned char filelist[MAX_FILES * 2];
	struct p11Slot_t *slot = token->slot;
	int rc,listlen,i,id,prefix;

	FUNC_CALLED();

	rc = enumerateObjects(slot, filelist, sizeof(filelist));
	if (rc < 0) {
		FUNC_FAILS(rc, "enumerateObjects failed");
	}

	listlen = rc;
	for (i = 0; i < listlen; i += 2) {
		prefix = filelist[i];
		id = filelist[i + 1];

		if (publicObjects) {
			switch(prefix) {
			case KEY_PREFIX:
				if (id != 0) {				// Skip Device Authentication Key
					rc = addEECertificateObject(token, id);
					if (rc != CKR_OK) {
#ifdef DEBUG
						debug("addCertificateObject failed with rc=%d\n", rc);
#endif
					}
				}
				break;
			}
		} else {
			switch(prefix) {
			case KEY_PREFIX:
				if (id != 0) {				// Skip Device Authentication Key
					rc = addPrivateKeyObject(token, id);
					if (rc != CKR_OK) {
#ifdef DEBUG
						debug("addPrivateKeyObject failed with rc=%d\n", rc);
#endif
					}
				}
				break;
			}
		}
	}
	FUNC_RETURNS(CKR_OK);
}



/**
 * Update internal PIN status based on SW1/SW2 received from token
 */
static int updatePinStatus(struct p11Token_t *token, int pinstatus)
{
	int rc = CKR_OK;

	token->info.flags &= ~(CKF_TOKEN_INITIALIZED | CKF_USER_PIN_INITIALIZED | CKF_USER_PIN_FINAL_TRY | CKF_USER_PIN_LOCKED | CKF_USER_PIN_COUNT_LOW);

	if (pinstatus != 0x6984) {
		token->info.flags |= CKF_TOKEN_INITIALIZED | CKF_USER_PIN_INITIALIZED;
	}

	switch(pinstatus) {
	case 0x9000:
		rc = CKR_OK;
		break;
	case 0x6984:
		rc = CKR_USER_PIN_NOT_INITIALIZED;
		break;
	case 0x6983:
		token->info.flags |= CKF_USER_PIN_LOCKED;
		rc = CKR_PIN_LOCKED;
		break;
	case 0x63C1:
		token->info.flags |= CKF_USER_PIN_FINAL_TRY|CKF_USER_PIN_COUNT_LOW;
		rc = CKR_PIN_INCORRECT;
		break;
	case 0x63C2:
		token->info.flags |= CKF_USER_PIN_COUNT_LOW;
		rc = CKR_PIN_INCORRECT;
		break;
	default:
		rc = CKR_PIN_INCORRECT;
		break;
	}
	return rc;
}



/**
 * Perform PIN verification and make private objects visible
 *
 * @param slot      The slot in which the token is inserted
 * @param userType  One of CKU_SO or CKU_USER
 * @param pin       Pointer to PIN value or NULL is PIN shall be verified using PIN-Pad
 * @param pinLen    The length of the PIN supplied in pin
 * @return          CKR_OK or any other Cryptoki error code
 */
int sc_hsm_login(struct p11Slot_t *slot, int userType, unsigned char *pin, int pinlen)
{
	int rc = CKR_OK;
	unsigned short SW1SW2;
	FUNC_CALLED();

	if (userType == CKU_SO) {
		if (pinlen != 16) {
			FUNC_FAILS(CKR_ARGUMENTS_BAD, "SO-PIN must be 16 characters long");
		}
		// Store SO PIN
	} else {

		if (slot->hasFeatureVerifyPINDirect && !pinlen && !pin) {
#ifdef DEBUG
			debug("Verify PIN using CKF_PROTECTED_AUTHENTICATION_PATH\n");
#endif
			rc = transmitVerifyPinAPDU(slot, 0x00, 0x20, 0x00, 0x81, &SW1SW2, PIN_FORMAT_ASCII, 0x06, 0x0F, 0x00, 0x00);
		} else {
#ifdef DEBUG
			debug("Verify PIN using provided PIN value\n");
#endif
			rc = transmitAPDU(slot, 0x00, 0x20, 0x00, 0x81,
				pinlen, pin,
				0, NULL, 0, &SW1SW2);
		}

		if (rc < 0) {
			FUNC_FAILS(CKR_DEVICE_ERROR, "transmitAPDU failed");
		}

		rc = updatePinStatus(slot->token, SW1SW2);

		if (rc != CKR_OK) {
			FUNC_FAILS(rc, "sc_hsm_login failed");
		}

		rc = sc_hsm_loadObjects(slot->token, FALSE);
	}

	FUNC_RETURNS(rc);
}



/**
 * Reselect applet in order to reset authentication state
 *
 * @param slot      The slot in which the token is inserted
 * @return          CKR_OK or any other Cryptoki error code
 */
int sc_hsm_logout(struct p11Slot_t *slot)
{
	int rc;
	FUNC_CALLED();

	rc = selectApplet(slot);
	if (rc < 0) {
		FUNC_FAILS(CKR_TOKEN_NOT_RECOGNIZED, "applet selection failed");
	}

	rc = checkPINStatus(slot);
	if (rc < 0) {
		FUNC_FAILS(CKR_TOKEN_NOT_RECOGNIZED, "checkPINStatus failed");
	}

	updatePinStatus(slot->token, rc);

	FUNC_RETURNS(CKR_OK);
}



/**
 * Create a new SmartCard-HSM token if token detection and initialization is successful
 *
 * @param slot      The slot in which a token was detected
 * @param token     Pointer to pointer updated with newly created token structure
 * @return          CKR_OK or any other Cryptoki error code
 */
int newSmartCardHSMToken(struct p11Slot_t *slot, struct p11Token_t **pptoken)
{
	struct p11Token_t *token;
//	token_sc_hsm_t *sc;
	int rc, pinstatus;

	FUNC_CALLED();

	rc = checkPINStatus(slot);
	if (rc < 0) {
		FUNC_FAILS(CKR_TOKEN_NOT_RECOGNIZED, "checkPINStatus failed");
	}

	if ((rc != 0x9000) && (rc & 0xFF00) != 0x6300 && (rc & 0xFF00) != 0x6900) {
		rc = selectApplet(slot);
		if (rc < 0) {
			FUNC_FAILS(CKR_TOKEN_NOT_RECOGNIZED, "applet selection failed");
		}

		rc = checkPINStatus(slot);
		if (rc < 0) {
			FUNC_FAILS(CKR_TOKEN_NOT_RECOGNIZED, "checkPINStatus failed");
		}
	}
	pinstatus = rc;

	token = (struct p11Token_t *)calloc(sizeof(struct p11Token_t) + sizeof(token_sc_hsm_t), 1);

	if (token == NULL) {
		FUNC_FAILS(CKR_HOST_MEMORY, "Out of memory");
	}

	token->slot = slot;
	token->nextObjectHandle = 1;
	strbpcpy(token->info.label, "SmartCard-HSM", sizeof(token->info.label));
	strbpcpy(token->info.manufacturerID, "CardContact (www.cardcontact.de)", sizeof(token->info.manufacturerID));
	strbpcpy(token->info.model, "SmartCard-HSM", sizeof(token->info.model));
	token->info.ulFreePrivateMemory = CK_UNAVAILABLE_INFORMATION;
	token->info.ulFreePublicMemory = CK_UNAVAILABLE_INFORMATION;
	token->info.ulMinPinLen = 6;
	token->info.ulMaxPinLen = 16;
	token->info.ulTotalPrivateMemory = CK_UNAVAILABLE_INFORMATION;
	token->info.ulTotalPublicMemory = CK_UNAVAILABLE_INFORMATION;
	token->info.ulMaxSessionCount = CK_EFFECTIVELY_INFINITE;
	token->info.ulMaxRwSessionCount = CK_EFFECTIVELY_INFINITE;
	token->info.ulSessionCount = CK_UNAVAILABLE_INFORMATION;

	token->info.flags = CKF_WRITE_PROTECTED | CKF_LOGIN_REQUIRED;
	token->userType = 0xFF;

	updatePinStatus(token, pinstatus);

//	sc = getPrivateData(token);

	sc_hsm_loadObjects(token, TRUE);

	*pptoken = token;
	return CKR_OK;
}

