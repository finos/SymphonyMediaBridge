#API description
This document lists SMB supported requests and responses in a pseudo request example form. Types are either specified in the document or implicit from the example value.

##Allocate conference

Allocate a new conference resource, responds with conference id for the allocated resource.

Normally, SMB will use a shared UDP port for all conferences that all clients connect to. It will then de-multiplex using ICE ufrag and source IP:port. By setting global-port to false, conference will allocate its own port for the conference. All clients will connect to that port instead, for this conference. If global-port is not set, it will default to true.

```json
POST /conferences/
{
    "last-n": Integer,
    "global-port": Boolean
}
```

```json
200 OK
{
    "id": String
}
```

##Allocate endpoint

Allocate a new endpoint with endpoint id {endpointId} in the conference {conferenceId}. Responds with information used to construct an SDP offer.

The idleTimeout is optional timeout in seconds. If no packets have been received during this period, the endpoint will be removed.

Relay-type is how SMB will relay the incoming packets. For audio it may be mixed into 1 ssrc. It may be forwarded from the sender by preserving the original ssrc (forwarder). The preferred mode is ssrc-rewrite. SMB will then relay the packets by mapping the ssrcs to a fixed set of outbound ssrcs. This allows fewer ssrcs to be negotiated and will cause fewer streams to be forwarded. It also prevents re-negotiations that would otherwise happen when people join and leave the conference as ssrcs would be added and removed at that time. Clients are also likely to be limited in how many audio sinks and video sinks they can handle. With ssrc-rewrite the conference can be very large.

```json
POST /conferences/{conferenceId}/{endpointId}
{
    "action": "allocate",
    "bundle-transport": {
        "ice-controlling": Boolean,
        "ice": Boolean,
        "dtls": Boolean
        },
    "audio": {
        "relay-type": ["forwarder | mixed | ssrc-rewrite"]
        },
    "video": {
        "relay-type": "forwarder | ssrc-rewrite"
        },
    "data": {},
    "idleTimeout": <90>
}
```

```json
200 OK
{
    "bundle-transport": {
        "dtls": {
            "setup": "actpass",
            "type": "sha-256",
            "hash": String
        },
        "ice": {
            "ufrag": String,
            "pwd": String,
            "candidates": [
                {
                    "foundation": String,
                    "component": Integer,
                    "protocol": String,
                    "priority": Long,
                    "ip": String,
                    "port": Integer,
                    "type": String,
                    "generation": Integer,
                    "network": Integer
                }
            ]
        }
    },
    "audio": {
        "payload-type": {
            "id": 111,
            "parameters": {
                "minptime": "10",
                "useinbandfec": "1"
            },
            "rtcp-fbs": [],
            "name": "opus",
            "clockrate": 48000,
            "channels": 2
        },
        "ssrcs": [int, int],
        "rtp-hdrexts": [
            {
                "id": 1,
                "uri": "urn:ietf:params:rtp-hdrext:ssrc-audio-level"
            },
            {
                "id": 3,
                "uri": "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"
            }
        ]
    },
    "video": {
        "payload-types": [
            {
                "id": 100,
                "parameters": {},
                "rtcp-fbs": [
                    {
                        "type": "goog-remb"
                    },
                    {
                        "type": "nack"
                    },
                    {
                        "type": "nack",
                        "subtype": "pli"
                    }
                ],
                "name": "VP8",
                "clockrate": 90000
            },
            {
                "id": 96,
                "parameters": {
                    "apt": "100"
                },
                "rtcp-fbs": [],
                "name": "rtx",
                "clockrate": 90000
            }
        ],
        "streams": [
            {
                "sources": [
                    {
                        "main": Integer,
                        "feedback": Integer
                    }
                ],
                "id": "Nuxr31v1qPvNIGkNDBvEar2FX298cQHO6jmi",
                "content": "video|slides|local"
            }
        ],
        "rtp-hdrexts": [
            {
                "id": 3,
                "uri": "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"
            },
            {
                "id": 4,
                "uri": "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"
            }
        ]
    },
    "data": {
        "port": 5000
    }
}
```

