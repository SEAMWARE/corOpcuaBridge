//
// FILE            opcuaTestServer.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The OPC UA server the functional tests talk to - the bridge's peer, as mosquitto is the MQTT bridge's.
//
//   opcuaTestServer <port> <command fifo> <writes file>
//
// Its address space, namespace 2 ("urn:seamware:opcua-test" - namespace 1 is the server's own):
//
//   Objects/Press17          ns=2;s=Press17, a PressType - HasDictionaryEntry 0173-1#01-AKJ975#017
//     Hydraulics             ns=2;s=P17.Hydraulics, a folder (Organizes)
//       Pressure             ns=2;s=P17.Pressure, Double 212.5, EngineeringUnits BAR
//       OilTemp              ns=2;s=P17.OilTemp, Double 48.1, EngineeringUnits CEL
//     Mode                   ns=2;s=P17.Mode, String "auto", writable
//     Unit1                  ns=2;s=P17.Unit1, a HydraulicUnitType - HasEffect -> Press17
//       Running              ns=2;s=P17.Unit1.Running, Boolean true
//     Start                  ns=2;s=P17.Start, a method
//   Objects/Tank1            ns=2;s=Tank1
//     Level                  ns=2;s=Tank1.Level, Double 1.5
//     Setpoint               ns=2;s=Tank1.Setpoint, Double 0, writable
//     Name                   ns=2;s=Tank1.Name, String "T1", writable
//
// Every Variable's SourceTimestamp starts as 2026-10-17T10:00:00Z: a test asserts the observedAt it becomes
// literally.
//
// The command fifo takes one line per change - the server's own value changing, as a device's would, at the
// time it says:
//
//   set <node id> <ISO 8601 time> <JSON value>        set ns=2;s=P17.Pressure 2026-10-17T10:00:05Z 214
//
// Every Write a client makes is appended to the writes file: "<node id> <JSON value>".
//
#include <fcntl.h>                                    // open, O_RDWR, O_NONBLOCK
#include <signal.h>                                   // signal, SIGTERM, SIGINT
#include <stdio.h>                                    // fprintf, snprintf, fopen
#include <stdlib.h>                                   // atoi, strtod
#include <string.h>                                   // strchr, strncmp, strlen
#include <unistd.h>                                   // read, usleep

#include <open62541/server.h>
#include <open62541/server_config_default.h>



static volatile UA_Boolean running = true;
static const char*         writesPath = NULL;
static UA_UInt16           ns = 1;
static UA_Boolean          ownWrite = false;      // a 'set': the server's own change, not a client's Write



static void stop(int sig)
{
  (void) sig;
  running = false;
}



// -----------------------------------------------------------------------------
//
// valueText - a Variant as the writes file shows it: a number, true/false, a "string"
//
static void valueText(const UA_Variant* vP, char* buf, size_t size)
{
  if (UA_Variant_hasScalarType(vP, &UA_TYPES[UA_TYPES_DOUBLE]))
    snprintf(buf, size, "%g", *(UA_Double*) vP->data);
  else if (UA_Variant_hasScalarType(vP, &UA_TYPES[UA_TYPES_BOOLEAN]))
    snprintf(buf, size, "%s", (*(UA_Boolean*) vP->data) ? "true" : "false");
  else if (UA_Variant_hasScalarType(vP, &UA_TYPES[UA_TYPES_STRING]))
  {
    UA_String* sP = (UA_String*) vP->data;
    snprintf(buf, size, "\"%.*s\"", (int) sP->length, (char*) sP->data);
  }
  else
    snprintf(buf, size, "?");
}



// -----------------------------------------------------------------------------
//
// written - a client wrote a node: the writes file says so
//
static void written(UA_Server* server, const UA_NodeId* sessionId, void* sessionContext, const UA_NodeId* nodeId,
                    void* nodeContext, const UA_NumericRange* range, const UA_DataValue* data)
{
  (void) server; (void) sessionId; (void) sessionContext; (void) nodeContext; (void) range;

  if ((ownWrite == true) || (data == NULL) || (data->hasValue == false))
    return;                                           // the server's own change (a 'set'), not a client's Write

  UA_String id = UA_STRING_NULL;
  char      value[256];

  UA_NodeId_print(nodeId, &id);
  valueText(&data->value, value, sizeof(value));

  FILE* fP = fopen(writesPath, "a");

  if (fP != NULL)
  {
    fprintf(fP, "%.*s %s\n", (int) id.length, (char*) id.data, value);
    fclose(fP);
  }

  UA_String_clear(&id);
}



