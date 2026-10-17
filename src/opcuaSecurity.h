#ifndef COROPCUABRIDGE_OPCUASECURITY_H_
#define COROPCUABRIDGE_OPCUASECURITY_H_

//
// FILE            opcuaSecurity.h
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
// How the client connects: its SecureChannel (security mode, policy, the client's certificate, the server
// certificates it trusts) and its user (anonymous, or a username and password). From "server" in the
// bridge's member of the --bridgeConfig file:
//
//   "server": { "url": "opc.tcp://...",
//               "security": { "mode": "SignAndEncrypt",              // None (the default), Sign, SignAndEncrypt
//                             "policy": "Basic256Sha256",            // the default; Aes128_Sha256_RsaOaep, Aes256_Sha256_RsaPss
//                             "certificate": "/etc/coraine/opcua/client.der",
//                             "privateKey":  "/etc/coraine/opcua/client.key",
//                             "applicationUri": "urn:example:coraine",   // as in the certificate's subjectAltName
//                             "trustList": [ "/etc/coraine/opcua/server.der" ],
//                             "trustAny": false },
//               "user": { "username": "operator", "passwordFile": "/etc/coraine/opcua/password" } }
//
#include <stdbool.h>                                  // bool

#include <open62541/client.h>                         // UA_ClientConfig
#include <open62541/plugin/log.h>                     // UA_Logger

#include "corTree/CorNode.h"                          // CorNode



// -----------------------------------------------------------------------------
//
// opcuaSecurityConfigure - a client configuration, zeroed but for its logger, made: the defaults, and the
// SecureChannel and user the "server" object asks for
//
// A secure mode without a trust list is refused, unless "trustAny": true says to accept any server
// certificate - which is logged as what it is. A password is never logged.
//
// @return false, the reason logged: the configuration asks for something wrong, or a file cannot be read
//
extern bool opcuaSecurityConfigure(CorNode* serverP, UA_ClientConfig* configP);

#endif  // COROPCUABRIDGE_OPCUASECURITY_H_
