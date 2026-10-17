# OPC UA in coraine - a guide

How an OPC UA server's data becomes NGSI-LD in the coraine broker, and back: what you configure, what
you get, how to secure it, and how to tell what is going on. No source code is needed to follow it -
every example below is real output, from the test server this repository ships (`test/opcuaTestServer`).

- [1. What the bridge does](#1-what-the-bridge-does)
- [2. Two ways in: a Variable, or an Object](#2-two-ways-in-a-variable-or-an-object)
- [3. Getting started](#3-getting-started)
- [4. Variables - Channels](#4-variables---channels)
- [5. Objects - AssetChannels](#5-objects---assetchannels)
- [6. Security](#6-security)
- [7. Status and troubleshooting](#7-status-and-troubleshooting)
- [8. From c³](#8-from-c)
- [9. What it does not do yet](#9-what-it-does-not-do-yet)
- [10. Testing it against your own server](#10-testing-it-against-your-own-server)

## 1. What the bridge does

The bridge (`opcua.so`) is a plugin of the coraine broker: an OPC UA **client** of **one** OPC UA
server. The broker loads it when started with `--bridges opcua`, and reads which nodes to carry from its
bridge configuration file (`--bridgeConfig`). It connects, reads, browses, subscribes to changes and
writes - and the broker turns all of it into NGSI-LD entities: stored, queried, subscribed to and kept
in history like any entity a client writes.

    OPC UA server  <-- opc.tcp -->  opcua.so  <-->  coraine  <-- NGSI-LD -->  applications

The bridge connects without holding up the broker: a server that is not reachable yet is retried every
2 seconds, and the broker says why nothing arrives meanwhile (section 7).

## 2. Two ways in: a Variable, or an Object

You name OPC UA nodes by their **NodeId**, written as OPC UA writes it: `ns=2;s=Press17`,
`ns=1;i=1001`. What the bridge does with a node is decided by the node's class:

| You name | coraine calls it | It becomes |
|---|---|---|
| a **Variable** | a **Channel** | **one attribute** of an entity you name - its value in, written back out |
| an **Object** | an **AssetChannel** | a **graph of entities**: the object and what it contains, browsed |

**A Channel** is the precise tool: you decide the entity, its type and the attribute name, and the value
goes both ways - a PATCH of the attribute is a Write to the Variable.

**An AssetChannel** is the model tool: you name one Object (a machine, a line, a plant section), and the
bridge browses everything it contains. Its parts become entities, its Variables attributes, its
references Relationships - without naming them one by one. It is read-only in this version: values come
in, nothing is written back to the server.

Both can be mixed in one configuration, on one server.

## 3. Getting started

**Install.** On Ubuntu 26.04 or Debian 13 the bridge needs open62541 1.4 from the distribution:

    sudo apt-get install libopen62541-1.4          # libopen62541-1.4-dev to build the bridge

(Ubuntu 24.04 has no open62541 package, and no OPC UA bridge.) The bridge is built with the broker by
`coraine-build <dir> --bridges opcua`, or from this repository (`make`, `make install`); c³'s Compile page
offers it as a bridge (section 8).

**Configure.** One file, `bridges.json`. The `server` part is the bridge's; the `ngsild` part tells the
broker what to make:

```json
{ "opcua": {
    "server": { "url": "opc.tcp://press-17.example.com:4840" },
    "ngsild": {
      "defaultEntity": false,
      "topics": {
        "ns=2;s=Tank1.Level": { "entityId": "urn:ngsi-ld:Tank:T1", "entityType": "Tank", "attribute": "level" }
      },
      "assetChannels": {
        "ns=2;s=Press17": { "entities": "natural" }
      } } } }
```

- `topics` - the Channels: a Variable's NodeId, and the entity and attribute it is.
- `assetChannels` - the AssetChannels: an Object's NodeId, and how its model becomes entities (section 5).
- `defaultEntity: false` - leave it so: it is a catch-all for bridges that hand over endpoints nobody
  named, which this one never does.

**Run.**

    coraine --bridges opcua --bridgeConfig bridges.json

**Look.**

    curl localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:Tank:T1
    curl localhost:1026/ngsi-ld/v1/channels           # the Channels, and how each one is doing
    curl localhost:1026/ngsi-ld/v1/assetChannels      # the AssetChannels, likewise

## 4. Variables - Channels

```json
"topics": {
  "ns=2;s=Tank1.Level":    { "entityId": "urn:ngsi-ld:Tank:T1", "entityType": "Tank", "attribute": "level" },
  "ns=2;s=Tank1.Setpoint": { "entityId": "urn:ngsi-ld:Tank:T1", "entityType": "Tank", "attribute": "setpoint" }
}
```

**In.** When the Channel is added the bridge reads the Variable's value, and from then on the server
reports each change (a monitored item in one subscription). Each value becomes the attribute's value;
the Variable's **SourceTimestamp** becomes the attribute's `observedAt`:

```json
"level": { "type": "Property", "value": 2.25, "observedAt": "2026-10-17T10:00:05Z" }
```

**Out.** A client that PATCHes the attribute makes the bridge **Write** the Variable - converted to the
Variable's DataType, which the bridge reads when the Channel is added:

| DataType | takes |
|---|---|
| Boolean | `true`, `false` |
| SByte, Byte, Int16, UInt16, Int32, UInt32, Int64, UInt64 | a whole number within the type's range |
| Float, Double | a number |
| String | a JSON string |

A value that does not fit - `"hot"` for a Double, `300` for a Byte - is refused **before anything is
stored**: the client gets `400`, and the attribute keeps its value. Arrays and structured values cannot be
written yet.

**No echo.** The server reports the value the bridge just wrote as a change; the bridge recognises its own
Write and does not take it back in. (On the Channel, `samplesOut` counts writes, `samplesIn` values taken
in.)

**Data types read.** Boolean, the integer types, Float, Double (a non-finite one as `null`), String,
LocalizedText (its text), QualifiedName (its name), DateTime (ISO 8601), NodeId (as text), StatusCode (a
number); an array of these as a JSON array. Any other type arrives as `null`.

## 5. Objects - AssetChannels

### 5.1 What is browsed

From the Object you name, the bridge follows the **forward** references that say what a node contains -
**HasComponent**, **HasOrderedComponent**, **HasProperty**, **Organizes** - to Objects, Variables and
Methods, down to a depth of 16 and up to 10,000 nodes (a larger model is cut there, and the broker's log
says so). For every node it reads:

| OPC UA | used as |
|---|---|
| NodeClass | Object - `object` (`folder` when its TypeDefinition is FolderType), Variable - `variable`, Method - `method` |
| NodeId | the node's identity - entity ids, value updates |
| BrowseName | the node's name - entity ids, attribute names |
| TypeDefinition (HasTypeDefinition), and its BrowseName | the entity type (`natural`), the rules' `typeDefinition` |
| HasDictionaryEntry targets | the node's meaning: a `semanticId` (a string NodeId - an IRDI, a URI - as the string itself) |
| a Variable's Value and SourceTimestamp | the attribute's value and `observedAt` |
| a Variable's EngineeringUnits property | the attribute's `unitCode`: the UN/CEFACT common code in EUInformation.unitId (`BAR`, `CEL`) |
| other forward references (not type, modelling rule, encoding or dictionary ones) | Relationships, or Properties (5.4) |

A Variable's own properties (EngineeringUnits, EURange, ...) are read as metadata, not as parts.

### 5.2 A worked example

The test server's address space (namespace 2):

    Objects
    ├── Press17                     Object, TypeDefinition PressType - HasDictionaryEntry 0173-1#01-AKJ975#017
    │   ├── Hydraulics              Object, FolderType (Organizes)
    │   │   ├── Pressure            Variable, Double 212.5, EngineeringUnits BAR
    │   │   └── OilTemp             Variable, Double 48.1,  EngineeringUnits CEL
    │   ├── Mode                    Variable, String "auto"
    │   ├── Unit1                   Object, TypeDefinition HydraulicUnitType - HasEffect -> Press17
    │   │   └── Running             Variable, Boolean true
    │   └── Start                   Method
    └── Tank1 ...

With

```json
"assetChannels": { "ns=2;s=Press17": { "entities": "natural" } }
```

the broker holds two entities:

```json
[
  { "id": "urn:ngsi-ld:PressType:Press17",
    "type": "PressType",
    "semanticId":         { "type": "Property", "value": "0173-1#01-AKJ975#017" },
    "hydraulicsPressure": { "type": "Property", "value": 212.5, "unitCode": "BAR", "observedAt": "2026-10-17T10:00:00Z" },
    "hydraulicsOilTemp":  { "type": "Property", "value": 48.1,  "unitCode": "CEL", "observedAt": "2026-10-17T10:00:00Z" },
    "mode":               { "type": "Property", "value": "auto", "observedAt": "2026-10-17T10:00:00Z" },
    "unit1":              { "type": "Relationship", "object": "urn:ngsi-ld:HydraulicUnitType:Press17:Unit1" } },

  { "id": "urn:ngsi-ld:HydraulicUnitType:Press17:Unit1",
    "type": "HydraulicUnitType",
    "running":   { "type": "Property", "value": true, "observedAt": "2026-10-17T10:00:00Z" },
    "hasEffect": { "type": "Relationship", "object": "urn:ngsi-ld:PressType:Press17" } }
]
```

Read it as:

- **Press17 and Unit1 are entities** - with `natural`, every Object is (5.3); the Hydraulics folder is not.
- **Their types are their TypeDefinitions' names**: `PressType`, `HydraulicUnitType`.
- **The folder's Variables are attributes of the press, named by their path**: `Hydraulics` + `Pressure`
  -> `hydraulicsPressure` (5.5).
- **Unit1 is a part of the press, and an entity of its own**: the press has a Relationship to it, named
  by its BrowseName (`unit1`).
- **Unit1's HasEffect reference is a Relationship** `hasEffect`, to the press's entity.
- **The press's dictionary entry is its `semanticId`**; a Variable with one has a `semanticId`
  sub-Property on its attribute.
- **The method `Start` makes nothing** (section 9).

### 5.3 Which nodes become entities - `entities`

You must say; there is no default.

- **`"natural"`** - every **Object** is an entity (a folder is not: its content goes into the entity
  above it). Its entity type is its TypeDefinition's BrowseName.
- **Rules** - only the Objects a rule names, each rule giving the entity type:

```json
"ns=2;s=Press17": { "entities": [ { "from": "object", "typeDefinition": "ns=2;s=PressType", "type": "Press" } ] }
```

  `from` is the node kind (`object`, or `folder`); `typeDefinition` and `semanticId` (a
  HasDictionaryEntry target), when given, must match too. The first rule a node matches wins. A node no
  rule names is not an entity: its content goes into the entity above it. With the rule above, Unit1 is
  no longer an entity, and folds into the press:

```json
{ "id": "urn:ngsi-ld:Press:Press17", "type": "Press",
  "semanticId": "0173-1#01-AKJ975#017",
  "hydraulicsPressure": 212.5, "hydraulicsOilTemp": 48.1, "mode": "auto",
  "unit1Running": true,
  "hasEffect": "urn:ngsi-ld:Press:Press17" }
```

  (shown in key-value form). Note the last line: a reference of a node that folded into an entity
  belongs to that entity - here Unit1's reference to the press has become the press referring to itself.

### 5.4 Entity ids, Relationships and references

- **Entity ids.** An OPC UA NodeId is not an NGSI-LD id, so an entity's id is made of its type and the
  BrowseNames from the root you named to the node: `urn:ngsi-ld:<type>:<name>:<name>...`, each byte
  outside `[A-Za-z0-9._-]` as `%XX` - `urn:ngsi-ld:HydraulicUnitType:Press17:Unit1`. The same model gives
  the same ids every time.
- **A part that is an entity** (Unit1 in the press): a Relationship from the entity above, named by the
  part's BrowseName.
- **Another forward reference** (HasEffect, a vendor's own reference type): a Relationship named by the
  reference type's BrowseName (`hasEffect`) when its target is an entity of the model - several of one
  type make it a ListRelationship; a **Property**, its value the target's NodeId, when the target is not
  (outside the model, or not an entity).

### 5.5 Attribute names, and what a container becomes - `attributeLayout`

An attribute is named by the BrowseNames of its path from the entity, joined in camel case: only
`[A-Za-z0-9_]` is kept, a space starts a new word (`Hydraulics` / `Oil Temp` -> `hydraulicsOilTemp`).

What a **container that is not an entity** (a folder, a folded Object) becomes is the channel's
`attributeLayout`:

| `attributeLayout` | the Hydraulics folder becomes |
|---|---|
| `attributes` (the default) | one attribute per Variable, named by its path - each one queryable with `q` (5.2) |
| `subAttributes` | one attribute `hydraulics`, its Variables sub-attributes |
| `value` | one JsonProperty `hydraulics`, the folder's values as the server has them |

`subAttributes`:

```json
"hydraulics": { "type": "Property", "value": "Hydraulics",
                "pressure": { "type": "Property", "value": 212.5, "unitCode": "BAR", "observedAt": "2026-10-17T10:00:00Z" },
                "oilTemp":  { "type": "Property", "value": 48.1,  "unitCode": "CEL", "observedAt": "2026-10-17T10:00:00Z" } }
```

`value`:

```json
"hydraulics": { "type": "JsonProperty", "json": { "Pressure": 212.5, "OilTemp": 48.1 } }
```

### 5.6 Changes

- **A value** - every Variable of the model is monitored. A change writes the entity it is in again,
  `observedAt` the change's SourceTimestamp.
- **The structure** - a model is read when the bridge connects, and again after a reconnect: new nodes
  appear then. A node that is gone is **left alone** in the broker: a server restarting must not wipe what
  was read, and the last values stay readable.
- **Writes** - none: an AssetChannel is read-only. A client may still PATCH such an attribute in the
  broker (nothing owns it), and the next value from the server replaces it.

## 6. Security

Without a `security` member the bridge connects with **SecurityPolicy None** - no signing, no encryption,
and anonymously unless `user` is given. For anything but a trusted network, use a secure mode:

```json
"server": {
  "url": "opc.tcp://press-17.example.com:4840",
  "security": {
    "mode": "SignAndEncrypt",
    "policy": "Basic256Sha256",
    "certificate": "/etc/coraine/opcua/client.pem",
    "privateKey": "/etc/coraine/opcua/client.key.pem",
    "applicationUri": "urn:example:coraine:opcua",
    "trustList": [ "/etc/coraine/opcua/server.der" ] },
  "user": { "username": "operator", "passwordFile": "/etc/coraine/opcua/password" }
}
```

| member | |
|---|---|
| `mode` | `None` (the default), `Sign`, `SignAndEncrypt` |
| `policy` | `Basic256Sha256` (the default), `Aes128_Sha256_RsaOaep`, `Aes256_Sha256_RsaPss` |
| `certificate`, `privateKey` | the bridge's own certificate and its key - DER or PEM |
| `applicationUri` | the URI in the certificate's subjectAltName - it must match, or servers refuse the client |
| `trustList` | the server certificates the bridge trusts (files) |
| `trustAny` | `true`: accept any server certificate - the bridge then cannot tell the server it means from one that pretends to be it, and the broker's log says so. An empty `trustList` without it: the bridge does not start |
| `user.username` | with `password`, or `passwordFile` (the file's first line - keeps the secret out of the configuration file) |

The bridge uses **exactly** the mode and policy given: if the server does not offer that endpoint, it does
not connect (section 7) - it never falls back to a weaker one.

### 6.1 The bridge's certificate

Any X.509 certificate with the application URI in its subjectAltName will do. With OpenSSL:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -days 365 -sha256 \
  -keyout client.key.pem -out client.pem -subj "/CN=coraine opcua bridge" \
  -addext "subjectAltName=URI:urn:example:coraine:opcua,DNS:$(hostname)" \
  -addext "keyUsage=digitalSignature,nonRepudiation,keyEncipherment,dataEncipherment" \
  -addext "extendedKeyUsage=clientAuth"
openssl x509 -in client.pem -outform DER -out client.der      # the DER copy, for the server
```

`client.pem` and `client.key.pem` go in the bridge's configuration (`certificate`, `privateKey`), and
`urn:example:coraine:opcua` is its `applicationUri`. These exact steps were used against the test server
(section 10).

### 6.2 Trust, both ways

- **The server trusts the bridge**: put `client.der` in the server's trusted certificates - its PKI
  "trusted" folder, or its configuration tool's "trust" action on the rejected certificate after a first
  attempt. Until then the server refuses the bridge.
- **The bridge trusts the server**: get the server's certificate (from its PKI "own" folder, or its
  administrator) and name the file in `trustList`. Until then the bridge refuses the server
  (`BadCertificateUntrusted`).

### 6.3 The user

`user` sends a username and password; the server decides what that user may read and write. A password is
never written to the broker's log. With mode `None` the log warns that the password is protected only if
the server's user token policy encrypts it.

## 7. Status and troubleshooting

Each Channel and AssetChannel says how it is doing:

```bash
curl localhost:1026/ngsi-ld/v1/channels
curl localhost:1026/ngsi-ld/v1/assetChannels
```

```json
{ "id": "urn:ngsi-ld:AssetChannel:opcua:ns=2;s=Press17", "type": "AssetChannel",
  "bridgeId": "urn:ngsi-ld:ContextBridge:opcua", "channelTarget": "ns=2;s=Press17",
  "assetEntities": "natural", "attributeLayout": "attributes",
  "status": "available",
  "modelsIn": 1, "valuesIn": 0, "valuesRefused": 0, "samplesRefused": 0, "entitiesWritten": 2 }
```

`status` is `available`, or `dormant` with a `statusReason`:

| `statusReason` | what it means | what to do |
|---|---|---|
| `the OPC UA server opc.tcp://... cannot be reached (BadConnectionRejected)` | nothing answers at that address (or `BadTimeout`, `BadNotConnected`, ...) | the URL, the server, the network, a firewall - the bridge retries every 2 s |
| `... refused the connection (BadUserAccessDenied)` | wrong username or password | `user` |
| `... refused the connection (BadCertificateUntrusted)` | the bridge does not trust the server's certificate - or the server not the bridge's | 6.2 |
| `... refused the connection (BadIdentityTokenRejected)` | no endpoint with this mode and policy, or the user token is not accepted on it | `mode`, `policy`, `user` - what the server offers |
| `the OPC UA server has no node 'ns=2;s=...' (BadNodeIdUnknown)` | the NodeId is not in the server's address space | the NodeId - namespace indexes differ between servers |
| `'...' is neither a Variable (a value) nor an Object (a model)` | a Method, an ObjectType, ... | name a Variable or an Object |
| `not an OPC UA NodeId (ns=<namespace>;i=<number>, ...)` | the endpoint is not a NodeId | the spelling |
| `this bridge takes no channelInfo keys` | the channel has `channelInfo` | remove it |

The counters: on a Channel `samplesIn` (values taken in) and `samplesOut` (writes); on an AssetChannel
`modelsIn` (models read), `valuesIn` (value changes written), `valuesRefused` (changes of nodes not in
the model), `entitiesWritten`. The broker's log (with `-v`) has the bridge's own lines, `opcua: ...`, and
open62541's warnings and errors, `opcua: open62541: ...`.

A secure-mode configuration that is incomplete - no certificate, no key, no application URI, an empty
trust list - stops the broker at start, and its log says which.

## 8. From c³

c³, the coraine Control Center:

- **Compile** - tick the OPC UA bridge; the build then carries `opcua.so`.
- **Operate** - a broker's **OPC UA** panel (native brokers: the container image has no OPC UA bridge):
  the server's URL, the model roots (an Object's NodeId a line - each one an AssetChannel with `entities:
  natural`), the security mode and policy, the certificate, key, application URI and trusted server
  certificates, a username and password. c³ writes the bridge configuration itself. The password is sent
  once, when the broker is started; c³ keeps it in the broker's directory, readable by you only, and
  never shows it again.

### Browsing the server: the inventory

To find the NodeIds to configure, ask the broker what the server has. With the broker's `admin` API
plugin loaded (`--apiPlugins admin`):

    curl 'localhost:1026/admin/bridges/opcua/inventory?from=ns%3D2%3Bs%3DPress17&depth=1'

- `from` - the NodeId to start at, URL-encoded (`ns=2;s=Press17` -> `ns%3D2%3Bs%3DPress17`); without it,
  the server's Objects folder.
- `depth` - how many levels below it (default 1, at most 5 in one request: ask again further down).

The answer is the node tree, in the same shape the bridge hands the broker for an AssetChannel - for the
test server's press, one level (abridged):

```json
{ "node": "object", "id": "ns=2;s=Press17", "name": "Press17",
  "typeName": "PressType", "typeDefinition": "ns=2;s=PressType", "semanticIds": [ "0173-1#01-AKJ975#017" ],
  "children": [
    { "node": "folder", "id": "ns=2;s=P17.Hydraulics", "name": "Hydraulics", "hasChildren": true },
    { "node": "variable", "id": "ns=2;s=P17.Mode", "name": "Mode",
      "dataType": "String", "writable": true, "value": "auto", "sourceTime": "2026-10-17T10:00:00.000Z" },
    { "node": "object", "id": "ns=2;s=P17.Unit1", "name": "Unit1", "typeName": "HydraulicUnitType",
      "hasChildren": true, "references": [ { "type": "HasEffect", "target": "ns=2;s=Press17" } ] },
    { "node": "method", "id": "ns=2;s=P17.Start", "name": "Start" } ] }
```

A Variable shows its DataType, whether the bridge's user may write it (`writable`), its unit and its
value - what decides whether it is a good Channel. `hasChildren` marks a node with more below it than the
depth asked for. An Object is a candidate AssetChannel root.

| Answer | When |
|---|---|
| 200 | the tree |
| 404 | no bridge `opcua` loaded, the server has no node `from` - or a broker older than the inventory, which has no such URL |
| 501 | a bridge older than the inventory (bridge ABI 14), or a broker built without it (`COR_FEATURE_BRIDGE_INVENTORY`) |
| 503 | the bridge has no connection to the server - with the reason (section 7) |

c³'s mapping page is to browse this way and write the configuration of sections 4 and 5 from what you
pick.

## 9. What it does not do yet

- **Methods** - an OPC UA Method makes nothing yet. They are to become services of their entity (NGSI-LD
  Service Execution): calling the service calls the method.
- **Writing to an AssetChannel** - its attributes are read-only towards the server (5.6).
- **Arrays and structured values** on a Write (section 4); a structured value (an ExtensionObject) is read
  as `null`.
- **A user certificate** (X.509 identity token), certificate folders instead of file lists, a certificate
  the bridge makes for itself.
- **Per-node settings** (`channelInfo`): one sampling interval (`samplingIntervalMs`) and one publishing
  interval (`publishingIntervalMs`) for all, in `server`.
- **More than one server** per broker.
- Events and alarms, historical access.

## 10. Testing it against your own server

What is worth trying, and what to send back:

1. **A Channel on a Variable you can change** - does the value arrive, with the server's time as
   `observedAt`? Change it on the server: does the entity follow?
2. **A Write** - PATCH the attribute: does the server get the value? Try a value of the wrong type: `400`,
   and nothing written?
3. **An AssetChannel on a real machine model**, `entities: "natural"`: are the entities the parts you
   would expect, their types and names readable, the units right? Try `attributeLayout` `subAttributes`
   and `value`, and a rule on a TypeDefinition.
4. **Security**: SignAndEncrypt with your server's policy, the certificate from 6.1, a user.
5. **Failures**: stop the server, then start it again - dormant with the reason, then available, the
   model read again? A wrong password, an untrusted certificate - is the reason clear?

Send back, for anything that surprised you: the `bridges.json` (without the password), the output of
`GET /ngsi-ld/v1/channels` and `/assetChannels`, the entities in question, and the broker's log (`-v`).

The test server in this repository is a quick way to see everything working before your own:

```bash
make server
mkfifo /tmp/ua.cmd
test/opcuaTestServer 14840 /tmp/ua.cmd /tmp/ua.writes &              # the press and the tank, no security
echo "set ns=2;s=P17.Pressure 2026-10-17T10:00:05Z 214" > /tmp/ua.cmd   # a value changes on the server
```

`test/opcuaTestServer --makeCerts <dir>` makes certificates and a password, and
`test/opcuaTestServer <port> <fifo> <writes> <dir>` runs it secure (SignAndEncrypt, user `operator`, the
password in `<dir>/password`).
