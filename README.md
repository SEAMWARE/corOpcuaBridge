# corOpcuaBridge

An **OPC UA bridge plugin** for the coraine NGSI-LD broker: `opcua.so`, a client of one OPC UA server
(through [open62541](https://www.open62541.org/)). It carries an OPC UA Variable's value into an entity
attribute and back, and an OPC UA Object - with everything it contains - into a graph of entities.

    OPC UA server <-> opcua.so <-> broker <-> Channel       <-> entity attribute   (a Variable)
                                          <-> AssetChannel  <-> entities           (an Object: a model)

**[The guide](doc/guide.md)** - what you configure and what you get, with real examples: Variables and
Objects, the mapping, security step by step, status and troubleshooting, testing against your own server.
This README is the reference.

## Requirements

- **open62541 1.4**, the distribution's: `apt-get install libopen62541-1.4-dev` (to build) or
  `libopen62541-1.4` (to run) - Ubuntu 26.04, Debian 13. Ubuntu 24.04 has no package, and no OPC UA
  bridge. open62541 is linked dynamically and is not part of this repository or its builds.
- **coraine 0.6**, with `COR_FEATURE_ASSET_CHANNELS` for AssetChannels (Channels need no feature).

## Configuration

One `--bridgeConfig` file serves every bridge, one member per alias. The `ngsild` section is the broker's -
it makes the Channels and AssetChannels; `server` is this plugin's.

```json
{ "opcua": { "server": { "url": "opc.tcp://press-17.acme.example:4840",
                         "publishingIntervalMs": 500, "samplingIntervalMs": 250 },
             "ngsild": {
               "topics": {
                 "ns=2;s=Tank1.Level":    { "entityId": "urn:ngsi-ld:Tank:T1", "entityType": "Tank", "attribute": "level" },
                 "ns=2;s=Tank1.Setpoint": { "entityId": "urn:ngsi-ld:Tank:T1", "entityType": "Tank", "attribute": "setpoint" } },
               "assetChannels": {
                 "ns=2;s=Press17": { "entities": "natural", "attributeLayout": "attributes" } } } } }
```

| `server` member | |
|---|---|
| `url` | the server, `opc.tcp://<host>:<port>` - mandatory |
| `publishingIntervalMs` | the subscription's publishing interval (default 500) |
| `samplingIntervalMs` | each monitored item's sampling interval (default 250) |
| `security`, `user` | the SecureChannel and the user - below; none: SecurityPolicy None, anonymous |

### Security

```json
"server": { "url": "opc.tcp://press-17.acme.example:4840",
            "security": { "mode": "SignAndEncrypt", "policy": "Basic256Sha256",
                          "certificate": "/etc/coraine/opcua/client.der", "privateKey": "/etc/coraine/opcua/client.key",
                          "applicationUri": "urn:example:coraine",
                          "trustList": [ "/etc/coraine/opcua/server.der" ] },
            "user": { "username": "operator", "passwordFile": "/etc/coraine/opcua/password" } }
```

| `security` member | |
|---|---|
| `mode` | `None` (the default), `Sign`, `SignAndEncrypt` - the client connects with this mode only |
| `policy` | `Basic256Sha256` (the default), `Aes128_Sha256_RsaOaep`, `Aes256_Sha256_RsaPss` - this policy only: no fallback to a weaker endpoint the server also offers |
| `certificate`, `privateKey` | the client's, DER or PEM - mandatory for `Sign` and `SignAndEncrypt` |
| `applicationUri` | the URI in the certificate's subjectAltName - mandatory with a certificate; a server refuses a client whose URI does not match |
| `trustList` | the server certificates the client trusts (files). Empty: the bridge does not start |
| `trustAny` | `true`: any server certificate is accepted, and the log says what that means - a server that pretends to be the one meant is not told apart |

`user` - `username` and `password`, or `passwordFile` (the password is the file's first line: it stays out
of the configuration file); none: anonymous. A password is never logged. With mode `None` the log warns
that the password is protected only if the server's user token policy encrypts it.

A refusal shows as each endpoint's `statusReason` - "the OPC UA server ... refused the connection
(BadUserAccessDenied)", "(BadCertificateUntrusted)" - apart from a server that cannot be reached at all.

An **endpoint** is a NodeId as OPC UA writes it (`ns=2;s=Press17`, `ns=1;i=1001`). What it carries is
the node's class:

- **A Variable** (a Channel, `topics`) - in: its value, and each change the server reports (a monitored
  item), the SourceTimestamp the attribute's `observedAt`; out: a value written to the attribute is a Write
  to the Variable, converted to its DataType (Boolean, the integer types within their range, Float,
  Double, String). A value that does not fit is refused before anything is stored - 400.
- **An Object** (an AssetChannel, `assetChannels`) - the object and what it contains, browsed
  (HasComponent, HasOrderedComponent, HasProperty, Organizes) and handed over as the neutral node tree of
  coraine's [AssetChannels](https://github.com/SEAMWARE/coraine/blob/main/doc/asset-channels.md): Objects
  (`object`, `folder` for a FolderType), Variables (`variable`: value, SourceTimestamp, EngineeringUnits as
  the UN/CEFACT code), Methods (`method`); a node's type name and TypeDefinition; its HasDictionaryEntry
  targets as its `semanticIds` (a string NodeId - an IRDI, a URI - as the string); its other forward
  references as `references`. Then every Variable of the model is monitored, and each change handed over as
  `{ "values": [ ... ] }`. Which nodes become entities, and how, is the broker's mapping.
  With `"channelDirection": "both"` (coraine's AssetChannel write-back) a client's write to an attribute
  made from one of the model's Variables is a Write to that Variable, converted to its DataType - read
  with the model, with its AccessLevel: a value that does not fit is refused (400), a Variable the server
  does not let be written answers 422, nothing stored either way.

A **method** is a service (coraine's [Service Execution](https://github.com/SEAMWARE/coraine/blob/main/doc/service-execution.md),
bridge ABI 11): a Service Registration whose endpoint is `opcua://<object NodeId>/<method NodeId>` is
executed by the bridge - the method called on the object (an OPC UA Call):

```json
{ "type": "ServiceRegistration", "endpoint": "opcua://ns=2;s=Press17/ns=2;s=P17.SetSpeed",
  "entities": [ { "id": "urn:ngsi-ld:PressType:Press17", "type": "PressType" } ],
  "serviceInformation": { "serviceName": "setSpeed" } }
```

- **The input** is an object, a member per input argument by its name (the method's `InputArguments`),
  each converted to the argument's DataType - a scalar, or an array for an argument of valueRank 1 or more:
  `{ "speed": 12.5, "ramp": true }`. A missing argument, a member that is no argument, a value that does
  not fit: the execution fails before anything is called. `id` and `type` are the entity's.
- **The output** is an object, each output argument by its name (`OutputArguments`): `{ "previous": 0 }`;
  a method without output arguments has none.
- **A failure** - the method's StatusCode (`BadInvalidState`), a method the server does not have
  (`BadMethodInvalid`), a server that cannot be reached - fails the execution, the ProblemDetails saying
  why (a synchronous service answers 502). A '/' inside a NodeId is written `%2F`.
- A Call cannot be cancelled. One asked for while the server cannot be reached fails at once - it is not
  run once the server is back.

`channelInfo` takes no keys yet: one is refused, and the Channel is dormant.

## Behaviour

- **One thread owns the client.** It connects, browses, subscribes, reads and writes; the broker's calls
  are queued to it, and none waits on the server.
- **A server that is not up** does not hold the broker's startup: every endpoint is dormant, its
  `statusReason` "the OPC UA server ... cannot be reached", until the client connects - it tries every 2 s.
  A lost connection is reconnected, and every endpoint set up again (a model is browsed again).
- **Why an endpoint is dormant** is on `GET /ngsi-ld/v1/channels` (and `/assetChannels`): not a NodeId,
  a node the server does not have, a node that is neither a Variable nor an Object, a server that cannot be
  reached (bridge ABI 12, `channelStatusIn`).
- **No echo.** The bridge's own Write comes back from the server as a change; it is not taken in as a
  sample.
- **A model** is cut at 10,000 nodes and at depth 16, and says so in the log.
- open62541's own warnings and errors go to the broker's log.

## Not yet

- A user certificate (X.509 identity token), certificate and trust-list folders, a certificate the
  bridge makes for itself.
- Registering a model's methods as services by itself (a Service Registration is made by hand), arrays
  and structured values on a Write, structured method arguments, `channelInfo` keys (a different sampling
  interval per endpoint).

## Building

    make                    # opcua.so, debug (traces compiled in)
    make BUILD=release
    make contract           # does src/opcuaRegister.c still satisfy corBridge's BridgeDriver.h?
    make server             # test/opcuaTestServer - the tests' OPC UA server
    make install            # to /opt/seamware/plugins/bridge (PLUGIN_DIR=...)

The cor-lib headers are found in `COR_LIBS` (default `..`: the sibling repos, as every cor repo builds), or
the coraine-dev package's source. `OPEN62541=<dir>` builds against open62541 unpacked in `<dir>`
(`<dir>/usr/include`, `<dir>/usr/lib/x86_64-linux-gnu`) instead of the installed one.

## Testing

    make test

Starts the tests' OPC UA server (`test/opcuaTestServer.c`: a press and a tank, its SourceTimestamps fixed,
its values changed and its Writes recorded by the tests; secure, with certificates and a password it makes
for the run, for the security test) on port 14840, and `coraine` (from `PATH`) on port
19026 with ramDB, loading `./opcua.so` by path, and runs `test/funcTests/cases/*.test` with corTest.
`OPCUA_BROKER`, `OPCUA_PORT`, `OPCUA_SERVER`, `OPCUA_SERVER_PORT` and `OPCUA_TEST_DIR` override; the logs
are in `/tmp/opcua-bridge-test/`.

## Running

    coraine --bridges opcua --bridgeConfig bridges.json            # installed in <plugin dir>/bridge
    coraine --bridges $PWD/opcua.so --bridgeConfig bridges.json    # a path: used as it is

## Further reading

- [corBridge](https://github.com/SEAMWARE/corBridge) - the bridge contract
- coraine: [AssetChannels](https://github.com/SEAMWARE/coraine/blob/main/doc/asset-channels.md),
  [plugin architecture](https://github.com/SEAMWARE/coraine/blob/main/doc/plugin-architecture.md)
- [corBridgeTemplate](https://github.com/SEAMWARE/corBridgeTemplate) - where this bridge started

---

Copyright 2026 Seamware · Apache-2.0. open62541 is MPL-2.0, linked dynamically, not distributed here.
