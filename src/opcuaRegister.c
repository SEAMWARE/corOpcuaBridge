//
// FILE            opcuaRegister.c
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
// The opcua bridge - the SEAM: what the broker calls, and the one symbol it looks for (bridgeRegister).
// The contract is corBridge's BridgeDriver.h (broker -> plugin) and BridgeBroker.h (plugin -> broker).
//
// A bridge knows nothing about entities. It carries (endpoint, json, time) both ways:
//
//   in   the transport delivers a value on an endpoint -> brokerP->sampleIn() -> the broker finds the
//        endpoint's Channel and writes the value into the Channel's entity attribute
//   out  an attribute a Channel carries is written -> publish(endpoint, json) -> the transport sends it
//
// Which endpoint is which attribute is the broker's: the Channels, from the "ngsild" section of this
// bridge's member of the --bridgeConfig file. The plugin reads the rest of its member ("server").
//
// An endpoint is an OPC UA NodeId; what it carries - a value, or a model - is the node's class
// (opcuaTransport.h). The protocol is in opcuaTransport.c.
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // fopen, fread, fseek, ftell
#include <stdlib.h>                                   // malloc, free
#include <string.h>                                   // strdup

#include "corLog/corLog.h"                            // COR_E, COR_W
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, BridgeChannelKind, BridgeDirection
#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*

#include "opcuaTransport.h"                            // opcuaTransport*



// -----------------------------------------------------------------------------
//
// The broker's side of the seam - kept from init() until close()
//
static const BridgeBroker* brokerP = NULL;



// -----------------------------------------------------------------------------
//
// opcuaReceive - the transport delivered a value: hand it to the broker
//
// On the transport's thread. The broker sets up its own per-request state inside sampleIn and takes
// its own locks - the plugin prepares nothing. "opcua" is this bridge's alias: the broker finds the
// Channel by (alias, endpoint).
//
// BRIDGE_NOT_FOUND is not an error: nothing claims the endpoint. BRIDGE_BAD_INPUT is a payload that is
// not JSON - the broker has logged it and dropped the value.
//
// ⚠ json is a copy the transport made for this call and frees after it - never a string literal, and
// never a buffer it reads again: coraine up to 0.4.0 parses the text IN PLACE, although the contract
// says the plugin's text is left alone.
//
static void opcuaReceive(const char* endpoint, const char* json, int64_t timeNs)
{
  if (brokerP == NULL)
    return;

  int r = brokerP->sampleIn("opcua", endpoint, json, timeNs);

  if ((r != BRIDGE_OK) && (r != BRIDGE_NOT_FOUND))
    COR_W("opcua: the value on '%s' was not taken (%d)", endpoint, r);
}



// -----------------------------------------------------------------------------
//
// opcuaStatus - the transport says whether an endpoint can be carried, and why not: GET /ngsi-ld/v1/channels
// (or /assetChannels) shows it - the server cannot be reached, has no such node, is back
//
static void opcuaStatus(const char* endpoint, bool available, const char* reason)
{
#if BRIDGE_ABI_VERSION >= 12
  if ((brokerP != NULL) && (brokerP->abiVersion >= 12) && (brokerP->channelStatusIn != NULL))
    brokerP->channelStatusIn("opcua", endpoint, (available == true) ? BridgeChannelAvailable : BridgeChannelDormant, reason);
#else
  (void) endpoint;
  (void) available;
  (void) reason;
#endif
}



// -----------------------------------------------------------------------------
//
// configLoad - the whole --bridgeConfig file, NUL-terminated; malloc'd. NULL: not readable
//
// All of it: the file holds every bridge's Channels, and the broker takes up to 4 MiB.
//
static char* configLoad(const char* path)
{
  FILE* fP = fopen(path, "r");
  if (fP == NULL)
    return NULL;

  char* text = NULL;
  long  size = (fseek(fP, 0, SEEK_END) == 0) ? ftell(fP) : -1;

  if ((size >= 0) && (fseek(fP, 0, SEEK_SET) == 0) && ((text = malloc(size + 1)) != NULL))
  {
    if (fread(text, 1, size, fP) == (size_t) size)
      text[size] = 0;
    else
    {
      free(text);
      text = NULL;
    }
  }

  fclose(fP);
  return text;
}



