include(FetchContent)

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

include_directories(${gtest_SOURCE_DIR}/include)
include_directories(${gmock_SOURCE_DIR}/include)
include_directories(..)

set(TEST_LIB_FILES
    integration/SampleDataUtils.cpp
    integration/FFTanalysis.h
    integration/FFTanalysis.cpp
    sctp/SctpEndpoint.h
    sctp/SctpEndpoint.cpp
    transport/SrtpUnprotectJob.cpp
    transport/SrtpUnprotectJob.h
    transport/FakeNetwork.h
    transport/FakeNetwork.cpp
    transport/NetworkLink.h
    transport/NetworkLink.cpp
    integration/RtpDump.h
    integration/RtpDump.cpp
    integration/IntegrationTest.cpp
    integration/IntegrationTest.h
    integration/emulator/AudioSource.cpp
    integration/emulator/AudioSource.h
    integration/emulator/ApiChannel.h
    integration/emulator/ApiChannel.cpp
    integration/emulator/BaseChannel.h
    integration/emulator/BaseChannel.cpp
    integration/emulator/ColibriChannel.h
    integration/emulator/ColibriChannel.cpp
    integration/emulator/FakeVideoDecoder.h
    integration/emulator/FakeVideoDecoder.cpp
    integration/emulator/SfuClient.h
    integration/emulator/SfuClientReceivers.h
    integration/emulator/SfuGroupCall.h
    integration/emulator/Conference.cpp
    integration/emulator/Barbell.cpp
    integration/emulator/HttpRequests.h
    integration/emulator/HttpRequests.cpp
    integration/emulator/TimeTurner.h
    integration/emulator/TimeTurner.cpp
    integration/emulator/FakeEndpointFactory.h
    integration/emulator/FakeEndpointFactory.cpp
    integration/emulator/FakeEndpointImpl.h
    integration/emulator/FakeEndpointImpl.cpp
    integration/emulator/FakeUdpEndpoint.h
    integration/emulator/FakeUdpEndpoint.cpp
    integration/emulator/FakeTcpEndpoint.h
    integration/emulator/FakeTcpEndpoint.cpp
    integration/emulator/FakeTcpServerEndpoint.h
    integration/emulator/FakeTcpServerEndpoint.cpp
    integration/emulator/Httpd.cpp
    integration/emulator/JitterPacketSource.h
    integration/emulator/JitterPacketSource.cpp
    CsvWriter.h
    CsvWriter.cpp
    ResourceLoader.cpp
    bwe/FakeAudioSource.h
    bwe/FakeAudioSource.cpp
    bwe/FakeVideoSource.h
    bwe/FakeVideoSource.cpp
    bwe/FakeCall.h
    bwe/FakeCall.cpp
    bwe/RcCall.h
    bwe/RcCall.cpp
    bwe/FakeMedia.h
    bwe/FakeMedia.cpp
    bwe/FakeCrossTraffic.h
    bwe/FakeCrossTraffic.cpp
    bwe/BwBurstTracker.h
    bwe/BwBurstTracker.cpp
    utils/ApiUtils.cpp
    bridge/DummyRtcTransport.h
)

