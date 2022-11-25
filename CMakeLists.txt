cmake_minimum_required(VERSION 3.6.2)
project(smb)

include(version/CheckGit.cmake)
CheckGitSetup()

option(ENABLE_LEGACY_API "ENABLE_LEGACY_API" OFF)
option(ENABLE_LIBATOMIC "ENABLE_LIBATOMIC" OFF)
option(ENABLE_ALLOCATOR_METRICS "Force enable the allocator metrics. It will be active by default in DEBUG" OFF)

set(ADDITIONAL_CXX_FLAGS, "")

if (ENABLE_LEGACY_API)
    set(ADDITIONAL_CXX_FLAGS "${ADDITIONAL_CXX_FLAGS} -DENABLE_LEGACY_API=1")
    message(STATUS "Enable Legacy API: ON")
else()
    set(ADDITIONAL_CXX_FLAGS "${ADDITIONAL_CXX_FLAGS} -DENABLE_LEGACY_API=0")
endif (ENABLE_LEGACY_API)

if (ENABLE_ALLOCATOR_METRICS)
    set(ADDITIONAL_CXX_FLAGS "${ADDITIONAL_CXX_FLAGS} -DENABLE_ALLOCATOR_METRICS=1")
    message(STATUS "Enable Allocator metrics: ON")
endif()

if (ENABLE_LIBATOMIC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -latomic")
    message(STATUS "Enable Atomic lib: ON")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS_DEBUG "-stdlib=libc++ -glldb -O0 -DDEBUG ${ADDITIONAL_CXX_FLAGS} -fno-rtti -Wall -Wuninitialized -Wsign-compare -Wthread-safety -Wno-missing-braces")
set(CMAKE_CXX_FLAGS_DCHECK "-stdlib=libc++ -glldb -O0 -DDEBUG ${ADDITIONAL_CXX_FLAGS} -DDCHECK_BUILD -DNOPERF_TEST -fno-rtti -Wall -Wuninitialized -Wsign-compare -Wthread-safety -Wno-missing-braces -fsanitize=address -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_TCHECK "-stdlib=libc++ -glldb -O1 -DDEBUG ${ADDITIONAL_CXX_FLAGS} -DTCHECK_BUILD -DNOPERF_TEST -fno-rtti -Wall -Wuninitialized -Wsign-compare -Wthread-safety -Wno-missing-braces -fsanitize=thread -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_LCHECK "-stdlib=libc++ -glldb -O1 -g -DDEBUG ${ADDITIONAL_CXX_FLAGS} -DLCHECK_BUILD -DNOPERF_TEST -fno-rtti -Wall -Wuninitialized -Wsign-compare -Wthread-safety -Wno-missing-braces -fsanitize=leak -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELEASE "-stdlib=libc++ -O3 -DNDEBUG ${ADDITIONAL_CXX_FLAGS} -fno-rtti -Wno-missing-braces -Wuninitialized -Wsign-compare")
set(CMAKE_C_FLAGS_DEBUG "-glldb -O0 -DDEBUG -Wall")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_SYMRELEASE "-stdlib=libc++ -glldb -O2 -DNDEBUG ${ADDITIONAL_CXX_FLAGS} -fno-rtti -fno-omit-frame-pointer -Wall -Wno-missing-braces -Wuninitialized -Wsign-compare")
set(CMAKE_CXX_FLAGS_LCOV "-stdlib=libc++ -glldb -O0 -DDEBUG -DLCOV_BUILD -DNOPERF_TEST ${ADDITIONAL_CXX_FLAGS} -fno-rtti -Wall -Wuninitialized -Wsign-compare -Wthread-safety -Wno-missing-braces -fno-omit-frame-pointer \
                          -fprofile-arcs -ftest-coverage -fprofile-instr-generate -fcoverage-mapping")

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