// -----------------------------------------------------------------------------
//
// opcuaInit - bring the transport up
//
// The --bridgeConfig file is one JSON object, one member per bridge alias. The broker reads every
// member's "ngsild" section (the Channels); the rest of the member is the plugin's:
//
//   { "opcua": { "server": { ... }, "ngsild": { "topics": { "<endpoint>": { "entityId": ..., ... } } } } }
//
// Called once, late in the broker's startup: the store and the Channels are up, so a value arriving on
// the first callback has somewhere to land. The broker adds the Channels (channelAddInfo) after this.
//
static int opcuaInit(const char* configFile, const BridgeBroker* _brokerP)
{
  CorAlloc  ka;
  CorJson   cj;
  char      buf[16 * 1024];
  CorNode*  serverP = NULL;
  char*     text    = (configFile != NULL) ? configLoad(configFile) : NULL;

  brokerP = _brokerP;

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "opcua config");
  corJsonCreate(&cj, &ka);

  if (text != NULL)
  {
    CorNode* rootP   = corJsonParse(&cj, text);       // valid JSON: the broker has parsed the same file
    CorNode* bridgeP = (rootP != NULL) ? corTreeLookup(rootP, "opcua") : NULL;

    serverP = (bridgeP != NULL) ? corTreeLookup(bridgeP, "server") : NULL;
  }

  //
  // "server": { "url": "opc.tcp://<host>:<port>", "publishingIntervalMs": 500, "samplingIntervalMs": 250 } -
  // the client connects on its own thread: a server that is not up yet makes the endpoints dormant, it
  // does not hold the broker's startup
  //
  int r = opcuaTransportOpen(serverP, opcuaReceive, opcuaStatus);

  corAllocBufferReset(&ka, false);                    // nothing parsed here is kept
  free(text);                                         // after the tree: its strings point into it
  return r;
}



// -----------------------------------------------------------------------------
//
// opcuaClose - must not return until no thread of the plugin can call sampleIn
//
static void opcuaClose(void)
{
  opcuaTransportClose();
  brokerP = NULL;
}



// -----------------------------------------------------------------------------
//
// channelInfoNone - this bridge takes no channelInfo keys yet: any is refused (*whyP)
//
// channelInfo is how to read or carry an endpoint - JSON text, an array of {"key": "...", "value": "..."},
// or NULL. The broker checks that shape and never a key: a key the plugin does not know is the plugin's to
// refuse. An empty array is no key.
//
static bool channelInfoNone(const char* info, const char** whyP)
{
  if (info == NULL)
    return true;

  const char* p = info;

  while ((*p == ' ') || (*p == '\n') || (*p == '\t'))
    p++;

  if ((p[0] == '[') && (strchr(p + 1, '{') == NULL))
    return true;

  *whyP = "this bridge takes no channelInfo keys";
  return false;
}



// -----------------------------------------------------------------------------
//
// channelRefused - say on the Channel why it is not carried: GET /ngsi-ld/v1/channels shows it
//
// channelStatusIn (ABI 12) - from inside channelAddInfo, before it refuses, or at any time later from a
// thread of the plugin's own: a peer that turns out not to have the endpoint, one that cannot be
// reached (BridgeChannelDormant), one that is back (BridgeChannelAvailable). Built against corBridge
// headers from before ABI 12, or loaded by a broker without it, the reason is in the log only and the
// Channel shows the broker's own words.
//
static void channelRefused(const char* endpoint, const char* why)
{
#if BRIDGE_ABI_VERSION >= 12
  if ((brokerP != NULL) && (brokerP->abiVersion >= 12) && (brokerP->channelStatusIn != NULL))
    brokerP->channelStatusIn("opcua", endpoint, BridgeChannelDormant, why);
#else
  (void) endpoint;
  (void) why;
#endif
}