// -----------------------------------------------------------------------------
//
// unitAdd - an EngineeringUnits property: the UN/CEFACT common code in unitId, a byte a character
//
static void unitAdd(UA_Server* server, const char* parent, const char* code, const char* name)
{
  UA_EUInformation eu;
  UA_EUInformation_init(&eu);

  eu.namespaceUri = UA_STRING("http://www.opcfoundation.org/UA/units/un/cefact");
  eu.unitId       = (code[0] << 16) | (code[1] << 8) | code[2];
  eu.displayName  = UA_LOCALIZEDTEXT("", (char*) name);

  UA_VariableAttributes attr = UA_VariableAttributes_default;
  UA_Variant_setScalar(&attr.value, &eu, &UA_TYPES[UA_TYPES_EUINFORMATION]);
  attr.dataType    = UA_TYPES[UA_TYPES_EUINFORMATION].typeId;
  attr.displayName = UA_LOCALIZEDTEXT("", "EngineeringUnits");

  char id[128];
  snprintf(id, sizeof(id), "%s.EngineeringUnits", parent);

  UA_Server_addVariableNode(server, UA_NODEID_STRING(ns, id), UA_NODEID_STRING(ns, (char*) parent),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY), UA_QUALIFIEDNAME(0, "EngineeringUnits"),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), attr, NULL, NULL);
}



// -----------------------------------------------------------------------------
//
// variableAdd - a Variable under 'parent', by HasComponent
//
static void variableAdd(UA_Server* server, const char* parent, const char* id, const char* name, const UA_DataType* typeP, void* value, bool writable)
{
  UA_VariableAttributes attr = UA_VariableAttributes_default;

  UA_Variant_setScalar(&attr.value, value, typeP);
  attr.dataType    = typeP->typeId;
  attr.displayName = UA_LOCALIZEDTEXT("", (char*) name);
  attr.accessLevel = UA_ACCESSLEVELMASK_READ | (writable ? UA_ACCESSLEVELMASK_WRITE : 0);

  UA_Server_addVariableNode(server, UA_NODEID_STRING(ns, (char*) id), UA_NODEID_STRING(ns, (char*) parent),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), UA_QUALIFIEDNAME(ns, (char*) name),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, NULL, NULL);

  UA_ValueCallback callback = { NULL, written };
  UA_Server_setVariableNode_valueCallback(server, UA_NODEID_STRING(ns, (char*) id), callback);
}



// -----------------------------------------------------------------------------
//
// objectAdd / objectTypeAdd -
//
static void objectAdd(UA_Server* server, const UA_NodeId parent, const UA_NodeId refType, const char* id, const char* name, const UA_NodeId type)
{
  UA_ObjectAttributes attr = UA_ObjectAttributes_default;
  attr.displayName = UA_LOCALIZEDTEXT("", (char*) name);

  UA_Server_addObjectNode(server, UA_NODEID_STRING(ns, (char*) id), parent, refType, UA_QUALIFIEDNAME(ns, (char*) name), type, attr, NULL, NULL);
}

static void objectTypeAdd(UA_Server* server, const char* id, const char* name)
{
  UA_ObjectTypeAttributes attr = UA_ObjectTypeAttributes_default;
  attr.displayName = UA_LOCALIZEDTEXT("", (char*) name);

  UA_Server_addObjectTypeNode(server, UA_NODEID_STRING(ns, (char*) id), UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE), UA_QUALIFIEDNAME(ns, (char*) name), attr, NULL, NULL);
}



static UA_StatusCode startCalled(UA_Server* server, const UA_NodeId* sessionId, void* sessionHandle, const UA_NodeId* methodId,
                                 void* methodContext, const UA_NodeId* objectId, void* objectContext, size_t inputSize,
                                 const UA_Variant* input, size_t outputSize, UA_Variant* output)
{
  (void) server; (void) sessionId; (void) sessionHandle; (void) methodId; (void) methodContext;
  (void) objectId; (void) objectContext; (void) inputSize; (void) input; (void) outputSize; (void) output;
  return UA_STATUSCODE_GOOD;
}