##Configure endpoint
Configure endpoint id {endpointId} in the conference {conferenceId} based on information from the initial SDP answer from the endpoint.

```json
POST /conferences/{conferenceId}/{endpointId}
{
    "action": "configure",
    "bundle-transport": {
        "dtls": {
            "setup": [
                "active | passive"
            ],
            "type": String,
            "hash": String
        },
        "ice": {
            "ufrag": String,
            "pwd": String,
            "candidates": [
                {
                    "foundation": String,
                    "component": Integer,
                    "protocol": String,
                    "priority": Long,
                    "ip": String,
                    "port": Integer,
                    "type": String,
                    "generation": Integer,
                    "network": Integer
                }
            ]
        }
    },
    "audio": {
        "payload-type": {
            "id": 111,
            "parameters": {
                "minptime": "10",
                "useinbandfec": "1"
            },
            "rtcp-fbs": [],
            "name": "opus",
            "clockrate": 48000,
            "channels": 2
        },
        "ssrcs": [
    Integer
        ],
        "rtp-hdrexts": [
            {
                "id": 1,
                "uri": "urn:ietf:params:rtp-hdrext:ssrc-audio-level"
            },
            {
                "id": 3,
                "uri": "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"
            }
        ]
    },
    "video": {
        "payload-types": [
            {
                "id": 100,
                "parameters": {},
                "rtcp-fbs": [
                    {
                        "type": "goog-remb"
                    },
                    {
                        "type": "nack"
                    },
                    {
                        "type": "nack",
                        "subtype": "pli"
                    }
                ],
                "name": "VP8",
                "clockrate": 90000
            },
            {
                "id": 96,
                "parameters": {
                    "apt": "100"
                },
                "rtcp-fbs": [],
                "name": "rtx",
                "clockrate": 90000
            }
        ],
        "streams": [
            {
                "sources": [
                    {
                        "main": 1234,
                        "feedback": 4321
                    }
                ],
                "id": "Nuxr31v1qPvNIGkNDBvEar2FX298cQHO6jmi",
                "content": "video|slides|local"
            }
        ],
        "rtp-hdrexts": [
            {
                "id": 3,
                "uri": "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"
            },
            {
                "id": 4,
                "uri": "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"
            }
        ]
    },
    "data": {
        "port": 5000
    },
    "neighbours": {
        "groups": [
            "group1",
            "12345"
        ]
    }
}
```

```json
200 OK
```

##Reconfigure endpoint

Reconfigure endpoint id {endpointId} in the conference {conferenceId} adding or removing ssrcs sent from that endpoint.

```json
POST /conferences/{conferenceId}/{endpointId}
{
    "action": "reconfigure",
    "audio": {
        "ssrcs": [
    String
        ]
    },
    "video": {
        "streams": [
            {
                "sources": [
                    {
                        "main": 1234,
                        "feedback": 4321
                    }
                ],
                "id": "Nuxr31v1qPvNIGkNDBvEar2FX298cQHO6jmi",
                "content": "video|slides|local"
            }
        ],
    }
}
```

```json
200 OK
```

##Expire endpoint
Preferably, post a DELETE request for the end point id.

Delete an endpoint id {endpointId} in the conference {conferenceId}.

```json
DELETE /conferences/{conferenceId}/{endpointId}
```

```json
200 OK
```

Deprecated API

```json
POST /conferences/{conferenceId}/{endpointId}
{
"action": "expire"
}
```

```json
200 OK
```

