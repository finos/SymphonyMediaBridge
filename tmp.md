# Video streaming through SMB
Each participant can produce output from 0 to 2 video sources. From a video camera, the client will typically produce a simulcast stream with up to 3 levels. 
A second video stream may be present and that would in that case be a single ssrc screen share stream with variable quality.

The responsibility of SMB is to forward the incoming streams to receivers in the meeting according to the receivers capability. 
SMB will pick only one simulcast level to forward per receiver. 

# Pinning
Each user can pin one other user. Each user can be pinned by >= 0 users. Decisions are made at the following events:
1. A user leaves. Data structure must be updated to clear all pins on A
2. A packet from A on stream S arrives. Is this packet wanted by anyone who pins user A?
3. A packet from A on stream S is ready. Should this packet be forwarded to user Y?

Conceptually the pin feature is a table as below with one digit per row, and 0 or more digits per column. Users on the vertical axis pin users on the horizontal axis.
```
       A  B  C  D

A      0  1  0  0
B      0  0  1  0
C      1  0  0  0
D      1  0  0  0
E      0  0  0  0
```
By storing who is pinned along the vertical axis. It is easy to make decision 3.
```
       A  B  C  D

A (B)  0  1  0  0
B (C)  0  0  1  0
C (A)  1  0  0  0
D (A)  1  0  0  0
E ()   0  0  0  0
```

By storing a pin count per stream for each user, it is easy to make decision 2. It is easy to update as well when pin changes, or quality subscription changes.
The actual quality subscribed to for the pinned user can be derived from the subscription quality on dominant speaker as described in this document.
```
       A  B  C  D
count  2  1  1  0

A (B)  0  1  0  0
B (C)  0  0  1  0
C (A)  1  0  0  0
D (A)  1  0  0  0
E ()   0  0  0  0
```

The data above is cheap to maintain when users pin, unpin as the lookup is trivial.

To answer question 1, you still need a list of all users pinning a specific person. All those have to be updated with regards to their pin target.
This list can either be a list of user ids, or a bit vector. The first option is good if the number of pinning users is expected to be small.
The second option is good if the number of pinning users is expected to be big. In the latter case it is also a good option to store a list of exceptions. 
That is, who is not pinning a specific person.
The bit vector poses a problem when users are added and leaves as the position in the vector is tied to a specific user. To reduce shifting data in the vector
a content table is preferable to allow users to keep their position in the vector also when others are removed.
However, making decision 1, is not a frequent event. A check first if the pin count is > 0 on the leaving user, followed by a scan through the, hopefully short, list of
pinning users on the vertical axis is sufficient to make the necessary update.


Storage with bit vector is compact.
```
For x pinning users

x * id(size_t)
x * (x / 32) * sizeof(int32_t) (if all pin like no one else)
x * 3 * sizeof(int16_t) for pinned by stream count

For 1024 pinning users it makes 145408 B roughly
```

Storing only a map of pinning users, a count of pinned streams per user, and a pinned user per user would require:
```
x * id(size_t)
x * 3 * sizeof(uint16_t)
x * (id + set OH)

~ 22528 B for 1024 users

```
This is clearly the most efficient in terms of storage and maintenance. One maps containing participant records with: 
- optional pinned user id
- pinned-by count per stream quality

The map containing the records allows for quick lookup of the participant's data both for receiving a packet and forwarding a packet.
The map containing pointers to participant records for users who are actually pinning someone, will allow quick scan of pinning users when a pinned user leaves. You could scan the first map but it will be larger.

![](./images/PINPLI.png)

# Stream subscription on lastN list
Each user has a limited downlink bandwidth and this will inevitably put a restriction on which streams the user may receive from the video senders in the lastN list.
The subscription can be expressed as a quality matrix like 
```
Desired quality, position and quality
    H  M  L
1:  1  0  0
2:  0  1  0
3:  0  1  0 
4:  0  1  0
5:  0  1  0

or subscription vector
1: H
2: M
3: M
4: M
...

```

Considering only pinned user and potential SS, and number of users. The subscription can be collapsed into
- pinQuality: H
- unpinnedQuality: M
- SS: H
If number of unpinned users change, this subscription may change. Regardless, it can be expanded into a subscription matrix, or vector.


For all users in the system the total subscription can be aggregated with matrix addition into:
```
    H   M   L
1: 25   4   0
2:  0  29   1 
3:  0  29   0 
4:  0  29   0
5:  0  29   0
```

This reflects the desired quality, but then there is a concept of available quality. Some devices may only produce 1 or 2 simulcast levels instead of three. The exact quality of these levels can be unknown. A device can choose to produce one level and turn it dormant. SMB must detect this dynamically and be able to fall back to the next available quality below the desired quality. 
```
Available quality matrix

1:  1   1   1
2:  0   0   1 
3:  0   1   1 
4:  0   0   1
5:  0   0   1

```

Combining the global subscription and available quality matrix to answer the question: is received ssrc used?
Compare row by row and shift the subscription to the next available lower quality.
```
1: 25   4   0
2:  0   0  30 
3:  0  29   0 
4:  0   0  29
5:  0   0  29
```
To answer the question: Is receive ssrc in use?
If the ssrc received has a number > 0 in the global subscription matrix, the stream is in use. If ssrc is highest available, it is in use if the global subscription matrix contains nonero value for this quality, or non zero value for a higher quality.