// -----------------------------------------------------------------------------
//
// addressSpace - the press and the tank
//
static void addressSpace(UA_Server* server)
{
  ns = UA_Server_addNamespace(server, "urn:seamware:opcua-test");

  objectTypeAdd(server, "PressType",         "PressType");
  objectTypeAdd(server, "HydraulicUnitType", "HydraulicUnitType");

  UA_NodeId objects   = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
  UA_NodeId organizes = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
  UA_NodeId component = UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT);

  //
  // A dictionary entry - what the press is, as an IRDI (HasDictionaryEntry)
  //
  objectAdd(server, objects, organizes, "Dictionary", "Dictionary", UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE));
  objectAdd(server, UA_NODEID_STRING(ns, "Dictionary"), organizes, "0173-1#01-AKJ975#017", "Press", UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE));

  objectAdd(server, objects, organizes, "Press17", "Press17", UA_NODEID_STRING(ns, "PressType"));
  UA_Server_addReference(server, UA_NODEID_STRING(ns, "Press17"), UA_NODEID_NUMERIC(0, UA_NS0ID_HASDICTIONARYENTRY),
                         UA_EXPANDEDNODEID_STRING(ns, "0173-1#01-AKJ975#017"), true);

  objectAdd(server, UA_NODEID_STRING(ns, "Press17"), organizes, "P17.Hydraulics", "Hydraulics", UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE));

  UA_Double  pressure = 212.5;
  UA_Double  oilTemp  = 48.1;
  UA_String  mode     = UA_STRING("auto");
  UA_Boolean runningV = true;

  variableAdd(server, "P17.Hydraulics", "P17.Pressure", "Pressure", &UA_TYPES[UA_TYPES_DOUBLE], &pressure, true);
  unitAdd(server, "P17.Pressure", "BAR", "bar");
  variableAdd(server, "P17.Hydraulics", "P17.OilTemp", "OilTemp", &UA_TYPES[UA_TYPES_DOUBLE], &oilTemp, false);
  unitAdd(server, "P17.OilTemp", "CEL", "degree Celsius");
  variableAdd(server, "Press17", "P17.Mode", "Mode", &UA_TYPES[UA_TYPES_STRING], &mode, true);

  objectAdd(server, UA_NODEID_STRING(ns, "Press17"), component, "P17.Unit1", "Unit1", UA_NODEID_STRING(ns, "HydraulicUnitType"));
  variableAdd(server, "P17.Unit1", "P17.Unit1.Running", "Running", &UA_TYPES[UA_TYPES_BOOLEAN], &runningV, false);
  UA_Server_addReference(server, UA_NODEID_STRING(ns, "P17.Unit1"), UA_NODEID_NUMERIC(0, UA_NS0ID_HASEFFECT),
                         UA_EXPANDEDNODEID_STRING(ns, "Press17"), true);

  UA_MethodAttributes mattr = UA_MethodAttributes_default;
  mattr.displayName    = UA_LOCALIZEDTEXT("", "Start");
  mattr.executable     = true;
  mattr.userExecutable = true;
  UA_Server_addMethodNode(server, UA_NODEID_STRING(ns, "P17.Start"), UA_NODEID_STRING(ns, "Press17"), component,
                          UA_QUALIFIEDNAME(ns, "Start"), mattr, startCalled, 0, NULL, 0, NULL, NULL, NULL);

  objectAdd(server, objects, organizes, "Tank1", "Tank1", UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE));

  UA_Double level    = 1.5;
  UA_Double setpoint = 0;
  UA_String name     = UA_STRING("T1");

  variableAdd(server, "Tank1", "Tank1.Level",    "Level",    &UA_TYPES[UA_TYPES_DOUBLE], &level,    false);
  variableAdd(server, "Tank1", "Tank1.Setpoint", "Setpoint", &UA_TYPES[UA_TYPES_DOUBLE], &setpoint, true);
  variableAdd(server, "Tank1", "Tank1.Name",     "Name",     &UA_TYPES[UA_TYPES_STRING], &name,     true);
}



// -----------------------------------------------------------------------------
//
// isoParse - "2026-10-17T10:00:05Z" (or with .mmm) as a UA_DateTime; 0 when it is not one
//
static UA_DateTime isoParse(const char* iso)
{
  UA_DateTimeStruct ts;
  unsigned int      y, mo, d, h, mi, sec, ms = 0;

  memset(&ts, 0, sizeof(ts));

  if (sscanf(iso, "%4u-%2u-%2uT%2u:%2u:%2u.%3u", &y, &mo, &d, &h, &mi, &sec, &ms) < 6)
    return 0;

  ts.year = (UA_Int16) y; ts.month = mo; ts.day = d; ts.hour = h; ts.min = mi; ts.sec = sec; ts.milliSec = ms;
  return UA_DateTime_fromStruct(ts);
}



// -----------------------------------------------------------------------------
//
// valueSet - a Variable's value, at a SourceTimestamp - the server's own change
//
static void valueSet(UA_Server* server, const UA_NodeId id, UA_Variant* vP, UA_DateTime t)
{
  UA_DataValue dv;
  UA_DataValue_init(&dv);
  dv.value              = *vP;
  dv.hasValue           = true;
  dv.sourceTimestamp    = t;
  dv.hasSourceTimestamp = true;

  ownWrite = true;
  UA_Server_writeDataValue(server, id, dv);
  ownWrite = false;
}



