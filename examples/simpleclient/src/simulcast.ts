
function tracksOf(sdp: string): string[]
{
    var lines = sdp.split('\n');
    var tracks: Array<string> = [];

    var intrack = false;
    var track = "";
    for (var l of lines)
    {
        if (!intrack && l.startsWith("m="))
        {
            track = l;
            intrack = true;
        }
        else if (intrack && l.startsWith("m="))
        {
            tracks.push(track);
            track = l;
        }
        else if (intrack)
        {
            track += "\n" + l;
        }
    }
    return tracks;
}

function generateSsrcMap(trackDesc: string, rtxUsed: boolean, primarySsrc: number, nrOfSimulcastLayers: number):
    number[]
{
    let usedSsrc = new Set<number>();
    let i: number, j: number;
    let ssrcSimulcastMap: number[] = [ primarySsrc ];

    let regExpSsrc = new RegExp("^a=ssrc:(\\d+)\\s+.*$", "gmi");
    let ssrcMatch;
    while ((ssrcMatch = regExpSsrc.exec(trackDesc)) != null)
    {
        usedSsrc.add(Number(ssrcMatch[1]));
    }

    let columns: number = rtxUsed ? 2 : 1;
    if (rtxUsed)
    {
        ssrcSimulcastMap[1] = NaN;
        let regExpFid: RegExp = new RegExp("^a=ssrc-group:FID\\s+" + primarySsrc + "\\s+(\\d+)\s*$", "gmi");
        let fidLine;

        while ((fidLine = regExpFid.exec(trackDesc)) != null)
        {
            ssrcSimulcastMap[1] = Number(fidLine[1]);
        }
    }
    for (i = 1; i < nrOfSimulcastLayers; i++)
    {
        for (j = 0; j < columns; j++)
        {
            let newSsrc: number = generateSSRC();
            while (usedSsrc.has(newSsrc))
            {
                newSsrc = generateSSRC();
            }
            ssrcSimulcastMap[i * columns + j] = newSsrc;
            usedSsrc.add(newSsrc);
        }
    }
    return (ssrcSimulcastMap);
}

function generateSSRC()
{
    let minSSRC = 1;
    let maxSSRC = 0xffffffff;
    return (Math.floor(Math.random() * (maxSSRC - minSSRC)) + minSSRC);
}

export function addSimulcastSdpLocalDescription(inSessionDescription: RTCSessionDescriptionInit,
    nrOfSimulcastLayers: number)
{
    if (!inSessionDescription.sdp)
    {
        return inSessionDescription;
    }

    let sdp = inSessionDescription.sdp.replace(/\r\n/g, "\n");
    let i, j;

    for (const trackDesc of tracksOf(sdp))
    {
        if (!trackDesc.startsWith("m=video") || trackDesc.indexOf("a=ssrc-group:SIM") > 0)
        {
            continue;
        }

        const regExpPrimarySsrc = /^a=ssrc:(\d+)\s+msid:(\S+)\s+(\S+)\s*$/gm
        let primarySsrcMatch = regExpPrimarySsrc.exec(trackDesc);
        if (!primarySsrcMatch)
        {
            continue;
        }

        let primarySsrc = Number(primarySsrcMatch[1]);
        let msid = primarySsrcMatch[2];
        console.log("prim ssrc " + primarySsrc + " " + msid);

        const regExpPrimaryLabels = new RegExp("^a=ssrc:" + primarySsrc + " .*$", "gmi");
        let labelLineMatches = trackDesc.match(regExpPrimaryLabels);
        let labelLines = labelLineMatches.filter(lines => lines !== "").join("\n");
        labelLines += "\n";

        const regExpFid = /^a=ssrc-group:FID.+$/gm;
        let rtxUsed = !!regExpFid.exec(trackDesc);
        let ssrcSimulcastMap: number[] = generateSsrcMap(trackDesc, rtxUsed, primarySsrc, nrOfSimulcastLayers);

        let newTrack = trackDesc + "\n";

        let columns = rtxUsed ? 2 : 1;
        const regExpPrimaryLabel = new RegExp("a=ssrc:" + primarySsrc, "gmi");
        for (i = 1; i < nrOfSimulcastLayers; i++)
        {
            for (j = 0; j < columns; j++)
            {
                newTrack += labelLines.replace(regExpPrimaryLabel, "a=ssrc:" + ssrcSimulcastMap[i * columns + j]);
            }
        }

        for (i = 1; i < nrOfSimulcastLayers && rtxUsed; i++)
        {
            newTrack += "a=ssrc-group:FID " + ssrcSimulcastMap[i * 2] + " " + ssrcSimulcastMap[i * 2 + 1] + "\n";
        }

        let simGroupStr = "a=ssrc-group:SIM";
        for (i = 0; i < nrOfSimulcastLayers; i++)
        {
            simGroupStr += " " + ssrcSimulcastMap[i * columns];
        }
        newTrack += simGroupStr;

        sdp = sdp.replace(trackDesc, newTrack);
    }

    inSessionDescription.sdp = sdp;
    return inSessionDescription;
}