if(APPLE)
    message("MACOS detected")
    execute_process(COMMAND brew --prefix OUTPUT_VARIABLE BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(OPENSSL_ROOT_DIR "${BREW_PREFIX}/opt/openssl@1.1")
elseif(UNIX)
    message("UNIX detected")
    set(CMAKE_THREAD_LIBS_INIT "-lpthread")
    set(CMAKE_HAVE_THREADS_LIBRARY 1)
    set(CMAKE_USE_WIN32_THREADS_INIT 0)
    set(CMAKE_USE_PTHREADS_INIT 1)

    set(CMAKE_INSTALL_RPATH "./libs")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lc++abi -lrt")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")
    set(CMAKE_EXE_LINKER_FLAGS_SYMRELEASE "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")
    set(CMAKE_EXE_LINKER_FLAGS_DCHECK "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")
    set(CMAKE_EXE_LINKER_FLAGS_TCHECK "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")
else()
    message(FATAL_ERROR "Building on this platform is not supported.")
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Srtp2 REQUIRED)
find_package(Opus REQUIRED)
find_package(LibMicrohttpd REQUIRED)

include_directories(.)
include_directories(external)

list(APPEND THIRD_PARTY_LIBS
    OpenSSL::Crypto
    OpenSSL::SSL
    Opus::Opus
    Srtp2::Srtp
    LibMicrohttpd::Microhttpd
)

