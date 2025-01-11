import {addSimulcastSdpLocalDescription} from './simulcast';

const endpointIdLabel: HTMLLabelElement = <HTMLLabelElement>document.getElementById('endpointId');
const dominantSpeakerLabel: HTMLLabelElement = <HTMLLabelElement>document.getElementById('dominantSpeaker');
const audioElementsDiv: HTMLDivElement = <HTMLDivElement>document.getElementById('audioElements');
const videoElementsDiv: HTMLDivElement = <HTMLDivElement>document.getElementById('videoElements');
let peerConnection: RTCPeerConnection|undefined = undefined
let localMediaStream: MediaStream|undefined = undefined;
let localDataChannel: RTCDataChannel|undefined = undefined;
let endpointId: string|undefined = undefined;
let conferenceId: string|undefined = undefined;
let remoteMediaStreams: Set<string> = new Set();

const serverUrl = 'https://localhost:8081/conferences/';

interface UserVideoMapItem
{
    ssrc: Number;
    msid: String;
    element: HTMLVideoElement;
}

let receivers = new Map<string, UserVideoMapItem>();
let keepPolling = true;

// Keeps a long-poll running for simpleserver to simpleclient communication
async function startPoll()
{
    const url = serverUrl + 'endpoints/' + endpointId + '/poll';
    const requestInit: RequestInit = {method : 'GET', mode : 'cors', cache : 'no-store'};

    const request = new Request(url, requestInit);
    const result = await fetch(request);
    if (result.status === 200)
    {
        const resultJson = await result.json();
        console.log('Poll result ' + JSON.stringify(resultJson));
        onPollMessage(resultJson);
    }
    else if (result.status !== 204)
    {
        console.error('Poll failed ' + result.statusText)
        return;
    }

    if (keepPolling)
    {
        startPoll();
    }
}

async function onPollMessage(resultJson: any)
{
    if (resultJson.type === 'offer')
    {
        const remoteDescription = new RTCSessionDescription({type : 'offer', sdp : resultJson.payload});
        console.log("Got offer " + JSON.stringify(remoteDescription));
        await peerConnection.setRemoteDescription(remoteDescription);

        let localDescription = await peerConnection.createAnswer();

        // Add simulcast layers to the video stream with SDP munging
        const mediaTracks = localMediaStream.getVideoTracks();
        mediaTracks.forEach(
            mediaTrack => { localDescription = addSimulcastSdpLocalDescription(localDescription, mediaTrack.id, 3); });

        await peerConnection.setLocalDescription(localDescription);
    }
}

// SMB does not support trickle-ice, therefore we wait until the gathering is complete to send the SDP
// answer so that it includes all candidates
async function onIceGatheringStateChange(event: Event)
{
    const iceGatheringState = peerConnection.iceGatheringState;

    if (iceGatheringState === 'complete')
    {
        console.log('ICE candidate gathering complete');
        if (peerConnection.localDescription)
        {
            console.log('Sending answer ' + JSON.stringify(peerConnection.localDescription));

            const url = serverUrl + 'endpoints/' + endpointId + '/actions';
            const body = {type : 'answer', payload : peerConnection.localDescription.sdp};

            const requestInit:
                RequestInit = {method : 'POST', mode : 'cors', cache : 'no-store', body : JSON.stringify(body)};
            const request = new Request(url, requestInit);
            const result = await fetch(request);

            console.log('Answer result ' + result.status);
        }
    }
}

function onTrack(event: RTCTrackEvent)
{
    var count: number = 0;
    for (const stream of event.streams)
    {
        if (remoteMediaStreams.has(stream.id))
        {
            continue;
        }
        count = count + 1;
        remoteMediaStreams.add(stream.id);
        console.log(`Added remote stream ${stream.id} audio ${stream.getAudioTracks().length} video ${
            stream.getVideoTracks().length} `);

        if (stream.getAudioTracks().length !== 0)
        {
            const audioElement = new Audio();
            audioElement.autoplay = true;
            audioElement.srcObject = stream;
            audioElementsDiv.appendChild(audioElement);
            console.log(`${audioElementsDiv.children.length} Added audio element ${stream.id}`);
        }

        if (stream.getVideoTracks().length !== 0)
        {
            const videoElement = <HTMLVideoElement>document.createElement('video');
            videoElement.autoplay = true;
            videoElement.srcObject = stream;
            videoElement.loop = true;
            videoElement.width = 320;
            videoElement.height = 180;
            videoElement.muted = false;
            videoElementsDiv.appendChild(videoElement);
            var mapItem: UserVideoMapItem = {
                ssrc : 0, // not available anyway
                msid : event.track.id as String,
                element : videoElement
            };

            receivers.set(event.track.id, mapItem);
            console.log(`${videoElementsDiv.children.length} Added video element ${stream.id}`);
        }
    }
}