To answer the question: should ssrc from X be forwarded to user Y?
Look at the subscription row for user Y, and compare with availability row for user X. Shift subscription to the right until it coincides with 
available stream from X. If the receive ssrc has a 1 in the row, the packet should be forwarded.

The subscriptions could be like the pin matrix, for a specific user and quality. The advantage by having a subscription table referring to a position in the lastN list, means the subscription is mostly static, but the lastN list may change every 2s. Only when a user's downlink limit changes, would his subscription potentially change and the aggregated matrix must be update by a subtraction and addition.

I would actually suggest that the math::Matrix is used to store the subscription as it provides Matrix addition and subtraction that helps 
maintaining the global subscription table.
## Maintaining the subscriptions
Events that will cause updating of the subscription matrices:
A user joins or leaves. A user's downlink estimate is updated in a way that requires his subscription to change.

- If a user joins a new default subscription for initial downlink estimate will be created. That matrix is then added to the global one.
- If a user leaves, his matrix must be subtracted from the global matrix.
- If a user changes his subscription, his current matrix must be subtracted from the global matrix and the new one added to the global matrix.

These events can be considered to be fairly infrequent and thus fairly cheap with 3x9 additions. The client downlink estimate may cross a threshold often but decision to actually change the subscription to increase bitrate should be limited in frequency.

# Combining subscriptions and pin to receive and forward packets
## Receiving a packet
When a packet is received from user A stream S, the following will happen:
```
Find user A's participant record.
Find A's position in the lastN list. 
If A has a position: 
    check the global subscription matrix to see if his position has a count for the stream S. If so process the packet.
else :
    check participant A's pinned count for stream S. If it is > 0, process the packet.
```
## Forwarding a packet
When A packet from A stream S is about to be forwarded to X.
```
Retrieve A's and X's participant record.
Find A's position in lastN list.
If X is pinning A (it is in X's participant record)
       If S is of desired quality according to X subscription matrix for position 1 in lastN, then forward the packet.
else if A has a position in lastN
       check X's subscription matrix for A's position to see if stream S is wanted for this position.
```
## Complexity
Finding participant records A and X are plain hash map lookups.
Finding A's position in lastN is either a linear scan of a short list, or retrieved from a map in ActiveMediaList that would map users in lastN to their current position. Updating this map is a bit tedious as rearranging 
the lastN members requires updating all in the map.
The other values are straightforward referencing in the participant records.

# Use of PLI in SMB
Picture Loss Indicator is a request sent over RTCP to request the remote side to generate a new key frame. It is generally needed if the 
receiver lost the initial key frame, or has encountered too much packet loss to make it meaningful trying to mend the stream by re-transmissions.

## Situations when a PLI is needed and sent from SMB
- A client sends a PLI to SMB for a specific video ssrc. SMB cannot regenerate the key frame and must forward this PLI to the original sender of the stream.
The streams of interest can be identified by comparing the new users subscription matrix against the contents of lastN list.
- SMB Director changes the stream subscription for a receiver client, due to bandwidth variation. The streams not yet received by the user must generate a key frames.
- A client pins another user. This is similar to subscription change. The pinned user must generate a key frame on the higher quality stream. The previously pinned user must generate key frame for his lower resolution streams.
- A client enters the lastN list. His stream is new to everyone else and a key frame must be generated for streams subscribed to. The global subscription table can be used.
- A client stops sending on a stream that was previously active. SMB has to assume client dropped stream because of uplink bandwidth. The next active stream will be used and that stream must now generate a key frame. How is this best identified in the data structures???
- A client starts sending on a stream that was previously idle. This stream may now be forwarded according to global subscription matrix and a key frame is needed unless that is already.
- A client transport has connected, which means successful join and earliest point of sending video to the new receiver. All existing clients, with video, must now generate key frames for all the streams that the new client will receive.

What is actually being forwarded is a combination of: 
- the sender's position in lastN and if the receiver pins the sender.
- the receiver's subscription
- the available streams from the sender. Effective quality can actually be lower if requested quality is unavailable.

A new key frame is needed for the receiver if the source stream is different from the previous stream being sent to the receiver. 
Instead of comparing previous subscriptions, available qualities and pinning we can go for a much simpler condition to generate PLI.
- When a video packet is about to be forwarded, check if the ssrc of the packet is different from the previous ssrc of video packet being rewritten for the receiver. If it is, the stream is "new" to the receiver and a key frame is needed.
- If a client sends PLI to SMB it needs something went wrong at the client or network and SMB must forward that PLI to the sender.

There are optimizations in-place to prevent multiple PLIs being sent to a sender until there is a theoretical chance for the sender to generate a key frame and for SMB to receive it. That is RTT to sender is used.
Also if SMB receives a new PLI a short time after sending a key frame to client, it is probably the case that this PLI can be ignored. The RTT to the receiver can be used to figure out if the PLI could hve been sent after receiver got the key frame.


## Questions
In updateDirectorUplinkEstimates it seems that all users that are pinning someone, and also had a new "client downlink estimate" will trigger a PLI request to pinned target. If it is the same user, only one PLI will be sent.
This seems like a lot of load.