set(FILES
        api/AllocateConference.h
        api/AllocateEndpoint.h
        api/ConferenceEndpoint.h
        api/DataChannelMessage.h
        api/DataChannelMessageParser.cpp
        api/DataChannelMessageParser.h
        api/EndpointDescription.h
        api/RtcDescriptors.h
        api/RtcDescriptors.cpp
        api/Generator.cpp
        api/Generator.h
        api/Parser.cpp
        api/Parser.h
        api/Recording.h
        api/RecordingChannel.h
        api/utils.h
        api/utils.cpp
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
        bridge/RequestLogger.cpp
        bridge/RtpMap.h
        bridge/Stats.cpp
        bridge/Stats.h
        bridge/AudioStreamDescription.h
        bridge/VideoStreamDescription.h
        bridge/TransportDescription.h
        bridge/VideoStream.h
        bridge/endpointActions/ActionContext.h
        bridge/endpointActions/ApiActions.h
        bridge/endpointActions/GetConferenceInfo.cpp
        bridge/endpointActions/GetEndpointInfo.cpp
        bridge/endpointActions/GetProbingInfo.cpp
        bridge/endpointActions/AllocateConference.cpp
        bridge/endpointActions/ConferenceActions.cpp
        bridge/endpointActions/GetConferences.cpp
        bridge/endpointActions/About.cpp
        bridge/endpointActions/GetStats.cpp
        bridge/endpointActions/ApiHelpers.cpp
        bridge/endpointActions/ApiHelpers.h
        bridge/endpointActions/BarbellActions.cpp
        bridge/engine/ActiveMediaList.cpp
        bridge/engine/ActiveMediaList.h
        bridge/engine/ActiveTalker.h
        bridge/engine/AudioForwarderReceiveJob.cpp
        bridge/engine/AudioForwarderReceiveJob.h
        bridge/engine/AudioForwarderRewriteAndSendJob.cpp
        bridge/engine/AudioForwarderRewriteAndSendJob.h
        bridge/engine/EncodeJob.cpp
        bridge/engine/EncodeJob.h
        bridge/engine/Engine.cpp
        bridge/engine/Engine.h
        bridge/engine/EngineAudioStream.h
        bridge/engine/EngineDataStream.h
        bridge/engine/EngineMixer.cpp
        bridge/engine/EngineMixer.h
        bridge/engine/EngineStats.h
        bridge/engine/EngineStreamDirector.h
        bridge/engine/EngineStreamDirector.cpp
        bridge/engine/EngineVideoStream.h
        bridge/engine/EngineBarbell.h
        bridge/engine/EngineBarbell.cpp
        bridge/engine/PacketCache.cpp
        bridge/engine/PacketCache.h
        bridge/engine/ProcessMissingVideoPacketsJob.cpp
        bridge/engine/ProcessMissingVideoPacketsJob.h
        bridge/engine/ProcessUnackedRecordingEventPacketsJob.cpp
        bridge/engine/ProcessUnackedRecordingEventPacketsJob.h
        bridge/engine/RecordingAudioForwarderSendJob.cpp
        bridge/engine/RecordingAudioForwarderSendJob.h
        bridge/engine/RecordingVideoForwarderSendJob.h
        bridge/engine/RecordingVideoForwarderSendJob.cpp
        bridge/engine/RecordingEventAckReceiveJob.cpp
        bridge/engine/RecordingEventAckReceiveJob.h
        bridge/engine/RecordingRtpNackReceiveJob.cpp
        bridge/engine/RecordingRtpNackReceiveJob.h
        bridge/engine/RecordingSendEventJob.cpp
        bridge/engine/SendPliJob.cpp
        bridge/engine/SendPliJob.h
        bridge/engine/SendRtcpJob.cpp
        bridge/engine/SendRtcpJob.h
        bridge/engine/SimulcastLevel.h
        bridge/engine/SimulcastStream.h
        bridge/engine/SsrcInboundContext.h
        bridge/engine/SsrcOutboundContext.h
        bridge/engine/SsrcOutboundContext.cpp
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
        bridge/engine/DiscardReceivedVideoPacketJob.h
        bridge/engine/DiscardReceivedVideoPacketJob.cpp
        bridge/engine/Vp8Rewriter.h
        bridge/engine/SetMaxMediaBitrateJob.h
        bridge/engine/SetMaxMediaBitrateJob.cpp
        bridge/MixerJobs.h
        bridge/MixerJobs.cpp
        bridge/MixerManagerAsync.h
        bridge/MixerManagerAsync.cpp
        bwe/BandwidthEstimator.cpp
        bwe/BandwidthEstimator.h
        bwe/BandwidthUtils.cpp
        bwe/BandwidthUtils.h
        bwe/RateController.cpp
        bwe/RateController.h
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
        concurrency/CountdownEvent.cpp
        concurrency/CountdownEvent.h
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
        httpd/HttpdFactory.h
        httpd/HttpdFactory.cpp
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
        memory/AudioPacketPoolAllocator.h
        memory/PoolAllocator.h
        memory/PriorityQueue.h
        memory/RingAllocator.cpp
        memory/RingAllocator.h
        memory/RingBuffer.h
        memory/MemoryFile.h
        memory/MemoryFile.cpp
        memory/Map.h
        memory/Array.h
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
        transport/UdpEndpoint.h
        transport/BaseUdpEndpoint.cpp
        transport/BaseUdpEndpoint.h
        transport/DataReceiver.h
        transport/DtlsJob.cpp
        transport/DtlsJob.h
        transport/Endpoint.h
        transport/IceJob.cpp
        transport/IceJob.h
        transport/ProbeServer.cpp
        transport/ProbeServer.h
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
        transport/EndpointFactory.h
        transport/EndpointFactoryImpl.h
        transport/EndpointFactoryImpl.cpp
        transport/RtcpReportProducer.cpp
        transport/RtcpReportProducer.h
        transport/TransportFactory.cpp
        transport/TransportFactory.h
        transport/TransportImpl.cpp
        transport/TransportImpl.h
        transport/TransportStats.h
        transport/UdpEndpointImpl.cpp
        transport/UdpEndpointImpl.h
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
        transport/ice/IceSerialize.h
        transport/ice/IceSerialize.cpp
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
        utils/Optional.h
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
        utils/SimpleJson.cpp
        utils/SimpleJson.h
        webrtc/DataChannel.cpp
        webrtc/DataChannel.h
        webrtc/WebRtcDataStream.cpp
        webrtc/WebRtcDataStream.h
        )

