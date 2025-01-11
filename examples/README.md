# Symphony Media Bridge (SMB) usage examples

The examples consists of two projects, simpleserver and simpleclient. Simpleserver acts as a signalling server that communicates with clients and is 
responsible for allocating and configuring SMB resources. Simpleclient acts as the end user client for the conferencing system.

These projects are limited in their use cases and are only meant for documentation and testing purposes.

## Building
Make sure SMb has been built with legacy_api off

### Simpleserver

Simpleserver is a java project requiring maven and openjdk 11 or later.

```mvn clean package```

```java -jar target/simpleserver-0.0.1-SNAPSHOT.jar```

### Simpleclient

```npm install```

```gulp```

## Running

1. Start SMB.
2. Start simpleserver.
3. Open two or more tabs in Chrome with localhost:3000
4. Make sure the simpleserver instance at https://localhost:8081/ is accessible from Chrome. You might have to add an exception for the self-signed certificate of simpleserver.
5. Click 'join' in the simpleclient instances.

Each simpleclient instance will start a local audio stream, and a local simulcast enabled video stream. The endpointId considered the dominant speaker will be 'pinned' automatically by simpleclient.
