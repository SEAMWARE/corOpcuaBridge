//
// FILE            opcuaSecurity.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// The client's SecureChannel and user - see opcuaSecurity.h.
//
#include <stdio.h>                                    // fopen, fread, fseek, ftell
#include <stdlib.h>                                   // malloc, free
#include <string.h>                                   // strcmp, strlen, strncmp

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/plugin/pki_default.h>

#include "corLog/corLog.h"                            // COR_E, COR_W
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "opcuaSecurity.h"                            // Own interface



// -----------------------------------------------------------------------------
//
// fileLoad - a file as a UA_ByteString; a PEM file (text) with its terminating NUL counted, as mbedTLS
// parses PEM. false: not readable
//
static bool fileLoad(const char* path, UA_ByteString* bsP)
{
  UA_ByteString_init(bsP);

  FILE* fP = fopen(path, "rb");

  if (fP == NULL)
    return false;

  long size = (fseek(fP, 0, SEEK_END) == 0) ? ftell(fP) : -1;

  if ((size <= 0) || (fseek(fP, 0, SEEK_SET) != 0))
  {
    fclose(fP);
    return false;
  }

  bsP->data = (UA_Byte*) UA_malloc(size + 1);

  if ((bsP->data == NULL) || (fread(bsP->data, 1, size, fP) != (size_t) size))
  {
    UA_free(bsP->data);
    bsP->data = NULL;
    fclose(fP);
    return false;
  }

  fclose(fP);

  bsP->data[size] = 0;
  bsP->length     = (strncmp((const char*) bsP->data, "-----BEGIN", 10) == 0) ? (size_t) size + 1 : (size_t) size;

  return true;
}



// -----------------------------------------------------------------------------
//
// str - a string member, or NULL; *badP when it is there and is not a non-empty string
//
static const char* str(CorNode* objectP, const char* name, bool* badP)
{
  CorNode* memberP = (objectP != NULL) ? corTreeLookup(objectP, name) : NULL;

  if (memberP == NULL)
    return NULL;

  if ((memberP->type != CorString) || (memberP->value.s[0] == 0))
  {
    COR_E("opcua: server.%s is not a non-empty string", name);
    *badP = true;
    return NULL;
  }

  return memberP->value.s;
}



