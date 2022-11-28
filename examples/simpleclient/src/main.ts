import { addSimulcastSdpLocalDescription } from './simulcast';

const joinButton: HTMLButtonElement = <HTMLButtonElement>document.getElementById('join');
const endpointIdLabel: HTMLLabelElement = <HTMLLabelElement>document.getElementById('endpointId');
const dominantSpeakerLabel: HTMLLabelElement = <HTMLLabelElement>document.getElementById('dominantSpeaker');
const audioElementsDiv: HTMLDivElement = <HTMLDivElement>document.getElementById('audioElements');
const videoElementsDiv: HTMLDivElement = <HTMLDivElement>document.getElementById('videoElements');
let peerConnection: RTCPeerConnection|undefined = undefined
let localMediaStream: MediaStream|undefined = undefined;
let localDataChannel: RTCDataChannel|undefined = undefined;
let endpointId: string|undefined = undefined;
let remoteMediaStreams: Set<string> = new Set();

const serverUrl = 'https://localhost:8081/conferences/';

// Keeps a long-poll running for simpleserver to simpleclient communication
async function startPoll() {
    const url = serverUrl + 'endpoints/' + endpointId + '/poll';
    const requestInit: RequestInit = {
        method: 'GET',
        mode: 'cors',
        cache: 'no-store'
    };

    const request = new Request(url, requestInit);
    const result = await fetch(request);
    if (result.status === 200) {
        const resultJson = await result.json();
        console.log('Poll result ' + JSON.stringify(resultJson));
        onPollMessage(resultJson);

    } else if (result.status !== 204) {
        console.error('Poll failed ' + result.statusText)
        return;
    }

    startPoll();
}

async function onPollMessage(resultJson: any) {
    if (resultJson.type === 'offer') {
        const remoteDescription = new RTCSessionDescription({
            type: 'offer', 
            sdp: resultJson.payload
        });
        console.log("Got offer " + JSON.stringify(remoteDescription));
        await peerConnection.setRemoteDescription(remoteDescription);

        let localDescription = await peerConnection.createAnswer();

        // Add simulcast layers to the video stream with SDP munging
        const mediaTracks = localMediaStream.getVideoTracks();
        mediaTracks.forEach(mediaTrack => {
            localDescription = addSimulcastSdpLocalDescription(localDescription, mediaTrack.id, 3);
        });

        await peerConnection.setLocalDescription(localDescription);
    }
}

// SMB does not support trickle-ice, therefore we wait until the gathering is complete to send the SDP 
// answer so that it includes all candidates
async function onIceGatheringStateChange(event: Event) {
    const iceGatheringState = peerConnection.iceGatheringState;

    if (iceGatheringState === 'complete') {
        console.log('ICE candidate gathering complete');
        if (peerConnection.localDescription) {
            console.log('Sending answer ' + JSON.stringify(peerConnection.localDescription));

            const url = serverUrl + 'endpoints/' + endpointId + '/actions';
            const body = {
                type: 'answer',
                payload: peerConnection.localDescription.sdp
            };

            const requestInit: RequestInit = {
                method: 'POST',
                mode: 'cors',
                cache: 'no-store',
                body: JSON.stringify(body)
            };
            const request = new Request(url, requestInit);
            const result = await fetch(request);

            console.log('Answer result ' + result.status);
        }
    }
}

function onTrack(event: RTCTrackEvent) {
    for (const stream of event.streams) {
        if (stream.id === 'smbvideomslabel' || stream.id === 'smbaudiomslabel' || remoteMediaStreams.has(stream.id)) {
            continue;
        }

        remoteMediaStreams.add(stream.id);
        console.log(`Added remote stream ${stream.id} audio ${stream.getAudioTracks().length} video ${stream.getVideoTracks().length}`);

        if (stream.getAudioTracks().length !== 0) {
            const audioElement = new Audio();
            audioElement.autoplay = true;
            audioElement.srcObject = stream;
            audioElementsDiv.appendChild(audioElement);
            console.log('Added audio element ' + stream.id);
        }

        if (stream.getVideoTracks().length !== 0) {
            const videoElement = <HTMLVideoElement>document.createElement('video');
            videoElement.autoplay = true;
            videoElement.srcObject = stream;
            videoElement.loop = true;
            videoElement.width = 640;
            videoElement.height = 480;
            videoElementsDiv.appendChild(videoElement);
            console.log('Added video element ' + stream.id);
        }
    }
}

function onDataChannelMessage(event: MessageEvent<any>) {
    console.log('onDataChannelMessage ' + event.data);
    const message = JSON.parse(event.data);
    if (message.type === 'DominantSpeaker') {
        const dominantSpeakerEndpointId = message.endpoint;

        dominantSpeakerLabel.innerText = dominantSpeakerEndpointId;

        if (dominantSpeakerEndpointId !== endpointId) {
            const pinMessage = {
                type: 'PinnedEndpointsChanged',
                pinnedEndpoints: [
                    dominantSpeakerEndpointId
                ]
            };
    
            localDataChannel.send(JSON.stringify(pinMessage));
            console.log('Sent data channel message ' + JSON.stringify(pinMessage));
        }
    }
}

async function joinClicked() {
    console.log('Join clicked');

    peerConnection = new RTCPeerConnection();
    peerConnection.onicegatheringstatechange = onIceGatheringStateChange;
    peerConnection.ontrack = onTrack;
    console.log('Created RTCPeerConnection');

    localMediaStream = await navigator.mediaDevices.getUserMedia({
        audio: true,
        video: true
    });
    console.log('Got local stream ' + JSON.stringify(localMediaStream));
    localMediaStream.getTracks().forEach(track => peerConnection.addTrack(track, localMediaStream));

    localDataChannel = peerConnection.createDataChannel("webrtc-datachannel", { ordered: true });
    localDataChannel.onmessage = onDataChannelMessage;
        
    const url = serverUrl + 'endpoints/';
    const body = {};

    const requestInit: RequestInit = {
        method: 'POST',
        mode: 'cors',
        cache: 'no-store',
        body: JSON.stringify(body)
    };
    const request = new Request(url, requestInit);
    const result = await fetch(request);
    const resultJson = await result.json();

    console.log('Join result ' + JSON.stringify(resultJson));

    endpointId = resultJson.endpointId;
    endpointIdLabel.innerText = endpointId;
    startPoll()
}

function main() {
    joinButton.onclick = joinClicked;
}

main();
