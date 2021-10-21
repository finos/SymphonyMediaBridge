cmake_minimum_required(VERSION 3.6.2)
project(smb)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS_DEBUG "-stdlib=libc++ -glldb -O0 -DDEBUG -DENABLE_LEGACY_API=1 -fno-rtti -Wall -Wuninitialized -Wsign-compare -Wthread-safety -Wno-missing-braces")
set(CMAKE_CXX_FLAGS_DCHECK "-stdlib=libc++ -glldb -O0 -DDEBUG -DENABLE_LEGACY_API=1 -fno-rtti -Wall -Wuninitialized -Wsign-compare -Wthread-safety -Wno-missing-braces -fsanitize=address -fno-omit-frame-pointer \
                            -fprofile-arcs -ftest-coverage -fprofile-instr-generate -fcoverage-mapping")
set(CMAKE_CXX_FLAGS_TCHECK "-stdlib=libc++ -glldb -O1 -DDEBUG -DENABLE_LEGACY_API=1 -fno-rtti -Wall -Wuninitialized -Wsign-compare -Wthread-safety -Wno-missing-braces -fsanitize=thread -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELEASE "-stdlib=libc++ -O3 -DNDEBUG -DENABLE_LEGACY_API=1 -fno-rtti -Wno-missing-braces -Wuninitialized -Wsign-compare")
set(CMAKE_C_FLAGS_DEBUG "-glldb -O0 -DDEBUG -Wall")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_SYMRELEASE "-stdlib=libc++ -glldb -O2 -DNDEBUG -DENABLE_LEGACY_API=1 -fno-rtti -fno-omit-frame-pointer -Wall -Wno-missing-braces -Wuninitialized -Wsign-compare")

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

include_directories(.)
include_directories(external)

if(APPLE)
    message("MACOS detected")
    execute_process(COMMAND brew --prefix OUTPUT_VARIABLE BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)

    include_directories(${BREW_PREFIX}/opt/srtp/include)
    include_directories(${BREW_PREFIX}/opt/openssl@1.1/include)
    include_directories(${BREW_PREFIX}/opt/libmicrohttpd/include)
    include_directories(${BREW_PREFIX}/opt/opus/include)

    link_directories(${BREW_PREFIX}/opt/srtp/lib)
    link_directories(${BREW_PREFIX}/opt/openssl@1.1/lib)
    link_directories(${BREW_PREFIX}/opt/libmicrohttpd/lib)
    link_directories(${BREW_PREFIX}/opt/opus/lib)
elseif(UNIX)
        message("UNIX detected")
        set(CMAKE_INSTALL_RPATH "./libs")
        set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lc++abi -lrt")
else()
    message(FATAL_ERROR "Building on this platform is not supported.")
endif()

link_libraries(srtp2)
link_libraries(ssl crypto)
link_libraries(microhttpd)
link_libraries(opus)