// -----------------------------------------------------------------------------
//
// opcuaChannelAddInfo - start carrying an endpoint, ABI 9
//
// Once per Channel, after init. For BridgeDirectionIn or Both the plugin subscribes and calls sampleIn
// from then on; for Out, publish() must work. Topics only here: a service or an action is
// BRIDGE_UNSUPPORTED, and the broker marks such a Channel dormant - as it does on BRIDGE_BAD_INPUT, with
// the reason in GET /ngsi-ld/v1/channels.
//
static int opcuaChannelAddInfo(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction, const char* info)
{
  const char* why = NULL;

  if (kind != BridgeChannelTopic)
  {
    COR_E("opcua: '%s': this bridge carries topics only (a Variable's value, an Object's model)", endpoint);
    channelRefused(endpoint, "this bridge carries topics only (a Variable's value, an Object's model)");
    return BRIDGE_UNSUPPORTED;
  }

  if (channelInfoNone(info, &why) == false)
  {
    COR_E("opcua: '%s': %s", endpoint, why);
    channelRefused(endpoint, why);
    return BRIDGE_BAD_INPUT;
  }

  int r = opcuaTransportEndpointAdd(endpoint, direction, &why);

  if (r != BRIDGE_OK)
  {
    COR_E("opcua: '%s': %s", endpoint, (why != NULL) ? why : "the transport would not carry it");
    channelRefused(endpoint, (why != NULL) ? why : "the transport would not carry it");
  }

  return r;
}



// -----------------------------------------------------------------------------
//
// opcuaChannelAdd - a host from before ABI 9, which has no channelInfo
//
static int opcuaChannelAdd(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction)
{
  return opcuaChannelAddInfo(endpoint, kind, direction, NULL);
}



// -----------------------------------------------------------------------------
//
// opcuaChannelDel - stop carrying an endpoint
//
static int opcuaChannelDel(const char* endpoint)
{
  return opcuaTransportEndpointDel(endpoint);
}



// -----------------------------------------------------------------------------
//
// opcuaPublish - an attribute a Channel carries was written: send the value out
//
// On a BROKER thread, inside the request that wrote the attribute - hand the value to the transport and
// return. json is borrowed: copy what is needed after the call. A value that arrived through sampleIn
// is never published back out: the broker sees to that.
//
static int opcuaPublish(const char* endpoint, const char* json)
{
  return opcuaTransportSend(endpoint, json);
}



// -----------------------------------------------------------------------------
//
// opcuaVersionInfo - one line for GET /version: the plugin, and what it links
//
static const char* opcuaVersionInfo(void)
{
  static char line[96];

  snprintf(line, sizeof(line), "opcua 0.1.0 (%s)", opcuaTransportVersion());
  return line;
}



// -----------------------------------------------------------------------------
//
// bridgeRegister - the one symbol the broker looks for, and the one this .so exports
//
// Everything else is hidden (-fvisibility=hidden in the makefile). A plugin written in C++ declares
// it extern "C", or the broker's dlsym does not find it.
//
// The broker zeroes the struct, writes ITS BRIDGE_ABI_VERSION into abiVersion, and calls this. The struct
// is the broker's, allocated at the size of ITS header: a slot newer than the broker is not there, so a
// slot is filled only when the broker's abiVersion has it. A slot left NULL means "not supported".
// abiVersion is then overwritten with this plugin's - the broker logs a mismatch and shows it in
// GET /version.
//
// Not filled here, all optional: args (command-line options of the plugin's own), the service and
// action entry points (ABI 2-4), notifySchemes + notify (ABI 10, notifications to a URI scheme of the
// transport - corMqttBridge), serviceSchemes + serviceExecute + serviceCancel (ABI 11, Service
// Execution). See BridgeDriver.h.
//
__attribute__((visibility("default"))) void bridgeRegister(BridgeDriver* driverP)
{
  if (driverP->abiVersion < 1)
    driverP->abiVersion = 1;                          // a host from before the handshake

  driverP->alias       = "opcua";                      // --bridges opcua, and its member of --bridgeConfig
  driverP->version     = "0.1.0";
  driverP->args        = NULL;
  driverP->init        = opcuaInit;
  driverP->close       = opcuaClose;
  driverP->versionInfo = opcuaVersionInfo;
  driverP->channelAdd  = opcuaChannelAdd;
  driverP->channelDel  = opcuaChannelDel;
  driverP->publish     = opcuaPublish;

  if (driverP->abiVersion >= 9)
    driverP->channelAddInfo = opcuaChannelAddInfo;

  driverP->abiVersion  = BRIDGE_ABI_VERSION;
}