##Allocate barbell leg
A 2-way barbell leg can be setup between two SMBs. This can be used to create larger conference or multi location conference to facilitate lower delay on average. There is an allocation step, and a configuration step in the same manner as for channels. There is a small difference between endpoint allocation when it comes to video. An endpoint will only receive a selected video stream per participant and an rtc feedback stream in addition to that. The barbell endpoint can receive multicast and RTX for the participants. The dominant speaker may send low, medium and high res video streams. The others may send medium and low resolution to allow the receiving SMB to select lower resolution in case the clients' downlinks are limited.

Allocate a {barbell port} in the conference {conferenceId}.

```json
POST /barbell/{conferenceId}/{barbellId}
{
    "action": "allocate",
    "bundle-transport": {
        "ice-controlling": Boolean
    }
}
```

```json
200 OK
{
    "bundle-transport": {
        "dtls": {
            "setup": "actpass",
            "type": "sha-256",
            "hash": String
        },
        "ice": {
            "ufrag": String,
            "pwd": String,
            "candidates": [
                {
                    "foundation": String,
                    "component": Integer,
                    "protocol": String,
                    "priority": Long,
                    "ip": String,
                    "port": Integer,
                    "type": String,
                    "generation": Integer,
                    "network": Integer
                }
            ]
        }
    },
    "audio": {
        "payload-type": {
            "id": 111,
            "parameters": {
                "minptime": "10",
                "useinbandfec": "1"
            },
            "rtcp-fbs": [],
            "name": "opus",
            "clockrate": 48000,
            "channels": 2
        },
        "ssrcs": [
    Integer
        ],
        "rtp-hdrexts": [
            {
                "id": 1,
                "uri": "urn:ietf:params:rtp-hdrext:ssrc-audio-level"
            },
            {
                "id": 3,
                "uri": "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"
            }
        ]
    },
    "video": {
        "payload-types": [
            {
                "id": 100,
                "parameters": {},
                "rtcp-fbs": [
                    {
                        "type": "goog-remb"
                    },
                    {
                        "type": "nack"
                    },
                    {
                        "type": "nack",
                        "subtype": "pli"
                    }
                ],
                "name": "VP8",
                "clockrate": 90000
            },
            {
                "id": 96,
                "parameters": {
                    "apt": "100"
                },
                "rtcp-fbs": [],
                "name": "rtx",
                "clockrate": 90000
            }
        ],
        "streams": [
            {
                "sources": [
                    {
                        "main": 1234,
                        "feedback": 4321
                    }
                ],
                "id": "Nuxr31v1qPvNIGkNDBvEar2FX298cQHO6jmi",
                "content": "video|slides|local"
            }
        ],
        "rtp-hdrexts": [
            {
                "id": 3,
                "uri": "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"
            },
            {
                "id": 4,
                "uri": "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"
            }
        ]
    },
    "data": {
        "port": 5000
    }
}
```

##Remove Barbell
Remove {barbellId} from the conference {conferenceId}.

```json
DELETE /barbell/{conferenceId}/{barbellId}
```

```json
200 OK
```

##Configure barbell

Configure barbell {barbellId} in the conference {conferenceId} based on information from the initial SDP answer from the barbell allocation.