set(FILES
        api/AllocateConference.h
        api/AllocateEndpoint.h
        api/DataChannelMessage.h
        api/DataChannelMessageParser.cpp
        api/DataChannelMessageParser.h
        api/EndpointDescription.cpp
        api/EndpointDescription.h
        api/Generator.cpp
        api/Generator.h
        api/Parser.cpp
        api/Parser.h
        api/Recording.h
        api/RecordingChannel.h
        aws/AwsHarvester.cpp
        aws/AwsHarvester.h
        bridge/ApiRequestHandler.cpp
        bridge/ApiRequestHandler.h
        bridge/AudioStream.h
        bridge/Bridge.cpp
        bridge/Bridge.h
        bridge/DataStream.h
        bridge/DataStreamDescription.h
        bridge/LegacyApiRequestHandler.cpp
        bridge/LegacyApiRequestHandler.h
        bridge/LegacyApiRequestHandlerHelpers.cpp
        bridge/LegacyApiRequestHandlerHelpers.h
        bridge/LegacyApiRequestHandlerHelpers.h
        bridge/Mixer.cpp
        bridge/Mixer.h
        bridge/MixerManager.cpp
        bridge/MixerManager.h
        bridge/RequestLogger.h
        bridge/RtpMap.cpp
        bridge/RtpMap.h
        bridge/Stats.cpp
        bridge/Stats.h
        bridge/StreamDescription.h
        bridge/TransportDescription.h
        bridge/VideoStream.h
        bridge/engine/ActiveMediaList.cpp
        bridge/engine/ActiveMediaList.h
        bridge/engine/AudioForwarderReceiveJob.cpp
        bridge/engine/AudioForwarderReceiveJob.h
        bridge/engine/AudioForwarderRewriteAndSendJob.cpp
        bridge/engine/AudioForwarderRewriteAndSendJob.h
        bridge/engine/EncodeJob.cpp
        bridge/engine/EncodeJob.h
        bridge/engine/Engine.cpp
        bridge/engine/Engine.h
        bridge/engine/EngineAudioStream.h
        bridge/engine/EngineCommand.h
        bridge/engine/EngineDataStream.h
        bridge/engine/EngineMessage.h
        bridge/engine/EngineMessageListener.h
        bridge/engine/EngineMixer.cpp
        bridge/engine/EngineMixer.h
        bridge/engine/EngineStats.h
        bridge/engine/EngineStreamDirector.h
        bridge/engine/EngineVideoStream.h
        bridge/engine/PacketCache.cpp
        bridge/engine/PacketCache.h
        bridge/engine/ProcessMissingVideoPacketsJob.cpp
        bridge/engine/ProcessMissingVideoPacketsJob.h
        bridge/engine/ProcessUnackedRecordingEventPacketsJob.cpp
        bridge/engine/ProcessUnackedRecordingEventPacketsJob.h
        bridge/engine/RecordingAudioForwarderSendJob.cpp
        bridge/engine/RecordingAudioForwarderSendJob.h
        bridge/engine/RecordingEventAckReceiveJob.cpp
        bridge/engine/RecordingEventAckReceiveJob.h
        bridge/engine/RecordingRtpNackReceiveJob.cpp
        bridge/engine/RecordingRtpNackReceiveJob.h
        bridge/engine/RecordingSendEventJob.cpp
        bridge/engine/SendEngineMessageJob.h
        bridge/engine/SendPliJob.cpp
        bridge/engine/SendPliJob.h
        bridge/engine/SendRtcpJob.cpp
        bridge/engine/SendRtcpJob.h
        bridge/engine/SimulcastLevel.h
        bridge/engine/SimulcastStream.h
        bridge/engine/SsrcInboundContext.h
        bridge/engine/SsrcOutboundContext.h
        bridge/engine/SsrcRewrite.h
        bridge/engine/SsrcWhitelist.h
        bridge/engine/UnackedPacketsTracker.h
        bridge/engine/VideoForwarderReceiveJob.cpp
        bridge/engine/VideoForwarderReceiveJob.h
        bridge/engine/VideoForwarderRewriteAndSendJob.cpp
        bridge/engine/VideoForwarderRewriteAndSendJob.h
        bridge/engine/VideoForwarderRtxReceiveJob.cpp
        bridge/engine/VideoForwarderRtxReceiveJob.h
        bridge/engine/VideoMissingPacketsTracker.h
        bridge/engine/VideoNackReceiveJob.cpp
        bridge/engine/VideoNackReceiveJob.h
        bridge/engine/Vp8Rewriter.h
        bwe/BandwidthEstimator.cpp
        bwe/BandwidthEstimator.h
        bwe/BandwidthUtils.cpp
        bwe/BandwidthUtils.h
        bwe/RateController.h
        bwe/RateController.cpp
        codec/AudioLevel.cpp
        codec/AudioLevel.h
        codec/Opus.h
        codec/OpusDecoder.cpp
        codec/OpusDecoder.h
        codec/OpusEncoder.cpp
        codec/OpusEncoder.h
        codec/Vp8Header.h
        concurrency/EventSemaphore.cpp
        concurrency/EventSemaphore.h
        concurrency/LockFreeList.cpp
        concurrency/LockFreeList.h
        concurrency/MpmcHashmap.cpp
        concurrency/MpmcHashmap.h
        concurrency/MpmcPublish.h
        concurrency/MpmcQueue.h
        concurrency/ScopedMutexGuard.h
        concurrency/ScopedSpinLocker.h
        concurrency/Semaphore.cpp
        concurrency/Semaphore.h
        concurrency/ThreadUtils.cpp
        concurrency/ThreadUtils.h
        concurrency/WaitFreeStack.cpp
        concurrency/WaitFreeStack.h
        config/Config.h
        config/ConfigReader.cpp
        config/ConfigReader.h
        crypto/AesGcmIvGenerator.cpp
        crypto/AesGcmIvGenerator.h
        crypto/SslHelper.cpp
        crypto/SslHelper.h
        external/nlohmann/json.hpp
        httpd/HttpRequestHandler.h
        httpd/Httpd.cpp
        httpd/Httpd.h
        httpd/Request.h
        httpd/RequestErrorException.h
        httpd/Response.h
        jobmanager/Job.h
        jobmanager/JobManager.h
        jobmanager/JobQueue.h
        jobmanager/TimerQueue.cpp
        jobmanager/TimerQueue.h
        jobmanager/WorkerThread.cpp
        jobmanager/WorkerThread.h
        legacyapi/Candidate.h
        legacyapi/Channel.h
        legacyapi/ChannelBundle.h
        legacyapi/Conference.h
        legacyapi/Connection.h
        legacyapi/Content.h
        legacyapi/DataChannelMessage.h
        legacyapi/DataChannelMessageParser.cpp
        legacyapi/DataChannelMessageParser.h
        legacyapi/Fingerprint.h
        legacyapi/Generator.cpp
        legacyapi/Generator.h
        legacyapi/Helpers.h
        legacyapi/Parser.cpp
        legacyapi/Parser.h
        legacyapi/PatchConferenceType.h
        legacyapi/PayloadType.h
        legacyapi/SctpConnection.h
        legacyapi/SsrcAttribute.cpp
        legacyapi/SsrcGroup.h
        legacyapi/Transport.h
        legacyapi/Validator.cpp
        legacyapi/Validator.h
        logger/Logger.cpp
        logger/Logger.h
        logger/LoggerThread.cpp
        logger/LoggerThread.h
        logger/PacketLogger.cpp
        logger/PacketLogger.h
        memory/List.h
        memory/Packet.h
        memory/PacketPoolAllocator.h
        memory/PoolAllocator.h
        memory/PriorityQueue.h
        memory/RefCountedPacket.h
        memory/RingAllocator.cpp
        memory/RingAllocator.h
        memory/RingBuffer.h
        rtp/RtcpFeedback.cpp
        rtp/RtcpFeedback.h
        rtp/RtcpHeader.cpp
        rtp/RtcpHeader.h
        rtp/RtcpIntervalCalculator.cpp
        rtp/RtcpIntervalCalculator.h
        rtp/RtcpNackBuilder.cpp
        rtp/RtcpNackBuilder.h
        rtp/RtpHeader.cpp
        rtp/RtpHeader.h
        rtp/SendTimeDial.cpp
        rtp/SendTimeDial.h
        test/macros.h
        transport/BaseUdpEndpoint.cpp
        transport/BaseUdpEndpoint.h
        transport/DataReceiver.h
        transport/DtlsJob.cpp
        transport/DtlsJob.h
        transport/Endpoint.h
        transport/IceJob.cpp
        transport/IceJob.h
        transport/RecordingEndpoint.cpp
        transport/RecordingEndpoint.h
        transport/RecordingTransport.cpp
        transport/RecordingTransport.h
        transport/RtcSocket.cpp
        transport/RtcSocket.h
        transport/RtcTransport.h
        transport/RtcePoll.cpp
        transport/RtcePoll.h
        transport/RtpReceiveState.cpp
        transport/RtpReceiveState.h
        transport/RtpSenderState.cpp
        transport/RtpSenderState.h
        transport/SctpJob.cpp
        transport/SctpJob.h
        transport/TcpEndpoint.cpp
        transport/TcpEndpoint.h
        transport/TcpServerEndpoint.cpp
        transport/TcpServerEndpoint.h
        transport/Transport.h
        transport/TransportFactory.cpp
        transport/TransportFactory.h
        transport/TransportImpl.cpp
        transport/TransportImpl.h
        transport/TransportStats.h
        transport/UdpEndpoint.cpp
        transport/UdpEndpoint.h
        transport/dtls/DtlsMessageListener.h
        transport/dtls/SrtpClient.cpp
        transport/dtls/SrtpClient.h
        transport/dtls/SrtpClientFactory.cpp
        transport/dtls/SrtpClientFactory.h
        transport/dtls/SslDtls.cpp
        transport/dtls/SslDtls.h
        transport/dtls/SslWriteBioListener.h
        transport/ice/IceCandidate.cpp
        transport/ice/IceCandidate.h
        transport/ice/IceComponent.h
        transport/ice/IceSession.cpp
        transport/ice/IceSession.h
        transport/ice/Stun.cpp
        transport/ice/Stun.h
        transport/recp/RecControlHeader.h
        transport/recp/RecDominantSpeakerEventBuilder.cpp
        transport/recp/RecStartStopEventBuilder.cpp
        transport/recp/RecStartStopEventBuilder.h
        transport/recp/RecStreamAddedEventBuilder.cpp
        transport/recp/RecStreamRemovedEventBuilder.cpp
        transport/sctp/SctpAssociation.h
        transport/sctp/SctpAssociationImpl.cpp
        transport/sctp/SctpAssociationImpl.h
        transport/sctp/SctpConfig.h
        transport/sctp/SctpServerPort.cpp
        transport/sctp/SctpServerPort.h
        transport/sctp/SctpTimer.cpp
        transport/sctp/SctpTimer.h
        transport/sctp/Sctprotocol.cpp
        transport/sctp/Sctprotocol.h
        utils/Base64.cpp
        utils/Base64.h
        utils/ByteOrder.h
        utils/CheckedCast.h
        utils/FowlerNollHash.cpp
        utils/FowlerNollHash.h
        utils/IdGenerator.h
        utils/MersienneRandom.h
        utils/Offset.h
        utils/Optional.h
        utils/OutboundSequenceNumber.h
        utils/Pacer.cpp
        utils/Pacer.h
        utils/ScopedFileHandle.h
        utils/ScopedInvariantChecker.h
        utils/ScopedReentrancyBlocker.h
        utils/SocketAddress.cpp
        utils/SocketAddress.h
        utils/SsrcGenerator.h
        utils/StringBuilder.h
        utils/StringTokenizer.h
        utils/Time.cpp
        utils/Time.h
        utils/Trackers.cpp
        utils/Trackers.h
        webrtc/DataChannel.cpp
        webrtc/DataChannel.h
        webrtc/WebRtcDataStream.cpp
        webrtc/WebRtcDataStream.h
        )

