# API description

This document lists SMB supported requests and responses in a pseudo request example form. Types are either specified in the document or implicit from the example value.

## Allocate conference

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

## Allocate endpoint with bundled transport

Allocate a new endpoint with endpoint id {endpointId} in the conference {conferenceId}. Responds with information used to construct an SDP offer.

The idleTimeout is optional timeout in seconds. If no packets have been received during this period, the endpoint will be removed.

Relay-type is how SMB will relay the incoming packets. For audio it may be mixed into 1 ssrc. It may be forwarded from the sender by preserving the original ssrc (forwarder). The preferred mode is ssrc-rewrite. SMB will then relay the packets by mapping the ssrcs to a fixed set of outbound ssrcs. This allows fewer ssrcs to be negotiated and will cause fewer streams to be forwarded. It also prevents re-negotiations that would otherwise happen when people join and leave the conference as ssrcs would be added and removed at that time. Clients are also likely to be limited in how many audio sinks and video sinks they can handle. With ssrc-rewrite the conference can be very large.

**Note** that both DTLS and SDES can be enabled in the allocate request. DTLS and SDES will be prepared and put in offer. The configure request will decide which SRTP mode will be used. DTLS is a requirement for data channel as SCTP is enveloped in DTLS messages.
**Note** that ICE is mandatory for bundled transport.

```json
POST /conferences/{conferenceId}/{endpointId}
{
    "action": "allocate",
    "bundle-transport": {
        "ice-controlling": Boolean,
        "ice": true,
        "dtls": boolean,
        "sdes": boolean
        },
    "audio": {
        "relay-type": ["forwarder | mixed | ssrc-rewrite"]
        },
    "video": {
        "relay-type": "forwarder | ssrc-rewrite"
        },
    "data": {},
    "idleTimeout": 90 // seconds
}
```