add_library(smblib STATIC ${FILES})
target_include_directories(smblib PUBLIC ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(smblib PUBLIC ${THIRD_PARTY_LIBS})

add_executable(smb
        main.cpp)

target_link_libraries(smb smblib Threads::Threads git_version)

##################
# Test framework
##################

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

set(TEST_LIB_FILES
    test/integration/SampleDataUtils.cpp
    test/integration/FFTanalysis.h
    test/integration/FFTanalysis.cpp
    test/sctp/SctpEndpoint.h
    test/sctp/SctpEndpoint.cpp
    test/transport/SrtpProtectJob.cpp
    test/transport/SrtpProtectJob.h
    test/transport/SrtpUnprotectJob.cpp
    test/transport/SrtpUnprotectJob.h
    test/transport/FakeNetwork.h
    test/transport/FakeNetwork.cpp
    test/transport/NetworkLink.h
    test/transport/NetworkLink.cpp
    test/integration/RtpDump.h
    test/integration/RtpDump.cpp
    test/integration/emulator/AudioSource.cpp
    test/integration/emulator/AudioSource.h
    test/integration/emulator/ApiChannel.h
    test/integration/emulator/ApiChannel.cpp
    test/integration/emulator/FakeVideoDecoder.h
    test/integration/emulator/FakeVideoDecoder.cpp
    test/integration/emulator/SfuClient.h
    test/integration/emulator/HttpRequests.h
    test/integration/emulator/HttpRequests.cpp
    test/integration/emulator/TimeTurner.h
    test/integration/emulator/TimeTurner.cpp
    test/integration/emulator/FakeEndpointFactory.h
    test/integration/emulator/FakeEndpointFactory.cpp
    test/integration/emulator/FakeUdpEndpoint.h
    test/integration/emulator/FakeUdpEndpoint.cpp
    test/integration/emulator/Httpd.cpp
    test/CsvWriter.h
    test/CsvWriter.cpp
    test/ResourceLoader.cpp
    test/bwe/FakeAudioSource.h
    test/bwe/FakeAudioSource.cpp
    test/bwe/FakeVideoSource.h
    test/bwe/FakeVideoSource.cpp
    test/bwe/FakeCall.h
    test/bwe/FakeCall.cpp
    test/bwe/RcCall.h
    test/bwe/RcCall.cpp
    test/bwe/FakeMedia.h
    test/bwe/FakeMedia.cpp
    test/bwe/FakeCrossTraffic.h
    test/bwe/FakeCrossTraffic.cpp
    test/bwe/EstimatorReRun.cpp
    test/bwe/BwBurstTracker.h
    test/bwe/BwBurstTracker.cpp
    test/utils/ApiUtils.cpp
    test/transport/EndpointListenerMock.h
    test/bridge/DummyRtcTransport.h
)

set(TEST_FILES
    test/api/ParserTest.cpp
    test/memory/MapTest.cpp
    test/memory/PoolAllocatorTest.cpp
    test/memory/RingAllocatorTest.cpp
    test/utils/StringTokenizerTest.cpp
    test/utils/TrackerTest.cpp
    test/memory/RingBufferTest.cpp
    test/memory/ListTest.cpp
    test/memory/ArrayTest.cpp
    test/jobmanager/JobManagerTest.cpp
    test/concurrency/ProcessIntervalTest.cpp
    test/integration/IntegrationTest.cpp
    test/integration/IntegrationTest.h
    test/integration/RealTimeTest.cpp
    test/integration/RealTimeTest.h
    test/integration/BarbellTest.cpp
    test/integration/BarbellTest.h
#    test/integration/MixerTimeoutTest.cpp
#    test/integration/StatsTest.cpp
    test/sctp/SctpBasicsTests.cpp
    test/sctp/SctpTransferTests.cpp
    test/transport/ice/IceCandidateTest.cpp
    test/transport/SctpTest.cpp
    test/transport/RtcpReportsProducerTest.cpp
    test/transport/RtcTransportTest.cpp
    test/transport/RtpTest.cpp
    test/transport/IceIntegrationTest.cpp
    test/transport/SctpIntegrationTest.cpp
    test/transport/TransportIntegrationTest.cpp
    test/transport/TransportIntegrationTest.h
    test/transport/SrtpTest.cpp
    test/transport/Ipv6Test.cpp
    test/integration/TimeTurnerTest.cpp
    test/config/ConfigTest.cpp
    test/codec/AudioProcessingTest.cpp
    test/concurrency/MpscTest.cpp
    test/concurrency/MpmcMapTest.cpp
    test/concurrency/LockFreeListTest.cpp
    test/bwe/MatrixTests.cpp
    test/bwe/RateControllerTest.cpp
    test/transport/IceTest.cpp
    test/utils/Crc32Test.cpp
    test/utils/StringBuilderTest.cpp
    test/utils/RandGeneratorTest.cpp
    test/utils/TimeSourceTest.cpp
    test/utils/ApiUtils.cpp
    test/utils/SimpleJsonTest.cpp
    test/legacyapi/ParserTest.cpp
    test/legacyapi/GeneratorTest.cpp
    test/bridge/EngineStreamDirectorTest.cpp
    test/codec/Vp8HeaderTest.cpp
    test/bridge/ActiveMediaListTest.cpp
    test/bridge/BarbellMessagesTest.cpp
    test/bridge/Vp8RewriterTest.cpp
    test/rtp/RtcpFeedbackTest.cpp
    test/bridge/PacketCacheTest.cpp
    test/rtp/RtcpNackBuilderTest.cpp
    test/rtp/SendTimeTest.cpp
    test/bridge/VideoMissingPacketsTrackerTest.cpp
    test/bwe/BandwidthUtilsTest.cpp
    test/crypto/AESTest.cpp
    test/utils/Base64Test.cpp
    test/crypto/AesIvGeneratorTest.cpp
    test/transport/RecordingTransportTest.cpp
    test/transport/recp/RecStartStopEventBuilderTest.cpp
    test/transport/recp/RecStreamAddedEventBuilderTest.cpp
    test/bridge/UnackedPacketsTrackerTest.cpp
    test/memory/PriorityQueueTest.cpp
    test/memory/BacklogTest.cpp
    test/memory/StackMapTest.cpp
    test/bridge/ActiveMediaListTestLevels.h
    test/bridge/VideoNackReceiveJobTest.cpp
    test/utils/LogSpamTest.cpp
    test/utils/FunctionTest.cpp)

set(LOAD_TEST_FILES
    test/integration/RealTimeTest.cpp
    test/integration/RealTimeTest.h
    test/integration/LoadTestConfig.h
)


set(CMAKE_TEST_DIRECTORY "${CMAKE_BINARY_DIR}/test")
set(CMAKE_TEST_INCLUDE "${CMAKE_TEST_DIRECTORY}/include")
configure_file(test/TestConfig.h.in ${CMAKE_TEST_INCLUDE}/TestConfig.h)

add_library(testlib STATIC ${TEST_LIB_FILES})
target_include_directories(testlib PRIVATE ${CMAKE_TEST_INCLUDE} ${gtest_SOURCE_DIR}/include "test/include")
target_link_libraries(testlib PUBLIC smblib ${THIRD_PARTY_LIBS})

add_executable(UnitTest
    ${TEST_FILES} test/gtest_main.cpp)

target_include_directories(UnitTest PRIVATE ${CMAKE_TEST_INCLUDE} "test/include")

# Copy test Data
file(COPY "${CMAKE_SOURCE_DIR}/test/resources" DESTINATION "${CMAKE_TEST_DIRECTORY}")

target_link_libraries(UnitTest testlib gtest gmock git_version)

add_test(AllUnitTests UnitTest)

add_executable(LoadTest ${LOAD_TEST_FILES}
    test/load_test_main.cpp)

target_link_libraries(LoadTest testlib gtest gmock git_version)

if(APPLE)
    source_group(TREE ${PROJECT_SOURCE_DIR} FILES ${FILES} ${TEST_FILES})
endif()

install(TARGETS smb CONFIGURATIONS Release DESTINATION bin)