add_executable(smb
        ${FILES}
        main.cpp)
target_link_libraries(smb Threads::Threads)

##################
# Test framework
##################

if (CMAKE_VERSION VERSION_LESS 3.11.0)
    # Download and unpack googletest at configure time
    configure_file(CMakeLists-gtest.txt.in googletest-download/CMakeLists.txt)
    execute_process(COMMAND ${CMAKE_COMMAND} -G "${CMAKE_GENERATOR}" . RESULT_VARIABLE result WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/googletest-download )
    if(result)
        message(FATAL_ERROR "CMake step for googletest failed: ${result}")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} --build . RESULT_VARIABLE result WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/googletest-download )
    if(result)
        message(FATAL_ERROR "Build step for googletest failed: ${result}")
    endif()

    # Prevent overriding the parent project's compiler/linker
    # settings on Windows
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    # Add googletest directly to our build. This defines
    # the gtest and gtest_main targets.
    add_subdirectory(${CMAKE_CURRENT_BINARY_DIR}/googletest-src ${CMAKE_CURRENT_BINARY_DIR}/googletest-build EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(
            googletest
            # Specify the commit you depend on and update it regularly.
            URL https://github.com/google/googletest/archive/609281088cfefc76f9d0ce82e1ff6c30cc3591e5.zip
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

set(TEST_FILES
    test/memory/PoolAllocatorTest.cpp
    test/memory/RingAllocatorTest.cpp
    test/utils/StringTokenizerTest.cpp
    test/utils/TrackerTest.cpp
    test/memory/RingBufferTest.cpp
    test/memory/ListTest.cpp
    test/jobmanager/JobManagerTest.cpp
    test/concurrency/ProcessIntervalTest.cpp
#    test/integration/SampleDataUtils.cpp
#    test/integration/IntegrationTest.cpp
#    test/integration/IntegrationTest.h
#    test/integration/MixerTimeoutTest.cpp
#    test/integration/StatsTest.cpp
    test/sctp/SctpEndpoint.h
    test/sctp/SctpEndpoint.cpp
    test/sctp/SctpBasicsTests.cpp
    test/sctp/SctpTransferTests.cpp
    test/transport/SctpTest.cpp
    test/transport/RtcTransportTest.cpp
    test/transport/RtpTest.cpp
    test/transport/SrtpProtectJob.cpp
    test/transport/SrtpProtectJob.h
    test/transport/SrtpUnprotectJob.cpp
    test/transport/SrtpUnprotectJob.h
    test/transport/FakeNetwork.h
    test/transport/FakeNetwork.cpp
    test/transport/NetworkLink.h
    test/transport/NetworkLink.cpp
    test/transport/FakeUdpEndpoint.h
    test/transport/FakeUdpEndpoint.cpp
    test/transport/IceIntegrationTest.cpp
    test/transport/SctpIntegrationTest.cpp
    test/transport/TransportIntegrationTest.cpp
    test/transport/TransportIntegrationTest.h
    test/transport/SrtpTest.cpp
    test/transport/Ipv6Test.cpp
    test/integration/RtpDump.h
    test/integration/RtpDump.cpp
    test/config/ConfigTest.cpp
    test/codec/AudioProcessingTest.cpp
    test/gtest_main.cpp
    test/CsvWriter.h
    test/CsvWriter.cpp
    test/concurrency/MpscTest.cpp
    test/concurrency/MpmcMapTest.cpp
    test/concurrency/LockFreeListTest.cpp
    test/bwe/MatrixTests.cpp
    test/bwe/EstimatorTestEasy.cpp
    test/bwe/FakeAudioSource.h
    test/bwe/FakeAudioSource.cpp
    test/bwe/FakeVideoSource.h
    test/bwe/FakeVideoSource.cpp
    test/bwe/FakeCall.h
    test/bwe/FakeCall.cpp
    test/bwe/FakeMedia.h
    test/bwe/FakeMedia.cpp
    test/bwe/FakeCrossTraffic.h
    test/bwe/FakeCrossTraffic.cpp
    test/bwe/EstimatorReRun.cpp
    test/bwe/BwBurstTracker.h
    test/bwe/BwBurstTracker.cpp
    test/bwe/RateControllerTest.cpp
    test/transport/IceTest.cpp
    test/utils/Crc32Test.cpp
    test/utils/StringBuilderTest.cpp
    test/utils/RandGeneratorTest.cpp
    test/utils/TimeSourceTest.cpp
    test/legacyapi/ParserTest.cpp
    test/legacyapi/GeneratorTest.cpp
    test/bridge/EngineStreamDirectorTest.cpp
    test/codec/Vp8HeaderTest.cpp
    test/bridge/ActiveMediaListTest.cpp
    test/bridge/Vp8RewriterTest.cpp
    test/rtp/RtcpFeedbackTest.cpp
    test/bridge/PacketCacheTest.cpp
    test/rtp/RtcpNackBuilderTest.cpp
    test/rtp/SendTimeTest.cpp
    test/bridge/VideoMissingPacketsTrackerTest.cpp
    test/bwe/BandwidthUtilsTest.cpp
    test/utils/OutboundSequenceNumberTest.cpp
    test/crypto/AESTest.cpp
    test/utils/Base64Test.cpp
    test/crypto/AesIvGeneratorTest.cpp
    test/transport/RecordingTransportTest.cpp
    test/transport/EndpointListenerMock.h
    test/transport/recp/RecStartStopEventBuilderTest.cpp
    test/transport/recp/RecStreamAddedEventBuilderTest.cpp
    test/bridge/UnackedPacketsTrackerTest.cpp
    test/memory/PriorityQueueTest.cpp
    test/bridge/ActiveMediaListTestLevels.h)

add_executable(UnitTest
    ${FILES}
    ${TEST_FILES})

target_link_libraries(UnitTest gtest gmock)

add_test(AllUnitTests UnitTest)

if(APPLE)
    source_group(TREE ${PROJECT_SOURCE_DIR} FILES ${FILES} ${TEST_FILES})
endif()
