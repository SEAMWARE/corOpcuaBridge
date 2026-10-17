//
// FILE            opcuaTransport.c
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
// OPC UA, through open62541's client - see opcuaTransport.h.
//
// One thread (clientThread) owns the UA_Client: every call into open62541 is made on it. The broker's calls
// - an endpoint added, a value written - are put in a queue the thread empties between iterations of the
// client. Nothing a broker thread does waits on the server.
//
#include <math.h>                                     // isfinite
#include <pthread.h>                                  // pthread_*
#include <stdarg.h>                                   // va_list
#include <stdio.h>                                    // snprintf, vsnprintf
#include <stdlib.h>                                   // malloc, realloc, free, strtod
#include <string.h>                                   // strlen, strcmp, strdup, memcpy
#include <time.h>                                     // nanosleep
#include <unistd.h>                                   // usleep

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_subscriptions.h>
#include <open62541/plugin/log.h>

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_V, COR_T
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corBridge/BridgeBroker.h"                   // BRIDGE_*

#include "opcuaTransport.h"                           // Own interface



// -----------------------------------------------------------------------------
//
// Buf - a growing JSON text
//
typedef struct Buf
{
  char*   s;
  size_t  len;
  size_t  size;
  bool    failed;
} Buf;

static void bufAdd(Buf* bP, const char* s, size_t n)
{
  if (bP->failed == true)
    return;

  if (bP->len + n + 1 > bP->size)
  {
    size_t size = (bP->size == 0) ? 1024 : bP->size * 2;

    while (size < bP->len + n + 1)
      size *= 2;

    char* s2 = (char*) realloc(bP->s, size);

    if (s2 == NULL)
    {
      bP->failed = true;
      return;
    }

    bP->s    = s2;
    bP->size = size;
  }

  memcpy(&bP->s[bP->len], s, n);
  bP->len        += n;
  bP->s[bP->len]  = 0;
}

static void bufStr(Buf* bP, const char* s)
{
  bufAdd(bP, s, strlen(s));
}