function getSsrcOfVideoMsid(trackId: String): Number
{
    var rtpReceivers = peerConnection.getReceivers();
    for (var rtpReceiver of rtpReceivers)
    {
        var ssrcs = rtpReceiver.getSynchronizationSources();
        if (ssrcs.length == 0 || rtpReceiver.track.kind != "video")
        {
            continue;
        }

        if (rtpReceiver.track.label == trackId)
        {
            return ssrcs[0].source;
        }
    }
    return null;
}

function getVideoElementBySsrc(ssrc: Number): HTMLVideoElement
{
    var rtpReceivers = peerConnection.getReceivers();
    for (var rtpReceiver of rtpReceivers)
    {
        var ssrcs = rtpReceiver.getSynchronizationSources();
        if (ssrcs.length == 0 || rtpReceiver.track.kind != "video")
        {
            continue;
        }

        if (ssrcs[0].source == ssrc)
        {
            var mapItem = receivers.get(rtpReceiver.track.label);
            return mapItem.element;
        }
    }
    return null;
}

function getSsrcOfVideoElement(element: HTMLVideoElement)
{
    var rtpReceivers = peerConnection.getReceivers();

    for (var rx of receivers)
    {
        if (rx[1].element == element)
        {
            for (var rtpReceiver of rtpReceivers)
            {
                var ssrcs = rtpReceiver.getSynchronizationSources();
                if (ssrcs.length == 0 || rtpReceiver.track.kind != "video" || rtpReceiver.track.id != rx[1].msid)
                {
                    continue;
                }

                return ssrcs[0].source;
            }
        }
    }

    return null;
}

function getAllUserMapSsrcs(umap: any): Set<Number>
{
    var s = new Set<Number>();
    for (var endpoint of umap.endpoints)
    {
        for (var ssrc of endpoint.ssrcs)
        {
            s.add(ssrc);
        }
    }

    return s;
}

function onDataChannelMessage(event: MessageEvent<any>)
{
    console.log('onDataChannelMessage ' + event.data);
    const message = JSON.parse(event.data);
    if (message.type === 'DominantSpeaker')
    {
        const dominantSpeakerEndpointId = message.endpoint;

        dominantSpeakerLabel.innerText = dominantSpeakerEndpointId;

        if (dominantSpeakerEndpointId !== endpointId)
        {
            const pinMessage = {type : 'PinnedEndpointsChanged', pinnedEndpoints : [ dominantSpeakerEndpointId ]};

            localDataChannel.send(JSON.stringify(pinMessage));
            console.log('Sent data channel message ' + JSON.stringify(pinMessage));
        }
    }
    else if (message.type === 'UserMediaMap' || message.colibriClass === 'UserMediaMap')
    {
        var activeUsers = getAllUserMapSsrcs(message);
        for (var v of videoElementsDiv.children)
        {
            var videoElem = v as HTMLVideoElement;
            const ssrc = getSsrcOfVideoElement(videoElem);
            if (ssrc != null && !(ssrc in activeUsers))
            {
                videoElem.hidden = true;
            }
        }

        if (message.endpoints.length > 0)
        {
            var firstEndpoint = message.endpoints[0];
            for (var ssrc of firstEndpoint.ssrcs)
            {
                console.log("ssrc speaking", ssrc)

                var videoElement = getVideoElementBySsrc(ssrc);

                if (videoElement && videoElementsDiv.firstChild != videoElement)
                {
                    videoElementsDiv.removeChild(videoElement);
                    var speaker = videoElementsDiv.firstChild as HTMLVideoElement
                    speaker.width = 320;
                    speaker.height = 180;
                    videoElementsDiv.insertBefore(videoElement, videoElementsDiv.firstChild);
                    speaker = videoElementsDiv.firstChild as HTMLVideoElement
                    speaker.width = 640;
                    speaker.height = 360;
                    speaker.hidden = false;
                    console.log(`replace top ${videoElementsDiv.children.length}`);
                    break;
                }
            }
        }

        for (const endpoint of message.endpoints)
        {
            for (var ssrc of firstEndpoint.ssrcs)
            {
                var videoElement = getVideoElementBySsrc(ssrc);
                videoElement.hidden = false;
            }
            return;
        }
    }
}