```json
200 OK
{
    "bundle-transport": {
        "dtls": {
            "setup": "actpass",
            "type": "sha-256",
            "hash": "8F:C2:B8:3F:07:53:0C:F5:07:EF:EC:EB:93:DF:4E:7A:1B:E1:11:A8:A9:7B:9F:EE:86:EE:BD:05:77:83:CD:D2"
        },
        "sdes":[ // omitted if dtls is used
            {
                "profile": "AES_128_CM_HMAC_SHA1_80",
                "key": "SGVsbG8gYmFzZTY0IHdvcmxkIQ=="
            },
            {
                "profile": "AES_256_CM_HMAC_SHA1_80",
                "key" : "SGVsbG8gYmFzZTY0IHdvcmxkIQ=="
            }],
        "ice": {
            "ufrag": "wX4aN8AyMUVadg",
            "pwd": "4cLWhgmLHtYZgncvuopUh+3r",
            "candidates": [
                {
                    "foundation": "716080445600",
                    "component": 1,
                    "protocol": "udp",
                    "priority": 142541055,
                    "ip": "35.240.205.93",
                    "port": 10000,
                    "type": "host",
                    "generation": 0,
                    "network": 1
                }
            ]
        }
    },
    "audio": {
        "payload-types": [
            {
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
            {
                "id": 110,
                "parameters": {},
                "rtcp-fbs": [],
                "name": "telephone-events",
                "clockrate": 48000
            }
        ],
        "ssrcs": [1234, 355667],
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
                "content": "local",
                "sources": [
                    {
                        "main": 4279327125
                    }
                ]
            },
            {
                "content": "video|slides",
                "sources": [
                    {
                        "feedback": 1908157291,
                        "main": 827879182
                    }
                ]
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

_SRTP profiles_

```
AES_128_CM_HMAC_SHA1_80
AES_128_CM_HMAC_SHA1_32
AES_192_CM_HMAC_SHA1_80
AES_192_CM_HMAC_SHA1_32
AES_256_CM_HMAC_SHA1_80
AES_256_CM_HMAC_SHA1_32
AEAD_AES_128_GCM
AEAD_AES_256_GCM
```

### Allocate endpoint without bundle transport

You can allocate and endpoint with separate transport for audio and video. This means there will be multiple ports used and any ICE and DTLS will be established independently.
ICE is optional and if disabled, a fixed port will be allocated and configured public IP will be presented.
DTLS and SDES are also optional and thus the encryption can be disabled altogether

```json
POST /conferences/{conferenceId}/{endpointId}
{
    "action": "allocate",
    "audio": {
        "relay-type": [
            "forwarder | mixed | ssrc-rewrite"
        ],
        "transport": {
            "ice": false,
            "dtls": false,
            "sdes": true
        },
    },
    "data": {},
    "idleTimeout": <90> // seconds
}
```

```json
200 OK
{
    "audio": {
        "payload-types": [
            {
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
            {
                "id": 110,
                "parameters": {},
                "rtcp-fbs": [],
                "name": "telephone-events",
                "clockrate": 48000
            }
        ],
        "ssrcs": [122312, 44634783],
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
        "transport": {
            "sdes": [
                {
                    "profile": "AES_128_CM_HMAC_SHA1_80",
                    "key": "SGVsbG8gYmFzZTY0IHdvcmxkIQ=="
                },
                {
                    "profile": "AES_256_CM_HMAC_SHA1_80",
                    "key": "SGVsbG8gYmFzZTY0IHdvcmxkIQ=="
                }
            ],
            "connection":{
                "port":32467,
                "ip":"203.14.54.111"
            }
        },
    }
}
```

## Configure endpoint

Configure endpoint id {endpointId} in the conference {conferenceId} based on information from the initial SDP answer from the endpoint.

```json
PUT /conferences/{conferenceId}/{endpointId}
{
    "action": "configure",
    "bundle-transport": {
        "dtls": {
            "setup": [
                "active | passive"
            ],
            "type": "sha-256 | sha1",
            "hash": "8F:C2:B8:3F:07:53:0C:F5:07:EF:EC:EB:93:DF:4E:7A:1B:E1:11:A8:A9:7B:9F:EE:86:EE:BD:05:77:83:CD:D2"
        },
        "sdes":{ // omit if dtls is used
            "profile": "AES_128_CM_HMAC_SHA1_80",
            "key": "SGVsbG8gYmFzZTY0IHdvcmxkIQ=="
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
        "payload-types": [
            {
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
            {
                "id": 110,
                "parameters": {},
                "rtcp-fbs": [],
                "name": "telephone-events",
                "clockrate": 48000
            }
        ],
        "ssrcs": [Integer, ...],
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
{
    // same body as for allocate
}
```

## Reconfigure endpoint

Reconfigure endpoint id {endpointId} in the conference {conferenceId} adding or removing ssrcs sent from that endpoint.

```json
PUT /conferences/{conferenceId}/{endpointId}
{
    "action": "reconfigure",
    "audio": {
        "ssrcs": [Integer...]
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

## Remove endpoint

Delete an endpoint id {endpointId} in the conference {conferenceId}.

```json
DELETE /conferences/{conferenceId}/{endpointId}
```

```json
200 OK
```

_Deprecated API_

```json
POST /conferences/{conferenceId}/{endpointId}
{
"action": "expire"
}
```

```json
200 OK
```

## Allocate barbell leg

A 2-way barbell leg can be setup between two SMBs. This can be used to create larger conference or multi location conference to facilitate lower delay on average. There is an allocation step, and a configuration step in the same manner as for channels. There is a small difference between endpoint allocation when it comes to video. An endpoint will only receive a selected video stream per participant and an rtc feedback stream in addition to that. The barbell endpoint can receive multicast and RTX for the participants. The dominant speaker may send low, medium and high res video streams. The others may send medium and low resolution to allow the receiving SMB to select lower resolution in case the clients' downlinks are limited.

**Barbells have mandatory DTLS based SRTP**

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
        "ssrcs": [Integer, ...],
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

## Remove Barbell

Remove {barbellId} from the conference {conferenceId}.

```json
DELETE /barbell/{conferenceId}/{barbellId}
```

```json
200 OK
```

## Configure barbell

Configure barbell {barbellId} in the conference {conferenceId} based on information from the initial SDP answer from the barbell allocation.

```json
PUT /barbell/{conferenceId}/{barbellId}
{
    "action": "configure",
    "bundle-transport": {
        "dtls": {
            "setup": "active | passive",
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
        "ssrcs": [Integer,...],
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

## Monitor Conferences

### Get Conference List

You can use the following url to get a json array of currently instantiated conferenceIds.

```json
GET /conferences
```

```json
200 OK
["9465082961911005170","9465082961917344970"]
```

### Get Users List

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

### Get Detailed info

Retrieves the list of participants' endpoints in the conference {conferenceId}. Responds with basic information for each endpoint, such as: presence of audio and video streams, "Active Speaker" flag, use of recording, whether bundled transport is in use, ICE and DTLS status.

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

### Retrieve Metrics

SMB produces useful metrics about the CPU and network load. Bit rates are in kbps. Below is a list of a few selected metrics that need more detailed description.

-   **inbound_audio_streams** is number of streams with apparent RTP activity.
-   **inbound_video_streams** is number of streams with apparent activity. Note that clients may send 3 streams each to SMB.
-   **bwe_download_hist** is histogram of number of calls that falls into estimated bandwidth buckets for the clients´ bandwidth when sending to SMB. The buckets are: [125, 250, 500, 1000, 2000, 4000, 8000, 16000, 32000, >32000] kbps.
-   **loss_download_hist** is histogram for number of calls with certain average loss rate when sending to SMB. The buckets are [0,1,2,4,8, 100] percent.
-   **loss_upload_hist** histogram works the same but is based on loss reports from the clients.
-   **rtt_download_hist** is a histogram for number of calls with specific RTT. The buckets are: [0.1, 0.2, 0.4, 0.8, 1.6, >1.6] seconds.
-   **pacing_queue** is a queue used to pace video packets to adapt rate to the client`s receive bandwidth. This avoids choking the network and packets can be dropped in SMB instead of causing high latency towards client. If this runs high it means clients have network trouble and video will not be of good quality.
-   **rtx_pacing_queue** is a parallell pacing queue that allows video RTX requests to be prioritized.

```json
GET /stats
```

```json
{
    "audiochannels": 0,
    "bit_rate_download": 0,
    "bit_rate_upload": 0,
    "bwe_download_hist": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    "conferences": 0,
    "cpu_engine": 0.0,
    "cpu_manager": 0.0,
    "cpu_rtce": 0.0,
    "cpu_usage": 0.006067961165048544,
    "cpu_workers": 0.0027739251040221915,
    "current_timestamp": 66218305,
    "engine_slips": 1,
    "http_tcp_connections": 1,
    "inbound_audio_ext_streams": 0,
    "inbound_audio_streams": 0,
    "inbound_video_streams": 0,
    "job_queue": 0,
    "largestConference": 0,
    "loss_download": 0.0,
    "loss_download_hist": [0, 0, 0, 0, 0, 0],
    "loss_upload": 0.0,
    "loss_upload_hist": [0, 0, 0, 0, 0, 0],
    "opus_decode_packet_rate": 0.0,
    "outbound_audio_streams": 0,
    "outbound_video_streams": 0,
    "pacing_queue": 0,
    "packet_rate_download": 0,
    "packet_rate_upload": 0,
    "participants": 0,
    "receive_pool": 32768,
    "rtc_tcp4_connections": 0,
    "rtc_tcp6_connections": 0,
    "rtt_download_hist": [0, 0, 0, 0, 0, 0],
    "rtx_pacing_queue": 0,
    "send_pool": 131072,
    "shared_udp_end_drops": 0,
    "shared_udp_receive_rate": 0,
    "shared_udp_send_queue": 0,
    "shared_udp_send_rate": 0,
    "threads": 20,
    "total_memory": 1622616,
    "total_tcp_connections": 1,
    "total_udp_connections": 4,
    "used_memory": 336120,
    "videochannels": 0
}
```

## Detailed Barbell Metrics

Barbell Metrics allows live monitoring and troubleshooting of barbell connections.

Detailed barbell Metrics are:

-   gathered on-demand when the request is being made, so should **not** be used frequently.
-   non-cumulative, but momentary - it reflects the last known metrics for the active streams collected in the last second or so.
-   approximate window of stats collecting in seconds can be estimated using `125 * bitrateKbps * / octets` formula.

### Get detailed Barbell Metrics for all conferences on the SMB

```json
GET /stats/barbell
```

Returns:

-   see [Barbell Metrics Response](#Barbell-Metrics-Response);
-   empty JSON, if there are no conferences;

### Get detailed Barbell Metrics for specific conference

```json
GET /stats/barbell/<conference ID>
```

Returns:

-   see [Barbell Metrics Response](#Barbell-Metrics-Response);
-   HTTP 404 if the conference is not found.

### Barbell Metrics Response

```
{
    <Conference ID 1>: {
        <Barbell ID 1>: {
            "audio": {
                "inbound": {
                    "activeStreamCount": <number>,
                    "bitrateKbps": <number>,
                    "lostPackets": <number>,
                    "octets": <number>,
                    "packets": <number>,
                    "packetsPerSecond": <number>
                },
                "outbound": {
                    ...
                }
            }
            "video": {
                ...
            }
        },
        <Barbell ID 2> : {
            ...
        }
    },
    <Conference ID 2>: {
        ...
    }
    ...
}
```

### Example of Barbell Metrics Response

Here `13066525621269710799` is the `Conference ID`, and `test_barbell` is `Barbell ID`.

```json
{
    "13066525621269710799": {
        "test_barbell": {
            "audio": {
                "inbound": {
                    "activeStreamCount": 2,
                    "bitrateKbps": 224,
                    "lostPackets": 0,
                    "octets": 28200,
                    "packets": 100,
                    "packetsPerSecond": 100
                },
                "outbound": {
                    "activeStreamCount": 1,
                    "bitrateKbps": 109,
                    "lostPackets": 0,
                    "octets": 13732,
                    "packets": 51,
                    "packetsPerSecond": 50
                }
            },
            "video": {
                "inbound": {
                    "activeStreamCount": 6,
                    "bitrateKbps": 6217,
                    "lostPackets": 0,
                    "octets": 777515,
                    "packets": 649,
                    "packetsPerSecond": 649
                },
                "outbound": {
                    "activeStreamCount": 3,
                    "bitrateKbps": 3068,
                    "lostPackets": 0,
                    "octets": 385783,
                    "packets": 330,
                    "packetsPerSecond": 323
                }
            }
        }
    }
}
```

## ICE probe endpoint

ICE can be used to assess RTT to a mediabridge. For this purpose, there is a static ICE endpoint that can be used to measure RTT but cannot be used to establish media sessions. Use the following endpoint to get the info needed to setup such an ICE connection.

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
