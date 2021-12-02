function rtxVp8Used(sdpStr: string, primarySsrc: number): boolean {
    // Check that VP8 and rtx are present in SDP and connected with rtpmap
    let regExpVp8 = new RegExp(".*" + " VP8/90000" + ".*$", "gmi");
    let vp8SdpLines = sdpStr.match(regExpVp8);
    if (!vp8SdpLines) {
        return false;
    }
    let ptVp8: number = parseInt(vp8SdpLines[0].substr("a=rtpmap:".length));
    let regExpRtx = new RegExp(".*" + " rtx/90000" + ".*$", "gmi");
    let ptRtxVp8Present: boolean = false;
    let rtxLines = sdpStr.match(regExpRtx);
    if (!ptVp8 || !rtxLines) {
        return false;
    }
    rtxLines.forEach(function(rtxLine) {
        let potentialPt: number = parseInt(rtxLine.substr("a=rtpmap:".length));
        let potentialRtxVp8str: string = "a=fmtp:" + potentialPt + " apt=" + ptVp8;
        if (sdpStr.indexOf(potentialRtxVp8str) >= 0) {
            ptRtxVp8Present = true;
        }
    });
    // Check that ssrc-group exists
    let fidPresent: boolean = (sdpStr.indexOf("a=ssrc-group:FID " + primarySsrc) >= 0);

    return (ptRtxVp8Present && fidPresent);
}

function generateSsrcMap(sdpStr: string, rtxUsed: boolean, primarySsrc: number, nrOfSimulcastLayers: number): number[] {
    let ssrcStr = "a=ssrc:";
    let usedSsrc: number[] = [0, 1];
    let i: number, j: number;
    let ssrcSimulcastMap: number[] = [primarySsrc];

    // Store already used SSRCs
    let regExpSsrc = new RegExp(".*" + ssrcStr + ".*$", "gmi");
    let ssrcLines = sdpStr.match(regExpSsrc);
    ssrcLines && ssrcLines.forEach(function(ssrcLine) {
        let sdpSsrc: number = parseInt(ssrcLine.substr(ssrcStr.length));
        if (!(usedSsrc.indexOf(sdpSsrc) >= 0)) {
            usedSsrc.push(sdpSsrc);
        }
    });

    // Insert known VP8/rtx ssrc and generate unique SSRCs for simulcast layers
    let columns: number = rtxUsed ? 2 : 1;
    if (rtxUsed) {
        let fidStr: string = "a=ssrc-group:FID " + primarySsrc + " ";
        let regExpFid: RegExp = new RegExp(".*" + fidStr + ".*$", "gmi");
        let fidLines = sdpStr.match(regExpFid);
        ssrcSimulcastMap[1] = fidLines ? parseInt(fidLines[0].substr(fidStr.length)) : NaN;
    }
    for (i = 1; i < nrOfSimulcastLayers; i++) {
        for (j = 0; j < columns; j++) {
            let newSsrc: number = generateSSRC();
            while (usedSsrc.indexOf(newSsrc) >= 0) {
                newSsrc = generateSSRC();
            }
            ssrcSimulcastMap[i * columns + j] = newSsrc;
            usedSsrc.push(newSsrc);
        }
    }
    return (ssrcSimulcastMap);
}

function generateSSRC() {
    let minSSRC = 1;
    let maxSSRC = 0xffffffff;
    return (Math.floor(Math.random() * (maxSSRC - minSSRC)) + minSSRC);
}