async function joinClicked()
{
    console.log('Join clicked ...');

    (async () => {
        await navigator.mediaDevices.getUserMedia({audio : true, video : true});
        let devices = await navigator.mediaDevices.enumerateDevices();
        console.log(devices);
    })();

    peerConnection = new RTCPeerConnection();
    peerConnection.onicegatheringstatechange = onIceGatheringStateChange;
    peerConnection.ontrack = onTrack;
    console.log('Created RTCPeerConnection');

    const videoDeviceId = sessionStorage.getItem('selectedVideoDevice');
    const audioDeviceId = sessionStorage.getItem('selectedAudioDevice');

    const contraints = {video : {deviceId : videoDeviceId}, audio : {deviceId : audioDeviceId}};

    localMediaStream = await navigator.mediaDevices.getUserMedia(contraints);
    const localVideo = document.getElementById("localVideo") as HTMLVideoElement;
    localVideo.srcObject = localMediaStream;
    localVideo.loop = true;
    localVideo.width = 160;
    localVideo.height = 90;

    console.log('Got local stream ' + JSON.stringify(localMediaStream));
    localMediaStream.getTracks().forEach(track => peerConnection.addTrack(track, localMediaStream));

    localDataChannel = peerConnection.createDataChannel("webrtc-datachannel", {ordered : true});
    localDataChannel.onmessage = onDataChannelMessage;

    const url = serverUrl + 'endpoints/';
    const body = {};

    const requestInit: RequestInit = {method : 'POST', mode : 'cors', cache : 'no-store', body : JSON.stringify(body)};
    const request = new Request(url, requestInit);
    const result = await fetch(request);
    const resultJson = await result.json();

    console.log('Join result ' + JSON.stringify(resultJson));

    endpointId = resultJson.endpointId;
    conferenceId = resultJson
    endpointIdLabel.innerText = endpointId;
    keepPolling = true;
    startPoll()
}

async function listAudioDevices()
{
    try
    {
        const devices = await navigator.mediaDevices.enumerateDevices();
        const audioDevices = devices.filter(device => device.kind === 'audioinput');
        const selectElement = document.getElementById('audioDevices') as HTMLSelectElement;
        selectElement.innerHTML = '';
        audioDevices.forEach(device => {
            const option = document.createElement('option');
            option.value = device.deviceId;
            option.text = device.label || `Device ${device.deviceId}`;
            selectElement.appendChild(option);
        });

        const savedDeviceId = sessionStorage.getItem('selectedAudioDevice');
        if (savedDeviceId)
        {
            selectElement.value = savedDeviceId;
        }

        selectElement.addEventListener('change',
            () => { sessionStorage.setItem('selectedAudioDevice', selectElement.value); });
    }
    catch (error)
    {
        console.error("Error accessing audio devices: ", error);
    }
}

async function listVideoDevices()
{
    try
    {
        const devices = await navigator.mediaDevices.enumerateDevices();
        const videoDevices = devices.filter(device => device.kind === 'videoinput');
        const selectElement = document.getElementById('videoDevices') as HTMLSelectElement;
        selectElement.innerHTML = '';
        videoDevices.forEach(device => {
            const option = document.createElement('option');
            option.value = device.deviceId;
            option.text = device.label || `Device ${device.deviceId}`;
            selectElement.appendChild(option);
        });
        const savedDeviceId = sessionStorage.getItem('selectedVideoDevice');
        if (savedDeviceId)
        {
            selectElement.value = savedDeviceId;
        }

        selectElement.addEventListener('change',
            async () => { sessionStorage.setItem('selectedVideoDevice', selectElement.value); });
    }
    catch (error)
    {
        console.error("Error accessing video devices: ", error);
    }
}

async function hangupClicked()
{
    keepPolling = false;
    const url = serverUrl + 'endpoints/' + endpointId + '/actions';
    const body = {type : 'hangup'};

    const requestInit: RequestInit = {method : 'POST', mode : 'cors', cache : 'no-store', body : JSON.stringify(body)};
    const request = new Request(url, requestInit);
    const result = await fetch(request);

    console.log('hangup result ' + result.status);

    localMediaStream.getTracks().forEach(function(track) { track.stop() })
    localMediaStream = null;
    localDataChannel.close();
    localDataChannel.onmessage = null;
    localDataChannel = null;
    peerConnection.ontrack = null;
    peerConnection.onicegatheringstatechange = null;
    peerConnection.ondatachannel = null;
    remoteMediaStreams.clear();
    peerConnection.close();
    peerConnection = null;

    const localVideo = document.getElementById("localVideo") as HTMLVideoElement;
    localVideo.srcObject = null;

    audioElementsDiv.textContent = '';
    videoElementsDiv.textContent = '';
}

async function main()
{
    var joinButton = document.getElementById('join') as HTMLButtonElement;
    joinButton.onclick = joinClicked;

    var hangupButton = document.getElementById('hangup') as HTMLButtonElement;
    hangupButton.onclick = hangupClicked;

    await listAudioDevices();
    await listVideoDevices();
}

main();