static void bufPrintf(Buf* bP, const char* fmt, ...)
{
  char    tmp[128];
  va_list args;

  va_start(args, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
  va_end(args);

  if ((n > 0) && (n < (int) sizeof(tmp)))
    bufAdd(bP, tmp, n);
}

//
// bufJsonString - a JSON string, escaped; 'n' bytes of 's' (UA_String is not NUL-terminated)
//
static void bufJsonString(Buf* bP, const char* s, size_t n)
{
  bufAdd(bP, "\"", 1);

  for (size_t ix = 0; ix < n; ix++)
  {
    unsigned char c = (unsigned char) s[ix];

    if      (c == '"')  bufAdd(bP, "\\\"", 2);
    else if (c == '\\') bufAdd(bP, "\\\\", 2);
    else if (c == '\n') bufAdd(bP, "\\n", 2);
    else if (c == '\r') bufAdd(bP, "\\r", 2);
    else if (c == '\t') bufAdd(bP, "\\t", 2);
    else if (c < 0x20)  bufPrintf(bP, "\\u%04x", c);
    else                bufAdd(bP, (const char*) &c, 1);
  }

  bufAdd(bP, "\"", 1);
}

static void bufUaString(Buf* bP, const UA_String* sP)
{
  bufJsonString(bP, (const char*) sP->data, sP->length);
}



// -----------------------------------------------------------------------------
//
// nodeIdText - a NodeId as OPC UA writes it ("ns=2;s=Press17"); malloc'd, NULL on failure
//
static char* nodeIdText(const UA_NodeId* idP)
{
  UA_String out = UA_STRING_NULL;

  if (UA_NodeId_print(idP, &out) != UA_STATUSCODE_GOOD)
    return NULL;

  char* text = (char*) malloc(out.length + 1);

  if (text != NULL)
  {
    memcpy(text, out.data, out.length);
    text[out.length] = 0;
  }

  UA_String_clear(&out);
  return text;
}



// -----------------------------------------------------------------------------
//
// isoTime - a UA_DateTime as ISO 8601, UTC, milliseconds
//
static void isoTime(UA_DateTime t, char* buf, size_t size)   // buf: 64 bytes
{
  UA_DateTimeStruct d = UA_DateTime_toStruct(t);

  snprintf(buf, size, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", d.year, d.month, d.day, d.hour, d.min, d.sec, d.milliSec);
}

static int64_t unixNs(UA_DateTime t)
{
  return (t == 0) ? 0 : (int64_t) (t - UA_DATETIME_UNIX_EPOCH) * 100;
}



// -----------------------------------------------------------------------------
//
// scalarJson - one value of a builtin type as JSON; false: a type without a JSON form here
//
static bool scalarJson(Buf* bP, const UA_DataType* typeP, const void* p)
{
  switch (typeP->typeKind)
  {
  case UA_DATATYPEKIND_BOOLEAN:  bufStr(bP, (*(const UA_Boolean*) p) ? "true" : "false"); return true;
  case UA_DATATYPEKIND_SBYTE:    bufPrintf(bP, "%d",   *(const UA_SByte*)  p);  return true;
  case UA_DATATYPEKIND_BYTE:     bufPrintf(bP, "%u",   *(const UA_Byte*)   p);  return true;
  case UA_DATATYPEKIND_INT16:    bufPrintf(bP, "%d",   *(const UA_Int16*)  p);  return true;
  case UA_DATATYPEKIND_UINT16:   bufPrintf(bP, "%u",   *(const UA_UInt16*) p);  return true;
  case UA_DATATYPEKIND_INT32:    bufPrintf(bP, "%d",   *(const UA_Int32*)  p);  return true;
  case UA_DATATYPEKIND_UINT32:   bufPrintf(bP, "%u",   *(const UA_UInt32*) p);  return true;
  case UA_DATATYPEKIND_INT64:    bufPrintf(bP, "%lld", (long long) *(const UA_Int64*) p);            return true;
  case UA_DATATYPEKIND_UINT64:   bufPrintf(bP, "%llu", (unsigned long long) *(const UA_UInt64*) p);  return true;
  case UA_DATATYPEKIND_STATUSCODE: bufPrintf(bP, "%u", *(const UA_StatusCode*) p); return true;

  case UA_DATATYPEKIND_FLOAT:
  case UA_DATATYPEKIND_DOUBLE:
  {
    double d = (typeP->typeKind == UA_DATATYPEKIND_FLOAT) ? (double) *(const UA_Float*) p : *(const UA_Double*) p;

    if (isfinite(d) == 0)
      bufStr(bP, "null");
    else
      bufPrintf(bP, "%.17g", d);
    return true;
  }

  case UA_DATATYPEKIND_STRING:         bufUaString(bP, (const UA_String*) p); return true;
  case UA_DATATYPEKIND_LOCALIZEDTEXT:  bufUaString(bP, &((const UA_LocalizedText*) p)->text); return true;
  case UA_DATATYPEKIND_QUALIFIEDNAME:  bufUaString(bP, &((const UA_QualifiedName*) p)->name); return true;

  case UA_DATATYPEKIND_DATETIME:
  {
    char iso[64];

    isoTime(*(const UA_DateTime*) p, iso, sizeof(iso));
    bufJsonString(bP, iso, strlen(iso));
    return true;
  }

  case UA_DATATYPEKIND_NODEID:
  {
    char* text = nodeIdText((const UA_NodeId*) p);

    if (text == NULL)
      return false;

    bufJsonString(bP, text, strlen(text));
    free(text);
    return true;
  }

  default:
    return false;
  }
}



// -----------------------------------------------------------------------------
//
// variantJson - a Variant's value as JSON: a scalar, or an array of them; "null" for what has no JSON form
//
static void variantJson(Buf* bP, const UA_Variant* vP)
{
  if ((vP == NULL) || (vP->type == NULL) || (vP->data == NULL))
  {
    bufStr(bP, "null");
    return;
  }

  if (UA_Variant_isScalar(vP) == true)
  {
    size_t before = bP->len;

    if (scalarJson(bP, vP->type, vP->data) == false)
    {
      bP->len = before;
      bufStr(bP, "null");
    }
    return;
  }

  bufStr(bP, "[");

  for (size_t ix = 0; ix < vP->arrayLength; ix++)
  {
    if (ix > 0)
      bufStr(bP, ",");

    size_t before = bP->len;

    if (scalarJson(bP, vP->type, (const char*) vP->data + ix * vP->type->memSize) == false)
    {
      bP->len = before;
      bufStr(bP, "null");
    }
  }

  bufStr(bP, "]");
}



// -----------------------------------------------------------------------------
//
// Endpoint - one endpoint the broker asked for
//
typedef enum EndpointKind
{
  EndpointUnknown  = 0,                               // not read from the server yet
  EndpointVariable = 1,
  EndpointModel    = 2                                // an Object: a model
} EndpointKind;

typedef struct Endpoint
{
  char*               name;                           // as the broker gave it
  UA_NodeId           nodeId;
  EndpointKind        kind;
  bool                ready;                          // read, subscribed - under the client of this connection
  const UA_DataType*  dataType;                       // a Variable's: what a published value is converted to
  char*               lastWritten;                    // the JSON of the last value written: its echo is not a sample
  bool                refused;                        // the server has no such node, or it is no Variable/Object
  struct Endpoint*    next;
} Endpoint;



// -----------------------------------------------------------------------------
//
// ItemContext - what a monitored item belongs to
//
typedef struct ItemContext
{
  char*   endpoint;                                   // the broker's endpoint: a Variable, or a model's root
  char*   nodeId;                                     // the monitored Variable, as text (a model's values name it)
  bool    model;                                      // a variable of a model: { "values": [ ... ] }
  bool    first;                                      // the first notification: the value at creation - skipped for a model (the model has it)
  struct ItemContext* next;
} ItemContext;



// -----------------------------------------------------------------------------
//
// Op - a broker call, queued for the client's thread
//
typedef enum OpType
{
  OpAdd   = 0,
  OpDel   = 1,
  OpWrite = 2
} OpType;

typedef struct Op
{
  OpType      type;
  char*       endpoint;
  UA_Variant  value;                                  // OpWrite
  char*       json;                                   // OpWrite: what the broker sent - for the echo
  struct Op*  next;
} Op;



// -----------------------------------------------------------------------------
//
// The transport's state
//
static char*             serverUrl            = NULL;
static double            publishingIntervalMs = 500;
static double            samplingIntervalMs   = 250;
static OpcuaReceiveFunc  receiveFunc          = NULL;
static OpcuaStatusFunc   statusFunc           = NULL;

static pthread_t         clientThreadId;
static bool              clientThreadRunning  = false;
static volatile bool     stopping             = false;

static pthread_mutex_t   mutex                = PTHREAD_MUTEX_INITIALIZER;   // endpoints and ops
static Endpoint*         endpoints            = NULL;
static Op*               opsHead              = NULL;
static Op*               opsTail              = NULL;

// The client's thread only
static UA_Client*        client               = NULL;
static UA_UInt32         subscriptionId       = 0;
static ItemContext*      items                = NULL;
static bool              connected            = false;
static char              unreachableReason[256];

static UA_Logger         logger;



// -----------------------------------------------------------------------------
//
// uaLog - open62541's log lines into the broker's log: warnings and errors only
//
static void uaLog(void* context, UA_LogLevel level, UA_LogCategory category, const char* msg, va_list args)
{
  (void) context;
  (void) category;

  if (level < UA_LOGLEVEL_WARNING)
    return;

  char line[512];

  vsnprintf(line, sizeof(line), msg, args);

  if (level >= UA_LOGLEVEL_ERROR)
    COR_E("opcua: open62541: %s", line);
  else
    COR_W("opcua: open62541: %s", line);
}



// -----------------------------------------------------------------------------
//
// endpointFind - under mutex
//
static Endpoint* endpointFind(const char* name)
{
  for (Endpoint* eP = endpoints; eP != NULL; eP = eP->next)
  {
    if (strcmp(eP->name, name) == 0)
      return eP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// endpointNames - the endpoints' names, a NULL-terminated copy (malloc'd, each one too)
//
static char** endpointNames(void)
{
  pthread_mutex_lock(&mutex);

  int count = 0;

  for (Endpoint* eP = endpoints; eP != NULL; eP = eP->next)
    count++;

  char** names = (char**) calloc(count + 1, sizeof(char*));
  int    n     = 0;

  for (Endpoint* eP = endpoints; (eP != NULL) && (names != NULL); eP = eP->next)
    names[n++] = strdup(eP->name);

  pthread_mutex_unlock(&mutex);
  return names;
}



// -----------------------------------------------------------------------------
//
// opPush - queue a broker call for the client's thread; takes json and value over
//
static void opPush(OpType type, const char* endpoint, UA_Variant* valueP, char* json)
{
  Op* opP = (Op*) calloc(1, sizeof(Op));

  if (opP == NULL)
    return;

  opP->type     = type;
  opP->endpoint = strdup(endpoint);
  opP->json     = json;

  if (valueP != NULL)
    opP->value = *valueP;                             // the data moves over
  else
    UA_Variant_init(&opP->value);

  pthread_mutex_lock(&mutex);

  if (opsTail == NULL)
    opsHead = opP;
  else
    opsTail->next = opP;

  opsTail = opP;

  pthread_mutex_unlock(&mutex);
}

static void opFree(Op* opP)
{
  free(opP->endpoint);
  free(opP->json);
  UA_Variant_clear(&opP->value);
  free(opP);
}



// -----------------------------------------------------------------------------
//
// itemsClear - the monitored items' contexts (the items themselves die with the subscription)
//
static void itemsClear(void)
{
  while (items != NULL)
  {
    ItemContext* nextP = items->next;

    free(items->endpoint);
    free(items->nodeId);
    free(items);

    items = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// dataChange - a monitored item's value changed (on the client's thread, inside UA_Client_run_iterate)
//
static void dataChange(UA_Client* clientP, UA_UInt32 subId, void* subContext, UA_UInt32 monId, void* monContext, UA_DataValue* valueP)
{
  (void) clientP;
  (void) subId;
  (void) subContext;
  (void) monId;

  ItemContext* icP = (ItemContext*) monContext;

  if ((icP == NULL) || (valueP == NULL) || (valueP->hasValue == false) || (receiveFunc == NULL))
    return;

  bool first = icP->first;

  icP->first = false;

  Buf     b      = { NULL, 0, 0, false };
  int64_t timeNs = (valueP->hasSourceTimestamp == true) ? unixNs(valueP->sourceTimestamp) : 0;

  if (icP->model == true)
  {
    if (first == true)
      return;                                         // the value the model was read with

    char iso[64] = "";

    if (valueP->hasSourceTimestamp == true)
      isoTime(valueP->sourceTimestamp, iso, sizeof(iso));

    bufStr(&b, "{\"values\":[{\"id\":");
    bufJsonString(&b, icP->nodeId, strlen(icP->nodeId));
    bufStr(&b, ",\"value\":");
    variantJson(&b, &valueP->value);

    if (iso[0] != 0)
    {
      bufStr(&b, ",\"sourceTime\":");
      bufJsonString(&b, iso, strlen(iso));
    }

    bufStr(&b, "}]}");
  }
  else
  {
    variantJson(&b, &valueP->value);

    //
    // The echo of the bridge's own Write: the server reports the value it was just given. Not a sample -
    // the broker wrote it
    //
    pthread_mutex_lock(&mutex);

    Endpoint* eP   = endpointFind(icP->endpoint);
    bool      echo = (eP != NULL) && (eP->lastWritten != NULL) && (b.s != NULL) && (strcmp(eP->lastWritten, b.s) == 0);

    if (echo == true)
    {
      free(eP->lastWritten);
      eP->lastWritten = NULL;
    }

    pthread_mutex_unlock(&mutex);

    if (echo == true)
    {
      free(b.s);
      return;
    }
  }

  if ((b.failed == false) && (b.s != NULL))
    receiveFunc(icP->endpoint, b.s, (icP->model == true) ? 0 : timeNs);

  free(b.s);
}



// -----------------------------------------------------------------------------
//
// monitor - a monitored item on a Variable, for an endpoint
//
static bool monitor(const UA_NodeId* nodeIdP, const char* endpoint, const char* nodeId, bool model)
{
  ItemContext* icP = (ItemContext*) calloc(1, sizeof(ItemContext));

  if (icP == NULL)
    return false;

  icP->endpoint = strdup(endpoint);
  icP->nodeId   = strdup(nodeId);
  icP->model    = model;
  icP->first    = true;
  icP->next     = items;
  items         = icP;

  UA_MonitoredItemCreateRequest request = UA_MonitoredItemCreateRequest_default(*nodeIdP);

  request.requestedParameters.samplingInterval = samplingIntervalMs;

  UA_MonitoredItemCreateResult result = UA_Client_MonitoredItems_createDataChange(client, subscriptionId, UA_TIMESTAMPSTORETURN_BOTH,
                                                                                    request, icP, dataChange, NULL);
  bool ok = (result.statusCode == UA_STATUSCODE_GOOD);

  if (ok == false)
    COR_W("opcua: no monitored item on '%s' (%s)", nodeId, UA_StatusCode_name(result.statusCode));

  UA_MonitoredItemCreateResult_clear(&result);
  return ok;
}



// -----------------------------------------------------------------------------
//
// browseAll - every reference of a node, forward; the caller clears
//
static bool browseAll(const UA_NodeId* nodeIdP, UA_BrowseResult* resultP)
{
  UA_BrowseRequest request;
  UA_BrowseRequest_init(&request);

  UA_BrowseDescription bd;
  UA_BrowseDescription_init(&bd);

  bd.nodeId          = *nodeIdP;
  bd.browseDirection = UA_BROWSEDIRECTION_FORWARD;
  bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_REFERENCES);
  bd.includeSubtypes = true;
  bd.nodeClassMask   = 0;                           // every class: a type definition is an ObjectType - the walk picks its children
  bd.resultMask      = UA_BROWSERESULTMASK_ALL;

  request.nodesToBrowse     = &bd;
  request.nodesToBrowseSize = 1;

  UA_BrowseResponse response = UA_Client_Service_browse(client, request);

  UA_BrowseResult_init(resultP);

  if ((response.responseHeader.serviceResult != UA_STATUSCODE_GOOD) || (response.resultsSize != 1))
  {
    UA_BrowseResponse_clear(&response);
    return false;
  }

  UA_BrowseResult_copy(&response.results[0], resultP);
  UA_BrowseResponse_clear(&response);

  //
  // A server that returns references a page at a time: the rest, one page after the other
  //
  while (resultP->continuationPoint.length > 0)
  {
    UA_BrowseNextRequest next;
    UA_BrowseNextRequest_init(&next);

    next.continuationPoints     = &resultP->continuationPoint;
    next.continuationPointsSize = 1;

    UA_BrowseNextResponse nextResponse = UA_Client_Service_browseNext(client, next);

    UA_ByteString_clear(&resultP->continuationPoint);

    if ((nextResponse.responseHeader.serviceResult != UA_STATUSCODE_GOOD) || (nextResponse.resultsSize != 1))
    {
      UA_BrowseNextResponse_clear(&nextResponse);
      break;
    }

    UA_BrowseResult* pageP = &nextResponse.results[0];
    size_t           total = resultP->referencesSize + pageP->referencesSize;
    void*            refs  = UA_realloc(resultP->references, total * sizeof(UA_ReferenceDescription));

    if (refs != NULL)
    {
      resultP->references = (UA_ReferenceDescription*) refs;
      memcpy(&resultP->references[resultP->referencesSize], pageP->references, pageP->referencesSize * sizeof(UA_ReferenceDescription));
      resultP->referencesSize = total;
      UA_free(pageP->references);                     // moved: the references' members now belong to resultP
      pageP->references     = NULL;
      pageP->referencesSize = 0;
    }

    resultP->continuationPoint = pageP->continuationPoint;
    UA_ByteString_init(&pageP->continuationPoint);
    UA_BrowseNextResponse_clear(&nextResponse);
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// browseName - a node's BrowseName, malloc'd text; NULL on failure. A small cache: type and reference-type
// names are asked for again and again
//
typedef struct NameCache
{
  UA_NodeId          id;
  char*              name;
  struct NameCache*  next;
} NameCache;

static NameCache* nameCache = NULL;

static const char* browseName(const UA_NodeId* idP)
{
  for (NameCache* cP = nameCache; cP != NULL; cP = cP->next)
  {
    if (UA_NodeId_equal(&cP->id, idP) == true)
      return cP->name;
  }

  UA_QualifiedName qn;
  UA_QualifiedName_init(&qn);

  if (UA_Client_readBrowseNameAttribute(client, *idP, &qn) != UA_STATUSCODE_GOOD)
    return NULL;

  NameCache* cP = (NameCache*) calloc(1, sizeof(NameCache));

  if (cP == NULL)
  {
    UA_QualifiedName_clear(&qn);
    return NULL;
  }

  cP->name = (char*) malloc(qn.name.length + 1);

  if (cP->name != NULL)
  {
    memcpy(cP->name, qn.name.data, qn.name.length);
    cP->name[qn.name.length] = 0;
  }

  UA_NodeId_copy(idP, &cP->id);
  cP->next  = nameCache;
  nameCache = cP;

  UA_QualifiedName_clear(&qn);
  return cP->name;
}

static void nameCacheClear(void)
{
  while (nameCache != NULL)
  {
    NameCache* nextP = nameCache->next;

    UA_NodeId_clear(&nameCache->id);
    free(nameCache->name);
    free(nameCache);

    nameCache = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// unitCode - a Variable's EngineeringUnits as a UN/CEFACT common code ("BAR", "CEL"); false when it has none
//
// OPC UA encodes the common code in EUInformation.unitId: up to three characters, one per byte (Part 8,
// 5.6.3).
//
static bool unitCode(const UA_BrowseResult* refsP, char* code)
{
  for (size_t ix = 0; ix < refsP->referencesSize; ix++)
  {
    const UA_ReferenceDescription* rP = &refsP->references[ix];

    UA_String engineeringUnits = UA_STRING("EngineeringUnits");

    if ((rP->nodeClass != UA_NODECLASS_VARIABLE) || (UA_String_equal(&rP->browseName.name, &engineeringUnits) == false))
      continue;

    UA_Variant v;
    UA_Variant_init(&v);

    if ((UA_Client_readValueAttribute(client, rP->nodeId.nodeId, &v) != UA_STATUSCODE_GOOD) ||
        (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_EUINFORMATION]) == false))
    {
      UA_Variant_clear(&v);
      return false;
    }

    UA_Int32 id = ((const UA_EUInformation*) v.data)->unitId;
    int      n  = 0;

    UA_Variant_clear(&v);

    if (id <= 0)
      return false;

    for (int shift = 16; shift >= 0; shift -= 8)
    {
      char c = (char) ((id >> shift) & 0xFF);

      if (c != 0)
        code[n++] = c;
    }

    code[n] = 0;
    return n > 0;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// Walk - one model being browsed
//
typedef struct Walk
{
  Buf          b;
  const char*  endpoint;
  UA_NodeId*   visitedV;                              // no node twice: OPC UA's graph has cycles
  int          visited;
  int          visitedMax;
  UA_NodeId*   variableV;                             // the Variables of the model - monitored once it is handed over
  int          variables;
  int          variablesMax;
  int          nodes;
} Walk;

#define WALK_NODES_MAX  10000                         // a model larger than this is cut there, and says so
#define WALK_DEPTH_MAX  16

static bool walkVisited(Walk* wP, const UA_NodeId* idP)
{
  for (int ix = 0; ix < wP->visited; ix++)
  {
    if (UA_NodeId_equal(&wP->visitedV[ix], idP) == true)
      return true;
  }

  if (wP->visited == wP->visitedMax)
  {
    int        max = (wP->visitedMax == 0) ? 256 : wP->visitedMax * 2;
    UA_NodeId* v   = (UA_NodeId*) realloc(wP->visitedV, max * sizeof(UA_NodeId));

    if (v == NULL)
      return true;                                    // out of memory: as if seen - the walk ends sooner

    wP->visitedV   = v;
    wP->visitedMax = max;
  }

  UA_NodeId_copy(idP, &wP->visitedV[wP->visited++]);
  return false;
}

static void walkVariable(Walk* wP, const UA_NodeId* idP)
{
  if (wP->variables == wP->variablesMax)
  {
    int        max = (wP->variablesMax == 0) ? 64 : wP->variablesMax * 2;
    UA_NodeId* v   = (UA_NodeId*) realloc(wP->variableV, max * sizeof(UA_NodeId));

    if (v == NULL)
      return;

    wP->variableV    = v;
    wP->variablesMax = max;
  }

  UA_NodeId_copy(idP, &wP->variableV[wP->variables++]);
}

static void walkFree(Walk* wP)
{
  for (int ix = 0; ix < wP->visited; ix++)
    UA_NodeId_clear(&wP->visitedV[ix]);

  for (int ix = 0; ix < wP->variables; ix++)
    UA_NodeId_clear(&wP->variableV[ix]);

  free(wP->visitedV);
  free(wP->variableV);
  free(wP->b.s);
}



// -----------------------------------------------------------------------------
//
// isChildReference - a reference to what a node contains: HasComponent, HasOrderedComponent, HasProperty,
// Organizes. HasProperty of a Variable is metadata (EngineeringUnits, EURange) - read, not a child.
//
static bool isChildReference(const UA_NodeId* refTypeP)
{
  if (refTypeP->namespaceIndex != 0 || refTypeP->identifierType != UA_NODEIDTYPE_NUMERIC)
    return false;

  UA_UInt32 id = refTypeP->identifier.numeric;

  return (id == UA_NS0ID_HASCOMPONENT) || (id == UA_NS0ID_HASORDEREDCOMPONENT) || (id == UA_NS0ID_HASPROPERTY) || (id == UA_NS0ID_ORGANIZES);
}

//
// isTypeReference - references that say what a node is, not what it relates to: not mapped
//
static bool isTypeReference(const UA_NodeId* refTypeP)
{
  if (refTypeP->namespaceIndex != 0 || refTypeP->identifierType != UA_NODEIDTYPE_NUMERIC)
    return false;

  UA_UInt32 id = refTypeP->identifier.numeric;

  return (id == UA_NS0ID_HASTYPEDEFINITION) || (id == UA_NS0ID_HASMODELLINGRULE) || (id == UA_NS0ID_HASENCODING) ||
         (id == UA_NS0ID_HASSUBTYPE) || (id == UA_NS0ID_GENERATESEVENT) || (id == UA_NS0ID_HASDICTIONARYENTRY) ||
         (id == UA_NS0ID_HASINTERFACE) || (id == UA_NS0ID_HASADDIN);
}



// -----------------------------------------------------------------------------
//
// walkNode - one node, and what it contains, as the neutral node tree (coraine's AssetChannel.h)
//
static void walkNode(Walk* wP, const UA_NodeId* idP, UA_NodeClass nodeClass, const UA_QualifiedName* nameP, const UA_ExpandedNodeId* typeDefP, int depth)
{
  Buf* bP = &wP->b;

  wP->nodes++;

  char* idText = nodeIdText(idP);

  //
  // The kind: an Object (a "folder" when its type is FolderType), a Variable, a Method
  //
  const char* kind = "object";

  if (nodeClass == UA_NODECLASS_VARIABLE)
    kind = "variable";
  else if (nodeClass == UA_NODECLASS_METHOD)
    kind = "method";
  else if ((typeDefP != NULL) && (typeDefP->nodeId.namespaceIndex == 0) && (typeDefP->nodeId.identifierType == UA_NODEIDTYPE_NUMERIC) &&
           (typeDefP->nodeId.identifier.numeric == UA_NS0ID_FOLDERTYPE))
    kind = "folder";

  bufStr(bP, "{\"node\":");
  bufJsonString(bP, kind, strlen(kind));

  if (idText != NULL)
  {
    bufStr(bP, ",\"id\":");
    bufJsonString(bP, idText, strlen(idText));
  }

  bufStr(bP, ",\"name\":");
  bufUaString(bP, &nameP->name);

  if ((typeDefP != NULL) && (UA_NodeId_isNull(&typeDefP->nodeId) == false) && (nodeClass != UA_NODECLASS_METHOD))
  {
    char*       typeText = nodeIdText(&typeDefP->nodeId);
    const char* typeName = browseName(&typeDefP->nodeId);

    if (typeName != NULL)
    {
      bufStr(bP, ",\"typeName\":");
      bufJsonString(bP, typeName, strlen(typeName));
    }

    if (typeText != NULL)
    {
      bufStr(bP, ",\"typeDefinition\":");
      bufJsonString(bP, typeText, strlen(typeText));
      free(typeText);
    }
  }

  UA_BrowseResult refs;
  bool            browsed = (nodeClass != UA_NODECLASS_METHOD) && (browseAll(idP, &refs) == true);

  //
  // Its meaning: HasDictionaryEntry -> semanticIds
  //
  if (browsed == true)
  {
    bool first = true;

    for (size_t ix = 0; ix < refs.referencesSize; ix++)
    {
      const UA_ReferenceDescription* rP = &refs.references[ix];

      if ((rP->referenceTypeId.namespaceIndex != 0) || (rP->referenceTypeId.identifierType != UA_NODEIDTYPE_NUMERIC) ||
          (rP->referenceTypeId.identifier.numeric != UA_NS0ID_HASDICTIONARYENTRY))
        continue;

      //
      // A dictionary entry named by a string (an IRDI, a URI: "0173-1#02-AAO677#002") is that string - its
      // namespace index is this server's own, and means nothing outside it. Any other id as OPC UA writes it
      //
      const UA_NodeId* entryP = &rP->nodeId.nodeId;

      bufStr(bP, first ? ",\"semanticIds\":[" : ",");

      if (entryP->identifierType == UA_NODEIDTYPE_STRING)
        bufUaString(bP, &entryP->identifier.string);
      else
      {
        char* text = nodeIdText(entryP);

        bufJsonString(bP, (text != NULL) ? text : "", (text != NULL) ? strlen(text) : 0);
        free(text);
      }

      first = false;
    }

    if (first == false)
      bufStr(bP, "]");
  }

  //
  // A Variable: its value, unit and time - a leaf. Its properties (EngineeringUnits, ...) are metadata
  //
  if (nodeClass == UA_NODECLASS_VARIABLE)
  {
    UA_ReadValueId rv;
    UA_ReadValueId_init(&rv);
    rv.nodeId      = *idP;
    rv.attributeId = UA_ATTRIBUTEID_VALUE;

    UA_ReadRequest request;
    UA_ReadRequest_init(&request);
    request.nodesToRead        = &rv;
    request.nodesToReadSize    = 1;
    request.timestampsToReturn = UA_TIMESTAMPSTORETURN_SOURCE;

    UA_ReadResponse response = UA_Client_Service_read(client, request);

    bufStr(bP, ",\"value\":");

    if ((response.responseHeader.serviceResult == UA_STATUSCODE_GOOD) && (response.resultsSize == 1) && (response.results[0].hasValue == true))
    {
      variantJson(bP, &response.results[0].value);

      if (response.results[0].hasSourceTimestamp == true)
      {
        char iso[64];

        isoTime(response.results[0].sourceTimestamp, iso, sizeof(iso));
        bufStr(bP, ",\"sourceTime\":");
        bufJsonString(bP, iso, strlen(iso));
      }
    }
    else
      bufStr(bP, "null");

    UA_ReadResponse_clear(&response);

    char code[4];

    if ((browsed == true) && (unitCode(&refs, code) == true))
    {
      bufStr(bP, ",\"unit\":");
      bufJsonString(bP, code, strlen(code));
    }

    walkVariable(wP, idP);
  }
  else if (browsed == true)
  {
    //
    // An Object: what it contains (children), and what it refers to (references)
    //
    bool firstChild = true;
    bool firstRef   = true;

    for (size_t ix = 0; ix < refs.referencesSize; ix++)
    {
      const UA_ReferenceDescription* rP = &refs.references[ix];

      if ((rP->isForward == false) || (rP->nodeId.serverIndex != 0))
        continue;

      bool modelClass = (rP->nodeClass == UA_NODECLASS_OBJECT) || (rP->nodeClass == UA_NODECLASS_VARIABLE) || (rP->nodeClass == UA_NODECLASS_METHOD);

      if ((isChildReference(&rP->referenceTypeId) == true) && (modelClass == true))
      {
        if ((depth >= WALK_DEPTH_MAX) || (wP->nodes >= WALK_NODES_MAX) || (walkVisited(wP, &rP->nodeId.nodeId) == true))
          continue;

        bufStr(bP, firstChild ? ",\"children\":[" : ",");
        walkNode(wP, &rP->nodeId.nodeId, rP->nodeClass, &rP->browseName, &rP->typeDefinition, depth + 1);
        firstChild = false;
      }
    }

    if (firstChild == false)
      bufStr(bP, "]");

    for (size_t ix = 0; ix < refs.referencesSize; ix++)
    {
      const UA_ReferenceDescription* rP = &refs.references[ix];

      if ((rP->isForward == false) || (isChildReference(&rP->referenceTypeId) == true) || (isTypeReference(&rP->referenceTypeId) == true))
        continue;

      const char* refName = browseName(&rP->referenceTypeId);
      char*       target  = nodeIdText(&rP->nodeId.nodeId);

      if ((refName != NULL) && (target != NULL))
      {
        bufStr(bP, firstRef ? ",\"references\":[" : ",");
        bufStr(bP, "{\"type\":");
        bufJsonString(bP, refName, strlen(refName));
        bufStr(bP, ",\"target\":");
        bufJsonString(bP, target, strlen(target));
        bufStr(bP, "}");
        firstRef = false;
      }

      free(target);
    }

    if (firstRef == false)
      bufStr(bP, "]");
  }

  if (browsed == true)
    UA_BrowseResult_clear(&refs);

  bufStr(bP, "}");
  free(idText);
}



// -----------------------------------------------------------------------------
//
// modelSetup - an Object endpoint: its model browsed and handed over, its Variables monitored
//
static bool modelSetup(Endpoint* eP)
{
  UA_QualifiedName  name;
  UA_NodeId         typeDef = UA_NODEID_NULL;
  UA_ExpandedNodeId typeDefX;

  UA_QualifiedName_init(&name);
  UA_ExpandedNodeId_init(&typeDefX);

  if (UA_Client_readBrowseNameAttribute(client, eP->nodeId, &name) != UA_STATUSCODE_GOOD)
    return false;

  //
  // The root's type: its HasTypeDefinition reference
  //
  UA_BrowseResult refs;

  if (browseAll(&eP->nodeId, &refs) == true)
  {
    for (size_t ix = 0; ix < refs.referencesSize; ix++)
    {
      const UA_ReferenceDescription* rP = &refs.references[ix];

      if ((rP->referenceTypeId.namespaceIndex == 0) && (rP->referenceTypeId.identifierType == UA_NODEIDTYPE_NUMERIC) &&
          (rP->referenceTypeId.identifier.numeric == UA_NS0ID_HASTYPEDEFINITION))
      {
        UA_NodeId_copy(&rP->nodeId.nodeId, &typeDef);
        break;
      }
    }

    UA_BrowseResult_clear(&refs);
  }

  typeDefX.nodeId = typeDef;

  Walk w;

  memset(&w, 0, sizeof(w));
  w.endpoint = eP->name;

  walkVisited(&w, &eP->nodeId);
  bufStr(&w.b, "{\"model\":");
  walkNode(&w, &eP->nodeId, UA_NODECLASS_OBJECT, &name, &typeDefX, 0);
  bufStr(&w.b, "}");

  UA_QualifiedName_clear(&name);
  UA_NodeId_clear(&typeDef);

  if (w.nodes >= WALK_NODES_MAX)
    COR_W("opcua: the model under '%s' is larger than %d nodes - cut there", eP->name, WALK_NODES_MAX);

  if ((w.b.failed == true) || (w.b.s == NULL))
  {
    walkFree(&w);
    return false;
  }

  COR_V("opcua: the model under '%s': %d nodes, %d variables", eP->name, w.nodes, w.variables);
  receiveFunc(eP->name, w.b.s, 0);

  for (int ix = 0; ix < w.variables; ix++)
  {
    char* idText = nodeIdText(&w.variableV[ix]);

    if (idText != NULL)
    {
      monitor(&w.variableV[ix], eP->name, idText, true);
      free(idText);
    }
  }

  walkFree(&w);
  return true;
}



// -----------------------------------------------------------------------------
//
// endpointSetup - what the endpoint is, and carrying it: a Variable monitored, an Object's model handed over
//
static void endpointSetup(Endpoint* eP)
{
  UA_NodeClass nodeClass = UA_NODECLASS_UNSPECIFIED;
  UA_StatusCode sc       = UA_Client_readNodeClassAttribute(client, eP->nodeId, &nodeClass);
  char          reason[256];

  if (sc != UA_STATUSCODE_GOOD)
  {
    snprintf(reason, sizeof(reason), "the OPC UA server has no node '%s' (%s)", eP->name, UA_StatusCode_name(sc));
    COR_W("opcua: %s", reason);
    eP->refused = true;

    if (statusFunc != NULL)
      statusFunc(eP->name, false, reason);
    return;
  }

  if (nodeClass == UA_NODECLASS_VARIABLE)
  {
    UA_NodeId dataTypeId = UA_NODEID_NULL;

    if (UA_Client_readDataTypeAttribute(client, eP->nodeId, &dataTypeId) == UA_STATUSCODE_GOOD)
    {
      pthread_mutex_lock(&mutex);
      eP->dataType = UA_findDataType(&dataTypeId);
      eP->kind     = EndpointVariable;
      pthread_mutex_unlock(&mutex);
    }

    UA_NodeId_clear(&dataTypeId);

    if (monitor(&eP->nodeId, eP->name, eP->name, false) == false)
      return;
  }
  else if (nodeClass == UA_NODECLASS_OBJECT)
  {
    pthread_mutex_lock(&mutex);
    eP->kind = EndpointModel;
    pthread_mutex_unlock(&mutex);

    if (modelSetup(eP) == false)
    {
      COR_W("opcua: the model under '%s' could not be read", eP->name);
      return;
    }
  }
  else
  {
    snprintf(reason, sizeof(reason), "'%s' is neither a Variable (a value) nor an Object (a model)", eP->name);
    COR_W("opcua: %s", reason);
    eP->refused = true;

    if (statusFunc != NULL)
      statusFunc(eP->name, false, reason);
    return;
  }

  pthread_mutex_lock(&mutex);
  eP->ready = true;
  pthread_mutex_unlock(&mutex);

  if (statusFunc != NULL)
    statusFunc(eP->name, true, NULL);
}



// -----------------------------------------------------------------------------
//
// opsRun - the queued broker calls, on the client's thread
//
static void opsRun(void)
{
  pthread_mutex_lock(&mutex);
  Op* opP = opsHead;
  opsHead = NULL;
  opsTail = NULL;
  pthread_mutex_unlock(&mutex);

  while (opP != NULL)
  {
    Op* nextP = opP->next;

    pthread_mutex_lock(&mutex);
    Endpoint* eP = endpointFind(opP->endpoint);
    pthread_mutex_unlock(&mutex);

    if ((opP->type == OpAdd) && (eP != NULL) && (connected == true) && (eP->ready == false) && (eP->refused == false))
      endpointSetup(eP);
    else if ((opP->type == OpWrite) && (eP != NULL) && (connected == true))
    {
      UA_StatusCode sc = UA_Client_writeValueAttribute(client, eP->nodeId, &opP->value);

      if (sc != UA_STATUSCODE_GOOD)
        COR_W("opcua: writing '%s' failed (%s)", eP->name, UA_StatusCode_name(sc));
      else
      {
        //
        // Its echo is not a sample: kept as the server will report it - rendered from the value written,
        // not as the broker spelled it ("70" and "70.0" are one value)
        //
        Buf b = { NULL, 0, 0, false };

        variantJson(&b, &opP->value);

        pthread_mutex_lock(&mutex);
        free(eP->lastWritten);
        eP->lastWritten = (b.failed == false) ? b.s : NULL;
        pthread_mutex_unlock(&mutex);

        if (b.failed == true)
          free(b.s);
      }
    }

    opFree(opP);
    opP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// connect - connect, subscribe, and set every endpoint up again; false: the server cannot be reached
//
static bool connect(void)
{
  UA_StatusCode sc = UA_Client_connect(client, serverUrl);

  if (sc != UA_STATUSCODE_GOOD)
  {
    char reason[256];

    snprintf(reason, sizeof(reason), "the OPC UA server %s cannot be reached (%s)", serverUrl, UA_StatusCode_name(sc));

    if (strcmp(reason, unreachableReason) != 0)       // said once, not on every retry
    {
      snprintf(unreachableReason, sizeof(unreachableReason), "%s", reason);
      COR_W("opcua: %s", reason);

      //
      // Each endpoint dormant, with the reason - the broker is called with no lock of ours held: a broker
      // thread may be inside opcuaTransportSend, waiting for it
      //
      char** names = endpointNames();

      for (int ix = 0; (names != NULL) && (names[ix] != NULL); ix++)
      {
        if (statusFunc != NULL)
          statusFunc(names[ix], false, reason);
        free(names[ix]);
      }

      free(names);
    }

    return false;
  }

  unreachableReason[0] = 0;

  UA_CreateSubscriptionRequest request = UA_CreateSubscriptionRequest_default();

  request.requestedPublishingInterval = publishingIntervalMs;

  UA_CreateSubscriptionResponse response = UA_Client_Subscriptions_create(client, request, NULL, NULL, NULL);

  if (response.responseHeader.serviceResult != UA_STATUSCODE_GOOD)
  {
    COR_W("opcua: no subscription on %s (%s)", serverUrl, UA_StatusCode_name(response.responseHeader.serviceResult));
    UA_Client_disconnect(client);
    return false;
  }

  subscriptionId = response.subscriptionId;
  connected      = true;
  COR_V("opcua: connected to %s", serverUrl);

  //
  // Every endpoint, again: after a reconnect the server's subscription is new, and a model may have changed
  //
  pthread_mutex_lock(&mutex);
  for (Endpoint* eP = endpoints; eP != NULL; eP = eP->next)
  {
    eP->ready   = false;
    eP->refused = false;
  }
  pthread_mutex_unlock(&mutex);

  char** names = endpointNames();

  for (int ix = 0; (names != NULL) && (names[ix] != NULL); ix++)
  {
    pthread_mutex_lock(&mutex);
    Endpoint* eP = endpointFind(names[ix]);
    pthread_mutex_unlock(&mutex);

    if (eP != NULL)
      endpointSetup(eP);

    free(names[ix]);
  }

  free(names);
  return true;
}



// -----------------------------------------------------------------------------
//
// disconnected - the connection is gone: everything set up on it is
//
static void disconnected(void)
{
  connected      = false;
  subscriptionId = 0;
  itemsClear();
  nameCacheClear();
  UA_Client_disconnect(client);
}



// -----------------------------------------------------------------------------
//
// clientThread -
//
static void* clientThread(void* arg)
{
  (void) arg;

  while (stopping == false)
  {
    if (connected == false)
    {
      if (connect() == false)
      {
        for (int ix = 0; (ix < 20) && (stopping == false); ix++)   // 2 s between attempts, stopping in 100 ms
          usleep(100 * 1000);
        continue;
      }
    }

    opsRun();

    UA_StatusCode sc = UA_Client_run_iterate(client, 50);

    UA_SecureChannelState channelState;
    UA_SessionState       sessionState;
    UA_StatusCode         connectStatus;

    UA_Client_getState(client, &channelState, &sessionState, &connectStatus);

    if ((sc != UA_STATUSCODE_GOOD) || (sessionState != UA_SESSIONSTATE_ACTIVATED))
    {
      COR_W("opcua: the connection to %s is lost (%s) - reconnecting", serverUrl, UA_StatusCode_name((sc != UA_STATUSCODE_GOOD) ? sc : connectStatus));
      disconnected();
    }
  }

  if (connected == true)
    disconnected();

  return NULL;
}



// -----------------------------------------------------------------------------
//
// numberMember - a positive number member of "server", or 'dflt'
//
static double numberMember(CorNode* serverP, const char* name, double dflt)
{
  CorNode* nP = (serverP != NULL) ? corTreeLookup(serverP, name) : NULL;

  if ((nP != NULL) && (nP->type == CorInt) && (nP->value.i > 0))
    return (double) nP->value.i;

  if ((nP != NULL) && (nP->type == CorFloat) && (nP->value.f > 0))
    return nP->value.f;

  if (nP != NULL)
    COR_W("opcua: server.%s is not a positive number - %g", name, dflt);

  return dflt;
}



// -----------------------------------------------------------------------------
//
// opcuaTransportOpen -
//
int opcuaTransportOpen(CorNode* serverP, OpcuaReceiveFunc receiveF, OpcuaStatusFunc statusF)
{
  CorNode* urlP = (serverP != NULL) ? corTreeLookup(serverP, "url") : NULL;

  if ((urlP == NULL) || (urlP->type != CorString) || (strncmp(urlP->value.s, "opc.tcp://", 10) != 0))
  {
    COR_E("opcua: \"server\": { \"url\": \"opc.tcp://<host>:<port>\" } is missing in the bridge configuration");
    return BRIDGE_ERR;
  }

  serverUrl            = strdup(urlP->value.s);
  publishingIntervalMs = numberMember(serverP, "publishingIntervalMs", 500);
  samplingIntervalMs   = numberMember(serverP, "samplingIntervalMs",   250);
  receiveFunc          = receiveF;
  statusFunc           = statusF;
  stopping             = false;
  unreachableReason[0] = 0;

  //
  // open62541's own log lines go to the broker's log - warnings and errors (uaLog). The logger is in the
  // configuration BEFORE the client is made from it: the event loop and the security policies keep the
  // logger they are given, and UA_Client_new() would make them with its own. Static, and no clear(): the
  // client's to use, not to free
  //
  logger.log     = uaLog;
  logger.context = NULL;
  logger.clear   = NULL;

  UA_ClientConfig config;

  memset(&config, 0, sizeof(config));
  config.logging = &logger;

  if (UA_ClientConfig_setDefault(&config) != UA_STATUSCODE_GOOD)
    return BRIDGE_ERR;

  config.timeout = 2000;                              // ms - a service call that waits longer has failed

  client = UA_Client_newWithConfig(&config);          // the config moves into the client

  if (client == NULL)
    return BRIDGE_ERR;

  if (pthread_create(&clientThreadId, NULL, clientThread, NULL) != 0)
  {
    UA_Client_delete(client);
    client = NULL;
    return BRIDGE_ERR;
  }

  clientThreadRunning = true;
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// opcuaTransportClose -
//
void opcuaTransportClose(void)
{
  stopping = true;

  if (clientThreadRunning == true)
  {
    pthread_join(clientThreadId, NULL);
    clientThreadRunning = false;
  }

  if (client != NULL)
  {
    UA_Client_delete(client);                         // the logger is static, with no clear() to call
    client = NULL;
  }

  pthread_mutex_lock(&mutex);

  while (endpoints != NULL)
  {
    Endpoint* nextP = endpoints->next;

    free(endpoints->name);
    free(endpoints->lastWritten);
    UA_NodeId_clear(&endpoints->nodeId);
    free(endpoints);

    endpoints = nextP;
  }

  while (opsHead != NULL)
  {
    Op* nextP = opsHead->next;
    opFree(opsHead);
    opsHead = nextP;
  }

  opsTail = NULL;
  pthread_mutex_unlock(&mutex);

  free(serverUrl);
  serverUrl = NULL;
}



// -----------------------------------------------------------------------------
//
// opcuaTransportEndpointAdd -
//
int opcuaTransportEndpointAdd(const char* endpoint, BridgeDirection direction, const char** whyP)
{
  (void) direction;

  UA_NodeId nodeId;

  if (UA_NodeId_parse(&nodeId, UA_STRING((char*) endpoint)) != UA_STATUSCODE_GOOD)
  {
    *whyP = "not an OPC UA NodeId (ns=<namespace>;i=<number>, ns=<namespace>;s=<string>, ...)";
    return BRIDGE_BAD_INPUT;
  }

  Endpoint* eP = (Endpoint*) calloc(1, sizeof(Endpoint));

  if (eP == NULL)
  {
    UA_NodeId_clear(&nodeId);
    return BRIDGE_ERR;
  }

  eP->name   = strdup(endpoint);
  eP->nodeId = nodeId;

  pthread_mutex_lock(&mutex);

  if (endpointFind(endpoint) != NULL)
  {
    pthread_mutex_unlock(&mutex);
    free(eP->name);
    UA_NodeId_clear(&eP->nodeId);
    free(eP);
    *whyP = "the endpoint is carried already";
    return BRIDGE_BAD_INPUT;
  }

  eP->next  = endpoints;
  endpoints = eP;

  pthread_mutex_unlock(&mutex);

  opPush(OpAdd, endpoint, NULL, NULL);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// opcuaTransportEndpointDel - the broker does not call it yet (Channels come from the file)
//
int opcuaTransportEndpointDel(const char* endpoint)
{
  pthread_mutex_lock(&mutex);
  bool found = (endpointFind(endpoint) != NULL);
  pthread_mutex_unlock(&mutex);

  return (found == true) ? BRIDGE_UNSUPPORTED : BRIDGE_NOT_FOUND;
}



// -----------------------------------------------------------------------------
//
// jsonToVariant - a published value, as the node's DataType; false: it does not fit
//
static bool jsonToVariant(const char* json, const UA_DataType* typeP, UA_Variant* vP)
{
  while ((*json == ' ') || (*json == '\t') || (*json == '\n'))
    json++;

  if (typeP == &UA_TYPES[UA_TYPES_BOOLEAN])
  {
    UA_Boolean b;

    if (strncmp(json, "true", 4) == 0)
      b = true;
    else if (strncmp(json, "false", 5) == 0)
      b = false;
    else
      return false;

    return UA_Variant_setScalarCopy(vP, &b, typeP) == UA_STATUSCODE_GOOD;
  }

  if (typeP == &UA_TYPES[UA_TYPES_STRING])
  {
    size_t len = strlen(json);

    if ((len < 2) || (json[0] != '"') || (json[len - 1] != '"') || (memchr(json + 1, '\\', len - 2) != NULL))
      return false;                                   // a plain JSON string - escapes are not decoded here

    UA_String s = { len - 2, (UA_Byte*) (json + 1) };

    return UA_Variant_setScalarCopy(vP, &s, typeP) == UA_STATUSCODE_GOOD;
  }

  char*  endP;
  double d = strtod(json, &endP);

  while ((*endP == ' ') || (*endP == '\n'))
    endP++;

  if ((endP == json) || (*endP != 0) || (isfinite(d) == 0))
    return false;

  //
  // A number: as the node's numeric type - an integer type takes an integral value within its range
  //
  union { UA_SByte sb; UA_Byte b; UA_Int16 i16; UA_UInt16 u16; UA_Int32 i32; UA_UInt32 u32; UA_Int64 i64; UA_UInt64 u64; UA_Float f; UA_Double d; } u;

  bool integral = (d == (double) (long long) d);

  switch (typeP->typeKind)
  {
  case UA_DATATYPEKIND_DOUBLE:  u.d   = d;                                                            break;
  case UA_DATATYPEKIND_FLOAT:   u.f   = (UA_Float) d;                                                 break;
  case UA_DATATYPEKIND_SBYTE:   if (!integral || d < -128 || d > 127)               return false; u.sb  = (UA_SByte) d;  break;
  case UA_DATATYPEKIND_BYTE:    if (!integral || d < 0 || d > 255)                  return false; u.b   = (UA_Byte) d;   break;
  case UA_DATATYPEKIND_INT16:   if (!integral || d < -32768 || d > 32767)           return false; u.i16 = (UA_Int16) d;  break;
  case UA_DATATYPEKIND_UINT16:  if (!integral || d < 0 || d > 65535)                return false; u.u16 = (UA_UInt16) d; break;
  case UA_DATATYPEKIND_INT32:   if (!integral || d < -2147483648.0 || d > 2147483647.0) return false; u.i32 = (UA_Int32) d; break;
  case UA_DATATYPEKIND_UINT32:  if (!integral || d < 0 || d > 4294967295.0)         return false; u.u32 = (UA_UInt32) d; break;
  case UA_DATATYPEKIND_INT64:   if (!integral)                                      return false; u.i64 = (UA_Int64) d;  break;
  case UA_DATATYPEKIND_UINT64:  if (!integral || d < 0)                             return false; u.u64 = (UA_UInt64) d; break;
  default:                                                                          return false;
  }

  return UA_Variant_setScalarCopy(vP, &u, typeP) == UA_STATUSCODE_GOOD;
}



// -----------------------------------------------------------------------------
//
// opcuaTransportSend -
//
int opcuaTransportSend(const char* endpoint, const char* json)
{
  pthread_mutex_lock(&mutex);

  Endpoint* eP = endpointFind(endpoint);

  if (eP == NULL)
  {
    pthread_mutex_unlock(&mutex);
    return BRIDGE_NOT_FOUND;
  }

  if ((eP->kind == EndpointModel) || (eP->refused == true))
  {
    pthread_mutex_unlock(&mutex);
    return BRIDGE_BAD_INPUT;
  }

  if ((eP->ready == false) || (eP->dataType == NULL))
  {
    pthread_mutex_unlock(&mutex);
    return BRIDGE_ERR;                                // not connected, or not read yet
  }

  const UA_DataType* typeP = eP->dataType;

  pthread_mutex_unlock(&mutex);

  UA_Variant value;
  UA_Variant_init(&value);

  if (jsonToVariant(json, typeP, &value) == false)
    return BRIDGE_BAD_INPUT;

  opPush(OpWrite, endpoint, &value, strdup(json));
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// opcuaTransportVersion -
//
const char* opcuaTransportVersion(void)
{
  static char version[64];

  snprintf(version, sizeof(version), "open62541 %d.%d.%d", UA_OPEN62541_VER_MAJOR, UA_OPEN62541_VER_MINOR, UA_OPEN62541_VER_PATCH);
  return version;
}