set(TEST_FILES
    api/ParserTest.cpp
    memory/MapTest.cpp
    memory/PoolAllocatorTest.cpp
    memory/RingAllocatorTest.cpp
    utils/StringTokenizerTest.cpp
    utils/TrackerTest.cpp
    utils/StdExtensionsTest.cpp
    utils/SocketAddressTest.cpp
    memory/ListTest.cpp
    memory/ArrayTest.cpp
    jobmanager/JobManagerTest.cpp
    jobmanager/JobTest.cpp
    codec/OpusCodecTest.cpp
    concurrency/ProcessIntervalTest.cpp

    sctp/SctpBasicsTests.cpp
    sctp/SctpTransferTests.cpp
    transport/ice/IceCandidateTest.cpp
    transport/SctpTest.cpp
    transport/RtcpReportsProducerTest.cpp
    transport/RtcTransportTest.cpp
    transport/RtpTest.cpp
    transport/IceIntegrationTest.cpp
    transport/SctpIntegrationTest.cpp
    transport/TransportIntegrationTest.cpp
    transport/TransportIntegrationTest.h
    transport/SrtpTest.cpp
    transport/Ipv6Test.cpp
    transport/JitterTest.cpp
    transport/AdaptiveJitterTest.cpp
    integration/TimeTurnerTest.cpp
    config/ConfigTest.cpp
    codec/AudioProcessingTest.cpp
    concurrency/MpscTest.cpp
    concurrency/MpmcMapTest.cpp
    concurrency/LockFreeListTest.cpp
    bwe/MatrixTests.cpp
    bwe/RateControllerTest.cpp
    transport/IceTest.cpp
    utils/Crc32Test.cpp
    utils/StringBuilderTest.cpp
    utils/RandGeneratorTest.cpp
    utils/TimeSourceTest.cpp
    utils/ApiUtils.cpp
    utils/SimpleJsonTest.cpp
    legacyapi/ParserTest.cpp
    legacyapi/GeneratorTest.cpp
    math/FieldsTest.cpp
    bridge/EngineStreamDirectorTest.cpp
    codec/H264HeaderTest.cpp
    codec/Vp8HeaderTest.cpp
    bridge/ActiveMediaListTest.cpp
    bridge/BarbellMessagesTest.cpp
    rtp/RtcpFeedbackTest.cpp
    bridge/PacketCacheTest.cpp
    bridge/SsrcOutboundContextTest.cpp
    rtp/RtcpNackBuilderTest.cpp
    rtp/SendTimeTest.cpp
    bridge/VideoMissingPacketsTrackerTest.cpp
    bwe/BandwidthUtilsTest.cpp
    bwe/EstimatorTestEasy.cpp
    bwe/EstimatorReRun.cpp
    crypto/AESTest.cpp
    utils/Base64Test.cpp
    crypto/AesIvGeneratorTest.cpp
    transport/RecordingTransportTest.cpp
    transport/recp/RecStartStopEventBuilderTest.cpp
    transport/recp/RecStreamAddedEventBuilderTest.cpp
    bridge/UnackedPacketsTrackerTest.cpp
    bridge/VideoForwarderRtxReceiveJobTest.cpp
    memory/PriorityQueueTest.cpp
    memory/BacklogTest.cpp
    memory/StackMapTest.cpp
    bridge/ActiveMediaListTestLevels.h
    bridge/MixerTest.cpp
    bridge/VideoNackReceiveJobTest.cpp
    utils/LogSpamTest.cpp
    utils/FunctionTest.cpp
    transport/JitterTest.cpp)


set(TEST_FILES2
    integration/RealTimeTest.cpp
    integration/RealTimeTest.h
    integration/BarbellTest.cpp
    integration/BarbellTest.h
    integration/ConfIntegrationTest.cpp
    integration/IntegrationLegApiTest.cpp
    integration/IceTransportTest.cpp
    integration/SrtpIntegrationTest.cpp
    integration/IntegrationCallTypes.cpp
    integration/IntegrationAudioTest.cpp
    integration/FFTtest.cpp
)

set(LOAD_TEST_FILES
    integration/IntegrationTest.cpp
    integration/IntegrationTest.h
    integration/RealTimeTest.cpp
    integration/RealTimeTest.h
    integration/LoadTestConfig.h
)


add_library(testlib STATIC ${TEST_LIB_FILES})
target_include_directories(testlib PRIVATE ${CMAKE_TEST_INCLUDE} ${gtest_SOURCE_DIR}/include "include")
target_link_libraries(testlib PUBLIC smblib ${THIRD_PARTY_LIBS})


add_executable(UnitTest
    ${TEST_FILES} gtest_main.cpp)

target_include_directories(UnitTest PRIVATE ${CMAKE_TEST_INCLUDE} "./include")


# Copy test Data
file(COPY "${CMAKE_SOURCE_DIR}/test/resources" DESTINATION "${CMAKE_TEST_DIRECTORY}")

target_link_libraries(UnitTest testlib gtest gmock)

add_executable(UnitTest2
    ${TEST_FILES2} gtest_main2.cpp)

target_include_directories(UnitTest2 PRIVATE ${CMAKE_TEST_INCLUDE} "./include")

# Copy test Data
file(COPY "${CMAKE_SOURCE_DIR}/test/resources" DESTINATION "${CMAKE_TEST_DIRECTORY}")

target_link_libraries(UnitTest2 testlib gtest gmock)


add_test(AllUnitTests UnitTest)
add_test(AllUnitTests UnitTest2)

add_executable(LoadTest ${LOAD_TEST_FILES}
    load_test_main.cpp)

target_link_libraries(LoadTest testlib gtest gmock)

