#ifndef COROPCUABRIDGE_OPCUATRANSPORT_H_
#define COROPCUABRIDGE_OPCUATRANSPORT_H_

//
// FILE            opcuaTransport.h
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
// The TRANSPORT - OPC UA, as a client of one server (open62541). opcuaRegister.c is the seam to the broker;
// nothing in this interface knows about the broker, entities or Channels: an endpoint, a JSON text and a
// time, both ways.
//
// An endpoint is a NodeId, as OPC UA writes it ("ns=2;s=Press17", "ns=1;i=1001"), and what it carries is
// decided by the node's class:
//
//   a Variable  its value: in, each change the server reports (a monitored item), its SourceTimestamp the
//               time; out, a value published is written to it (Write), converted to the node's DataType
//   an Object   a model (a coraine AssetChannel): the node and what it contains, browsed, handed over as the
//               neutral node tree { "model": <node> }, then { "values": [ ... ] } as its variables change
//
// One thread owns the client: it connects (and reconnects), browses, subscribes, reads and writes. The
// broker's calls are queued to it - none of them waits on the server.
//
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // int64_t

#include "corTree/CorNode.h"                          // CorNode
#include "corBridge/BridgeDriver.h"                   // BridgeDirection



// -----------------------------------------------------------------------------
//
// OpcuaReceiveFunc - what the transport calls when a value or a model arrives on an endpoint
//
// Called on the TRANSPORT's own thread, never on a broker thread. json is the transport's: borrowed for
// the duration of the call.
//
// @param timeNs  when the value was produced, nanoseconds since the epoch (a Variable's SourceTimestamp) -
//                0 for a model: its nodes carry their own times
//
typedef void (*OpcuaReceiveFunc)(const char* endpoint, const char* json, int64_t timeNs);



// -----------------------------------------------------------------------------
//
// OpcuaStatusFunc - what the transport calls when an endpoint can or cannot be carried, and why
//
// From the transport's thread: the server cannot be reached (available false), is back (true), has no
// such node (false).
//
typedef void (*OpcuaStatusFunc)(const char* endpoint, bool available, const char* reason);



// -----------------------------------------------------------------------------
//
// OpcuaCallFunc - what the transport calls when a method's Call has ended
//
// From the transport's thread. ok: the method's output (an object, each output argument by its name; NULL:
// the method has none); not ok: why, one line for a person. Borrowed for the duration of the call.
//
typedef void (*OpcuaCallFunc)(const char* executionId, bool ok, const char* outputJson, const char* why);



// -----------------------------------------------------------------------------
//
// opcuaTransportOpen - bring the transport up
//
// Starts the client's thread, which connects without the broker waiting for it: a server that is not up
// yet makes the endpoints dormant, with the reason, until it is.
//
// @param serverP    the "server" object of this bridge's member of the --bridgeConfig file:
//                   { "url": "opc.tcp://host:4840", "publishingIntervalMs": 500, "samplingIntervalMs": 250 }.
//                   Read during the call and not kept.
// @param receiveF   where arriving values and models go
// @param statusF    where an endpoint's status goes
//
// @return BRIDGE_OK, or BRIDGE_ERR (no url) with the reason logged
//
extern int opcuaTransportOpen(CorNode* serverP, OpcuaReceiveFunc receiveF, OpcuaStatusFunc statusF);



// -----------------------------------------------------------------------------
//
// opcuaTransportClose - take it down; returns once no thread of the transport can call receiveF
//
extern void opcuaTransportClose(void);



// -----------------------------------------------------------------------------
//
// opcuaTransportEndpointAdd - start carrying an endpoint (queued to the client's thread)
//
// @param whyP  on a refusal, why - one line for a person, static text
//
// @return BRIDGE_OK, BRIDGE_BAD_INPUT (not a NodeId) or BRIDGE_ERR
//
extern int opcuaTransportEndpointAdd(const char* endpoint, BridgeDirection direction, const char** whyP);



// -----------------------------------------------------------------------------
//
// opcuaTransportEndpointDel - stop carrying an endpoint; BRIDGE_OK or BRIDGE_NOT_FOUND
//
extern int opcuaTransportEndpointDel(const char* endpoint);



// -----------------------------------------------------------------------------
//
// opcuaTransportSend - write a value to a Variable endpoint, or to a Variable of a model that writes back
//
// Called on a BROKER thread, inside the request that wrote the attribute: the value is converted to the
// node's DataType here, and the Write queued to the client's thread. A model's Variable (an AssetChannel
// with channelDirection both: the broker publishes on the node an attribute was made from) is known once
// the model has been browsed - its DataType and AccessLevel read with it.
//
// @return BRIDGE_OK (queued), BRIDGE_BAD_INPUT (the value does not fit the node's DataType, or the
//         endpoint is no Variable), BRIDGE_UNSUPPORTED (a model's Variable the server does not let be
//         written), BRIDGE_ERR (the endpoint is not ready: not connected, not read yet),
//         BRIDGE_NOT_FOUND (not an endpoint of this bridge, nor a Variable of a model that writes back)
//
extern int opcuaTransportSend(const char* endpoint, const char* json);



// -----------------------------------------------------------------------------
//
// opcuaTransportCall - call a method: a Service Execution whose registration's endpoint is
// opcua://<object NodeId>/<method NodeId>, each NodeId as OPC UA writes it ('%' escapes a '/' in it)
//
// Called on a BROKER thread: the URL is read here, the Call queued to the client's thread, which reads the
// method's InputArguments and OutputArguments, converts the execution input (an object, a member per input
// argument by its name - a scalar, or an array for an argument of valueRank 1 or more - as each one's
// DataType; "id" and "type" are the entity's, as the broker sends them) and sends the Call. Its end reaches the OpcuaCallFunc, never from inside this call. A Call
// queued while the server cannot be reached fails at once - it is not run once the server is back.
//
// @return BRIDGE_OK (queued), BRIDGE_NOT_FOUND (not an opcua:// URL of an object and a method), BRIDGE_ERR
//
extern int opcuaTransportCall(const char* url, const char* executionId, const char* inputJson);



// -----------------------------------------------------------------------------
//
// opcuaTransportCallFunc - where the end of a Call goes; set before the first opcuaTransportCall
//
extern void opcuaTransportCallFunc(OpcuaCallFunc callF);



// -----------------------------------------------------------------------------
//
// opcuaTransportInventory - the server's address space, for a mapping tool: the node tree from 'from' (a NodeId;
// NULL: the Objects folder), 'depth' levels down (at most 5), as AssetChannel.h's neutral node tree with, for a
// Variable, its "dataType" and whether it is "writable", and "hasChildren" where the depth ended. Waits for the
// client's thread up to timeoutMs. *jsonP malloc'd - the caller frees it
//
// @return BRIDGE_OK, BRIDGE_NOT_FOUND (no such node), BRIDGE_ERR (not connected, timed out) - errBuf says why
//
extern int opcuaTransportInventory(const char* from, int depth, int timeoutMs, char** jsonP, char* errBuf, int errBufLen);



// -----------------------------------------------------------------------------
//
// opcuaTransportVersion - "open62541 1.4.11"
//
extern const char* opcuaTransportVersion(void);

#endif  // COROPCUABRIDGE_OPCUATRANSPORT_H_