```json
POST /barbell/{conferenceId}/{barbellId}
{
    "action": "configure",
    "bundle-transport": {
        "dtls": {
            "setup": [
                "active | passive"
            ],
            "type": String,
            "hash": String
        },
        "ice": {
            "ufrag": String,
            "pwd": String,
            "candidates": [
                {
                    "foundation": String,
                    "component": Integer,
                    "protocol": String,
                    "priority": Long,
                    "ip": String,
                    "port": Integer,
                    "type": String,
                    "generation": Integer,
                    "network": Integer
                }
            ]
        }
    },
    "audio": {
        "payload-type": {
            "id": 111,
            "parameters": {
                "minptime": "10",
                "useinbandfec": "1"
            },
            "rtcp-fbs": [],
            "name": "opus",
            "clockrate": 48000,
            "channels": 2
        },
        "ssrcs": [
    Integer
        ],
        "rtp-hdrexts": [
            {
                "id": 1,
                "uri": "urn:ietf:params:rtp-hdrext:ssrc-audio-level"
            },
            {
                "id": 3,
                "uri": "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"
            }
        ]
    },
    "video": {
        "payload-types": [
            {
                "id": 100,
                "parameters": {},
                "rtcp-fbs": [
                    {
                        "type": "goog-remb"
                    },
                    {
                        "type": "nack"
                    },
                    {
                        "type": "nack",
                        "subtype": "pli"
                    }
                ],
                "name": "VP8",
                "clockrate": 90000
            },
            {
                "id": 96,
                "parameters": {
                    "apt": "100"
                },
                "rtcp-fbs": [],
                "name": "rtx",
                "clockrate": 90000
            }
        ],
        "streams": [
            {
                "sources": [
                    {
                        "main": 1234,
                        "feedback": 4321
                    }
                ],
                "id": "Nuxr31v1qPvNIGkNDBvEar2FX298cQHO6jmi",
                "content": "video|slides|local"
            }
        ],
        "rtp-hdrexts": [
            {
                "id": 3,
                "uri": "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"
            },
            {
                "id": 4,
                "uri": "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"
            }
        ]
    },
    "data": {
        "port": 5000
    }
}
```

```json
200 OK
```

##Get Conference Info
###Get Conference List
You can use the following url to get a json array of currently instantiated conferenceIds.

```json
GET /conferences
```

```json
200 OK
["9465082961911005170","9465082961917344970"]
```

###Get Users List
Use this URL to retrieve a full list of conference ids and user ids in all conferences.

```json
GET /conferences?brief
```

```json
200 OK
[
    {
        "id": "9465082961911005170",
        "usercount": 3,
        "users": [
            "fc2bdae4-d490-5613-6055-00009dbc0e97",
            "af3928b1-bfd5-3f7a-7e57-000011448e16",
            "296098e6-f455-978b-283a-0000e806f078"
        ]
    }
]
```

###Get Detailed info
Retrieves the list of participants' endpoints in the conference {conferenceId}. Responds with basic information for each endpoint, such as: presense of audio and video streams, "Active Speaker" flag, use of recording, whether bundled transport is in use, ICE and DTLS status.

```json
GET /conferences/{conferenceId}
```

```json
200 OK
[
    {
        "dtlsState": string ,
        "iceState": string ,
        "id": string ,
        "isActiveTalker": bool,
        "isDominantSpeaker": bool,
        "ActiveTalker": optional, present only when 'isActiveTalker' is 'true'
    {
            "noiseLevel": integer
    "ptt": bool
    "score": integer
        }
    },
    {
    ...
    },
    ...
]
```

_DTLS States_

-   "IDLE"
-   "READY"
-   "CONNECTING"
-   "CONNECTED"
-   "FAILED"

_ICE States_

-   "IDLE"
-   "GATHERING"
-   "READY"
-   "CONNECTING"
-   "CONNECTED"
-   "FAILED"

##ICE probe endpoint
ICE can be used to assess RTT to a mediabridge. For this purpose, there is a static ICE endpoint that can be used to measure RTT but cannot be used to establish media session. Use the following endpoint to get the info needed to setup such an ICE connection.

```json
GET /ice-candidates
```

```json
200 OK
{
    "candidates": [
        {
            "component": 1,
            "foundation": "870038700640",
            "generation": 0,
            "ip": "10.0.1.90",
            "network": 1,
            "port": 10000,
            "priority": 142541055,
            "protocol": "udp",
            "type": "host"
        },
        {
            "component": 1,
            "foundation": "535128690992",
            "generation": 0,
            "ip": "82.29.139.91",
            "network": 1,
            "port": 10000,
            "priority": 142540799,
            "protocol": "udp",
            "type": "srflx"
        }
    ],
    "pwd": "QKvUzTxXHaa4RqzS0QBLFGoJ",
    "ufrag": "y2uBVS3RATC4Sd"
}
```