// -----------------------------------------------------------------------------
//
// timestampsFix - every Variable of the address space: its value again, SourceTimestamp 2026-10-17T10:00:00Z
//
static void timestampsFix(UA_Server* server)
{
  const char* ids[] = { "P17.Pressure", "P17.OilTemp", "P17.Mode", "P17.Unit1.Running", "Tank1.Level", "Tank1.Setpoint", "Tank1.Name", NULL };
  UA_DateTime t     = isoParse("2026-10-17T10:00:00Z");

  for (int ix = 0; ids[ix] != NULL; ix++)
  {
    UA_Variant v;
    UA_Variant_init(&v);

    if (UA_Server_readValue(server, UA_NODEID_STRING(ns, (char*) ids[ix]), &v) == UA_STATUSCODE_GOOD)
      valueSet(server, UA_NODEID_STRING(ns, (char*) ids[ix]), &v, t);

    UA_Variant_clear(&v);
  }
}



// -----------------------------------------------------------------------------
//
// command - "set <node id> <ISO 8601 time> <JSON value>": the value as the node's current type has it
//
static void command(UA_Server* server, char* line)
{
  if (strncmp(line, "set ", 4) != 0)
  {
    fprintf(stderr, "opcuaTestServer: unknown command '%s'\n", line);
    return;
  }

  char* idP    = line + 4;
  char* timeP  = strchr(idP, ' ');
  char* valueP = (timeP != NULL) ? strchr(timeP + 1, ' ') : NULL;

  if (valueP == NULL)
  {
    fprintf(stderr, "opcuaTestServer: set <node id> <ISO 8601 time> <JSON value>\n");
    return;
  }

  *timeP++  = 0;
  *valueP++ = 0;

  UA_DateTime t = isoParse(timeP);

  if (t == 0)
  {
    fprintf(stderr, "opcuaTestServer: '%s' is no ISO 8601 time\n", timeP);
    return;
  }

  UA_NodeId id;

  if (UA_NodeId_parse(&id, UA_STRING(idP)) != UA_STATUSCODE_GOOD)
  {
    fprintf(stderr, "opcuaTestServer: '%s' is no node id\n", idP);
    return;
  }

  UA_Variant current;
  UA_Variant_init(&current);
  UA_Server_readValue(server, id, &current);

  UA_Variant v;
  UA_Variant_init(&v);

  if (UA_Variant_hasScalarType(&current, &UA_TYPES[UA_TYPES_DOUBLE]))
  {
    UA_Double d = strtod(valueP, NULL);
    UA_Variant_setScalarCopy(&v, &d, &UA_TYPES[UA_TYPES_DOUBLE]);
  }
  else if (UA_Variant_hasScalarType(&current, &UA_TYPES[UA_TYPES_BOOLEAN]))
  {
    UA_Boolean b = (strncmp(valueP, "true", 4) == 0);
    UA_Variant_setScalarCopy(&v, &b, &UA_TYPES[UA_TYPES_BOOLEAN]);
  }
  else if (UA_Variant_hasScalarType(&current, &UA_TYPES[UA_TYPES_STRING]))
  {
    size_t len = strlen(valueP);

    if ((len >= 2) && (valueP[0] == '"') && (valueP[len - 1] == '"'))
    {
      valueP[len - 1] = 0;
      valueP++;
    }

    UA_String s = UA_STRING(valueP);
    UA_Variant_setScalarCopy(&v, &s, &UA_TYPES[UA_TYPES_STRING]);
  }

  valueSet(server, id, &v, t);

  UA_Variant_clear(&v);
  UA_Variant_clear(&current);
  UA_NodeId_clear(&id);
}



int main(int argc, char* argv[])
{
  if (argc != 4)
  {
    fprintf(stderr, "usage: opcuaTestServer <port> <command fifo> <writes file>\n");
    return 1;
  }

  writesPath = argv[3];

  signal(SIGTERM, stop);
  signal(SIGINT,  stop);

  int fd = open(argv[2], O_RDWR | O_NONBLOCK);        // RDWR: a writer always there - no EOF between commands

  if (fd < 0)
  {
    fprintf(stderr, "opcuaTestServer: cannot open the fifo '%s'\n", argv[2]);
    return 1;
  }

  UA_Server* server = UA_Server_new();

  UA_ServerConfig_setMinimal(UA_Server_getConfig(server), (UA_UInt16) atoi(argv[1]), NULL);
  addressSpace(server);
  timestampsFix(server);

  UA_Server_run_startup(server);

  char   line[1024];
  size_t len = 0;

  while (running)
  {
    UA_Server_run_iterate(server, false);

    char    c;
    ssize_t n;

    while ((n = read(fd, &c, 1)) == 1)
    {
      if (c == '\n')
      {
        line[len] = 0;
        command(server, line);
        len = 0;
      }
      else if (len < sizeof(line) - 1)
        line[len++] = c;
    }

    usleep(5 * 1000);
  }

  UA_Server_run_shutdown(server);
  UA_Server_delete(server);
  return 0;
}