// -----------------------------------------------------------------------------
//
// userConfigure - "user": a username and a password (or the file holding it); none: anonymous
//
static bool userConfigure(CorNode* userP, UA_ClientConfig* configP, bool secureChannel)
{
  if (userP == NULL)
    return true;

  if (userP->type != CorObject)
  {
    COR_E("opcua: server.user is an object: { \"username\", \"password\" | \"passwordFile\" }");
    return false;
  }

  bool        bad          = false;
  const char* username     = str(userP, "username", &bad);
  const char* password     = str(userP, "password", &bad);
  const char* passwordFile = str(userP, "passwordFile", &bad);

  if ((bad == true) || (username == NULL) || ((password == NULL) == (passwordFile == NULL)))
  {
    COR_E("opcua: server.user has a 'username' and one of 'password' and 'passwordFile'");
    return false;
  }

  char* fromFile = NULL;

  if (passwordFile != NULL)
  {
    UA_ByteString bs;

    if (fileLoad(passwordFile, &bs) == false)
    {
      COR_E("opcua: server.user.passwordFile '%s' cannot be read", passwordFile);
      return false;
    }

    fromFile = (char*) malloc(bs.length + 1);

    if (fromFile == NULL)
    {
      UA_ByteString_clear(&bs);
      return false;
    }

    memcpy(fromFile, bs.data, bs.length);
    fromFile[bs.length] = 0;
    UA_ByteString_clear(&bs);

    size_t len = strlen(fromFile);

    while ((len > 0) && ((fromFile[len - 1] == '\n') || (fromFile[len - 1] == '\r')))
      fromFile[--len] = 0;                            // the file's line end is not the password's

    password = fromFile;
  }

  if (secureChannel == false)
    COR_W("opcua: a username and password on a SecureChannel without security - the password is protected only if the server's user token policy encrypts it");

  UA_StatusCode sc = UA_ClientConfig_setAuthenticationUsername(configP, username, password);

  if (fromFile != NULL)
  {
    memset(fromFile, 0, strlen(fromFile));
    free(fromFile);
  }

  if (sc != UA_STATUSCODE_GOOD)
  {
    COR_E("opcua: the user cannot be configured (%s)", UA_StatusCode_name(sc));
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// opcuaSecurityConfigure -
//
bool opcuaSecurityConfigure(CorNode* serverP, UA_ClientConfig* configP)
{
  CorNode* securityP = (serverP != NULL) ? corTreeLookup(serverP, "security") : NULL;
  CorNode* userP     = (serverP != NULL) ? corTreeLookup(serverP, "user")     : NULL;

  if ((securityP != NULL) && (securityP->type != CorObject))
  {
    COR_E("opcua: server.security is an object");
    return false;
  }

  bool        bad    = false;
  const char* mode   = str(securityP, "mode", &bad);
  const char* policy = str(securityP, "policy", &bad);

  if (bad == true)
    return false;

  UA_MessageSecurityMode securityMode;

  if ((mode == NULL) || (strcmp(mode, "None") == 0))
    securityMode = UA_MESSAGESECURITYMODE_NONE;
  else if (strcmp(mode, "Sign") == 0)
    securityMode = UA_MESSAGESECURITYMODE_SIGN;
  else if (strcmp(mode, "SignAndEncrypt") == 0)
    securityMode = UA_MESSAGESECURITYMODE_SIGNANDENCRYPT;
  else
  {
    COR_E("opcua: server.security.mode is None, Sign or SignAndEncrypt - not '%s'", mode);
    return false;
  }

  //
  // No security: the defaults, and the None policy only
  //
  if (securityMode == UA_MESSAGESECURITYMODE_NONE)
  {
    if (UA_ClientConfig_setDefault(configP) != UA_STATUSCODE_GOOD)
      return false;

    configP->securityMode      = UA_MESSAGESECURITYMODE_NONE;
    configP->securityPolicyUri = UA_STRING_ALLOC("http://opcfoundation.org/UA/SecurityPolicy#None");

    return userConfigure(userP, configP, false);
  }

  if (policy == NULL)
    policy = "Basic256Sha256";

  if ((strcmp(policy, "Basic256Sha256") != 0) && (strcmp(policy, "Aes128_Sha256_RsaOaep") != 0) && (strcmp(policy, "Aes256_Sha256_RsaPss") != 0))
  {
    COR_E("opcua: server.security.policy is Basic256Sha256 (the default), Aes128_Sha256_RsaOaep or Aes256_Sha256_RsaPss - not '%s'", policy);
    return false;
  }

  const char* certificate    = str(securityP, "certificate", &bad);
  const char* privateKey     = str(securityP, "privateKey", &bad);
  const char* applicationUri = str(securityP, "applicationUri", &bad);
  CorNode*    trustListP     = corTreeLookup(securityP, "trustList");
  CorNode*    trustAnyP      = corTreeLookup(securityP, "trustAny");
  bool        trustAny       = (trustAnyP != NULL) && (trustAnyP->type == CorBoolean) && (trustAnyP->value.b == true);

  if ((bad == true) || (certificate == NULL) || (privateKey == NULL) || (applicationUri == NULL))
  {
    COR_E("opcua: server.security mode %s needs the client's 'certificate', its 'privateKey' and the 'applicationUri' the certificate names", mode);
    return false;
  }

  if ((trustListP != NULL) && (trustListP->type != CorArray))
  {
    COR_E("opcua: server.security.trustList is an array of certificate files");
    return false;
  }

  int trusted = 0;

  for (CorNode* itemP = (trustListP != NULL) ? trustListP->value.head : NULL; itemP != NULL; itemP = itemP->next)
    trusted++;

  if ((trusted == 0) && (trustAny == false))
  {
    COR_E("opcua: server.security.trustList is empty - name the server certificates to trust, or say \"trustAny\": true to accept any (and be open to a server that is not the one meant)");
    return false;
  }

  //
  // The files
  //
  UA_ByteString  cert;
  UA_ByteString  key;
  UA_ByteString* trustV = (UA_ByteString*) calloc(trusted + 1, sizeof(UA_ByteString));
  int            ix     = 0;
  bool           ok     = (trustV != NULL);

  UA_ByteString_init(&cert);
  UA_ByteString_init(&key);

  if ((ok == true) && (fileLoad(certificate, &cert) == false))
  {
    COR_E("opcua: server.security.certificate '%s' cannot be read", certificate);
    ok = false;
  }

  if ((ok == true) && (fileLoad(privateKey, &key) == false))
  {
    COR_E("opcua: server.security.privateKey '%s' cannot be read", privateKey);
    ok = false;
  }

  for (CorNode* itemP = (ok == true) && (trustListP != NULL) ? trustListP->value.head : NULL; (itemP != NULL) && (ok == true); itemP = itemP->next)
  {
    if ((itemP->type != CorString) || (fileLoad(itemP->value.s, &trustV[ix]) == false))
    {
      COR_E("opcua: server.security.trustList: '%s' cannot be read", (itemP->type == CorString) ? itemP->value.s : "(not a string)");
      ok = false;
    }
    else
      ix++;
  }

  UA_StatusCode sc = UA_STATUSCODE_GOOD;

  if (ok == true)
  {
    sc = UA_ClientConfig_setDefaultEncryption(configP, cert, key, trustV, ix, NULL, 0);

    if (sc != UA_STATUSCODE_GOOD)
    {
      COR_E("opcua: the client's certificate and key cannot be used (%s)", UA_StatusCode_name(sc));
      ok = false;
    }
    else
    {
      //
      // After the defaults, which set open62541's own: the URI the certificate names, or the server refuses
      //
      UA_String_clear(&configP->clientDescription.applicationUri);
      configP->clientDescription.applicationUri = UA_STRING_ALLOC(applicationUri);
    }
  }

  UA_ByteString_clear(&cert);
  UA_ByteString_clear(&key);

  for (int jx = 0; jx < ix; jx++)
    UA_ByteString_clear(&trustV[jx]);

  free(trustV);

  if (ok == false)
    return false;

  if (trustAny == true)
  {
    COR_W("opcua: server.security.trustAny - ANY server certificate is accepted: the bridge cannot tell the server it means from one that pretends to be it");

    if (configP->certificateVerification.clear != NULL)
      configP->certificateVerification.clear(&configP->certificateVerification);

    UA_CertificateVerification_AcceptAll(&configP->certificateVerification);
  }

  //
  // Only this mode and this policy: no fallback to a weaker endpoint the server also offers
  //
  char uri[128];

  snprintf(uri, sizeof(uri), "http://opcfoundation.org/UA/SecurityPolicy#%s", policy);

  configP->securityMode      = securityMode;
  configP->securityPolicyUri = UA_STRING_ALLOC(uri);

  return userConfigure(userP, configP, true);
}