export function addSimulcastSdpLocalDescription(inSessionDescription: RTCSessionDescriptionInit, mediaTrackId: string, nrOfSimulcastLayers: number) {
    if (inSessionDescription.sdp) {
        let sdpStr = inSessionDescription.sdp;
        // Validate that SDP has a video line
        let videoSdp = "m=video ";
        let ssrcStr = "a=ssrc:";
        let i, j;
        // Do basic validation that SDP contains video section and the media track
        if ((!(sdpStr.indexOf(videoSdp) >= 0)) || (!(sdpStr.indexOf(ssrcStr) >= 0)) || (!(sdpStr.indexOf(mediaTrackId) >= 0))) {
            return (inSessionDescription);
        } else {
            // Get SSRC for the mediatrack
            let regExpMediaTrackPrimarySsrc = new RegExp(".*" + "label:" + mediaTrackId + ".*$", "gmi");
            let mediaTrackPrimarySsrcArr = sdpStr.match(regExpMediaTrackPrimarySsrc);
            if (!mediaTrackPrimarySsrcArr) {
                return (inSessionDescription);
            }
            let primarySsrc = parseInt(mediaTrackPrimarySsrcArr[0].substr(ssrcStr.length));

            // Validate that SIM: hasn't already been added for this mediatrack
            if (sdpStr.indexOf("a=ssrc-group:SIM " + primarySsrc) >= 0) {
                return (inSessionDescription);
            }

            let rtxUsed = rtxVp8Used(sdpStr, primarySsrc);
            let ssrcSimulcastMap: number[] = generateSsrcMap(sdpStr, rtxUsed, primarySsrc, nrOfSimulcastLayers);

            // Fill in ssrc lines by copy pasting ssrc lines with generated simulcast ssrc
            let primarySsrcStr: string = "a=ssrc:" + primarySsrc + " ";
            let regExpPrimarySsrcLine = new RegExp(".*" + primarySsrcStr + ".*$", "gmi");
            let sdpPrimarySsrcLinesArr = sdpStr.match(regExpPrimarySsrcLine);
            if (!sdpPrimarySsrcLinesArr) {
                return (inSessionDescription);
            }
            let sdpPrimarySsrcLines = sdpPrimarySsrcLinesArr.filter(lines => lines !== "").join("\r\n");
            sdpPrimarySsrcLines += "\r\n";
            let ssrcAddLines: string = "";
            let columns = rtxUsed ? 2 : 1;
            let regExpPrimarySsrc = new RegExp(primarySsrcStr, "gmi");
            for (i = 1; i < nrOfSimulcastLayers; i++) {
                for (j = 0; j < columns; j++) {
                    ssrcAddLines += sdpPrimarySsrcLines.replace(regExpPrimarySsrc, "a=ssrc:" + ssrcSimulcastMap[i * columns + j] + " ");
                }
            }
            let lastSsrcStr: string = "a=ssrc:" + ssrcSimulcastMap[columns - 1] + " ";
            let index: number = sdpStr.lastIndexOf(lastSsrcStr);
            index = sdpStr.indexOf("\r\n", index) + 2;
            sdpStr = sdpStr.substr(0, index) + ssrcAddLines + sdpStr.substr(index);

            // Fill in the a=ssrc-group:FID lines for simulcast if rtx is present
            if (rtxUsed) {
                let fidAddLines = "";
                for (i = 1; i < nrOfSimulcastLayers; i++) {
                    fidAddLines += "a=ssrc-group:FID " + ssrcSimulcastMap[i * 2] + " " + ssrcSimulcastMap[i * 2 + 1] + "\r\n";
                }
                let findFidStr = "a=ssrc-group:FID " + ssrcSimulcastMap[0] + " " + ssrcSimulcastMap[1] + "\r\n";
                index = sdpStr.indexOf(findFidStr) + findFidStr.length;
                sdpStr = sdpStr.slice(0, index) + fidAddLines + sdpStr.slice(index);
                index += fidAddLines.length;
            } else {
                // Put next item under a=ssrc: list if ssrc-group:FID is missing
                index += ssrcAddLines.length;
            }

            // Add a=ssrc-group:SIM
            let simGroupStr = "a=ssrc-group:SIM";
            for (i = 0; i < nrOfSimulcastLayers; i++) {
                simGroupStr += " " + ssrcSimulcastMap[i * columns];
            }
            simGroupStr += "\r\n";
            sdpStr = sdpStr.slice(0, index) + simGroupStr + sdpStr.slice(index);

            // Update Session SDP
            inSessionDescription.sdp = sdpStr;
        }
    }
    return (inSessionDescription);
}

// Return the MediaStreamTrack id for the m= section with a=mid:<mid>
export function getTrackIdForMid(inSessionDescription: RTCSessionDescriptionInit, mid: string) {
    const sdpStr = inSessionDescription.sdp;
    if (sdpStr) {
        let videoIndex = -1;
        const midIndex = sdpStr.indexOf("a=mid:" + mid);
        const regExpVideo = new RegExp("m=video " + ".*$", "gmi");

        while (regExpVideo.exec(sdpStr)) {
            if (regExpVideo.lastIndex < midIndex) {
                videoIndex = regExpVideo.lastIndex;
            } else {
                break;
            }
        }

        if (videoIndex >= 0) {
            const regExpPrimarySsrcLine = new RegExp("a=ssrc:" + ".*" + "msid:.* (.*)" + ".*$", "mi");
            const primarySsrcLine = sdpStr.substr(videoIndex).match(regExpPrimarySsrcLine);
            if (primarySsrcLine) {
                return primarySsrcLine[1];
            }
        }
    }
    return undefined;
}
