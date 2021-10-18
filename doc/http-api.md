# Allocate conference
<table>
    <tr>
        <td>Description</td>
        <td>
            <p>Allocate a new conference resource, responds with conference id for the allocated resource.</p>
        </td>
    </tr>
    <tr>
        <td>Resource</td>
        <td>
            <p>POST /conferences/</p>
        </td>
    </tr>
    <tr>
        <td>Request body</td>
        <td>
            <pre>
{
    "last-n": Integer
}
            </pre>
        </td>
    </tr>
    <tr>
        <td>Response</td>
        <td>
            200 OK
            <pre>
{
    "id": String
}
            </pre>
        </td>
    </tr>    
</table>

# Allocate endpoint

<table>
    <tr>
        <td>Description</td>
        <td>
            <p>Allocate a new endpoint with endpoint id {endpointId} in the conference {conferenceId}. Responds with information used to construct an SDP offer.</p>
        </td>
    </tr>
    <tr>
        <td>Resource</td>
        <td>
            <p>POST /conferences/{conferenceId}/{endpointId}</p>
        </td>
    </tr>
    <tr>
        <td>Request body</td>
        <td>
            <pre>
{
    "action": "allocate",
    "bundle-transport": {
      "ice-controlling": true,
      "ice": true,
      "dtls": true
    },
    "audio": {
      "relay-type": "forwarder"
    },
    "video": {
      "relay-type": "forwarder"
    },
    "data": {
    }
}
            </pre>
        </td>
    </tr>
    <tr>
        <td>Response</td>
        <td>
            200 OK
            <pre>
{
  "bundle-transport": {
    "dtls": {
      "setup": "active",
      "type": "sha-256",
      "hash": "5F:E7:53:B4:7E:51:DF:23:5C:60:5D:B5:D7:4F:83:72:25:3D:39:2E:47:A5:70:39:D9:8A:B4:61:16:84:5B:F4"
    },
    "ice": {
      "ufrag": "MaUF",
      "pwd": "QHIw+ToRDbEREVmY7xoQKH+e",
      "candidates": [
        {
          "foundation": "4258379142",
          "component": 1,
          "protocol": "udp",
          "priority": 2122260223,
          "ip": "192.168.1.250",
          "port": 52037,
          "type": "host",
          "generation": 0,
          "network": 2
        },
        {
          "foundation": "1162850324",
          "component": 1,
          "protocol": "udp",
          "priority": 2122194687,
          "ip": "192.168.1.235",
          "port": 58625,
          "type": "host",
          "generation": 0,
          "network": 1
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
      "clockrate": "48000",
      "channels": "2"
    },
    "ssrcs": [
      2981946593
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
        "clockrate": "90000"
      },
      {
        "id": 96,
        "parameters": {
          "apt": "100"
        },
        "rtcp-fbs": [],
        "name": "rtx",
        "clockrate": "90000"
      }
    ],
    "ssrcs": [
      2321667837
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
            </pre>
        </td>
    </tr>    
</table>

# Configure endpoint

<table>
    <tr>
        <td>Description</td>
        <td>
            <p>Configure endpoint id {endpointId} in the conference {conferenceId} based on information from the initial SDP answer from the endpoint.</p>
        </td>
    </tr>
    <tr>
        <td>Resource</td>
        <td>
            <p>POST /conferences/{conferenceId}/{endpointId}</p>
        </td>
    </tr>
    <tr>
        <td>Request body</td>
        <td>
            <pre>
{
    "action": "configure",
    "bundle-transport": {
        "dtls": {
            "setup": "active",
            "type": "sha-256",
            "hash": "5F:E7:53:B4:7E:51:DF:23:5C:60:5D:B5:D7:4F:83:72:25:3D:39:2E:47:A5:70:39:D9:8A:B4:61:16:84:5B:F4"
        },
        "ice": {
            "ufrag": "MaUF",
            "pwd": "QHIw+ToRDbEREVmY7xoQKH+e",
            "candidates": [
                {
                    "foundation": "4258379142",
                    "component": 1,
                    "protocol": "udp",
                    "priority": 2122260223,
                    "ip": "192.168.1.250",
                    "port": 52037,
                    "type": "host",
                    "generation": 0,
                    "network": 2
                },
                {
                    "foundation": "1162850324",
                    "component": 1,
                    "protocol": "udp",
                    "priority": 2122194687,
                    "ip": "192.168.1.235",
                    "port": 58625,
                    "type": "host",
                    "generation": 0,
                    "network": 1
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
            "clockrate": "48000",
            "channels": "2"
        },
        "ssrcs": [
            2981946593
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
                "clockrate": "90000"
            },
            {
                "id": 96,
                "parameters": {
                    "apt": "100"
                },
                "rtcp-fbs": [],
                "name": "rtx",
                "clockrate": "90000"
            }
        ],
        "ssrcs": [
            2321667837,
            2710080505,
            575175879,
            1189679233,
            1842596883,
            2162674519
        ],
        "ssrc-groups": [
            {
                "ssrcs": [
                    "2321667837",
                    "2710080505"
                ],
                "semantics": "FID"
            },
            {
                "ssrcs": [
                    "575175879",
                    "1189679233"
                ],
                "semantics": "FID"
            },
            {
                "ssrcs": [
                    "1842596883",
                    "2162674519"
                ],
                "semantics": "FID"
            },
            {
                "ssrcs": [
                    "2321667837",
                    "575175879",
                    "1842596883"
                ],
                "semantics": "SIM"
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
}            </pre>
        </td>
    </tr>
    <tr>
        <td>Response</td>
        <td>
            200 OK
        </td>
    </tr>    
</table>

# Reconfigure endpoint

<table>
    <tr>
        <td>Description</td>
        <td>
            <p>Reconfigure endpoint id {endpointId} in the conference {conferenceId} adding or removing ssrcs sent from that endpoint.</p>
        </td>
    </tr>
    <tr>
        <td>Resource</td>
        <td>
            <p>POST /conferences/{conferenceId}/{endpointId}</p>
        </td>
    </tr>
    <tr>
        <td>Request body</td>
        <td>
            <pre>
{
    "action": "reconfigure",
    "audio": {
        "ssrcs": [
            2981946593
        ]
    },
    "video": {
        "ssrcs": [
            2321667837,
            2710080505,
            575175879,
            1189679233,
            1842596883,
            2162674519
        ],
        "ssrc-groups": [
            {
                "ssrcs": [
                    "2321667837",
                    "2710080505"
                ],
                "semantics": "FID"
            },
            {
                "ssrcs": [
                    "575175879",
                    "1189679233"
                ],
                "semantics": "FID"
            },
            {
                "ssrcs": [
                    "1842596883",
                    "2162674519"
                ],
                "semantics": "FID"
            },
            {
                "ssrcs": [
                    "2321667837",
                    "575175879",
                    "1842596883"
                ],
                "semantics": "SIM"
            }
        ]
    }
}
            </pre>
        </td>
    </tr>
    <tr>
        <td>Response</td>
        <td>
            200 OK
        </td>
    </tr>    
</table>

# Expire endpoint

<table>
    <tr>
        <td>Description</td>
        <td>
            <p>Delete an endpoint id {endpointId} in the conference {conferenceId}.</p>
        </td>
    </tr>
    <tr>
        <td>Resource</td>
        <td>
            <p>POST /conferences/{conferenceId}/{endpointId}</p>
        </td>
    </tr>
    <tr>
        <td>Request body</td>
        <td>
            <pre>
{
    "action": "expire",
    "data": {
    }
}
            </pre>
        </td>
    </tr>
    <tr>
        <td>Response</td>
        <td>
            200 OK
        </td>
    </tr>    
</table>

# Start recording endpoint

<table>
    <tr>
        <td>Description</td>
        <td>
           <p>Start forwarding recording for endpoint id {endpointId} in the conference {conferenceId}.</p>
        </td>
    </tr>
    <tr>
        <td>Resource</td>
        <td>
            <p>POST /conferences/{conferenceId}/{endpointId}</p>
        </td>
    </tr>
    <tr>
        <td>Request body</td>
        <td>
            <pre>
{
    "action": "record",
    "user-id": "userId0",
    "session-id": "bfe59e5d-4bbf-4ea1-9f94-ddc8d902ea75",
    "recording-id": "recordingId0",
    "recording-modalities": {
        "audio": true,
        "video": true,
        "screenshare": false
    },
    "channels": [
        {
            "id": String,
            "aes-key": String,
            "aes-salt": String,
            "host": String,
            "port": String,
        }
    ]
}            </pre>
        </td>
    </tr>
    <tr>
        <td>Response</td>
        <td>
            200 OK
        </td>
    </tr>    
</table>
