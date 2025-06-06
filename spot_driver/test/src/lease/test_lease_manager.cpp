// Copyright (c) 2025 Boston Dynamics AI Institute LLC. All rights reserved.

#include <gmock/gmock.h>

#include <memory>

#include <bosdyn/client/lease/lease_resources.h>

#include <bosdyn_api_msgs/conversions.hpp>

#include <spot_driver/lease/lease_manager.hpp>
#include <spot_driver/mock/mock_lease_client.hpp>
#include <spot_driver/mock/mock_logger_interface.hpp>
#include <spot_driver/mock/mock_timer_interface.hpp>

#include <tl_expected/expected.hpp>

namespace spot_ros2::lease::test {

using LeaseUseCallback = LeaseClientInterface::LeaseUseCallback;

using ::bosdyn::client::kArmResource;
using ::bosdyn::client::kBodyResource;

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::IsEmpty;
using ::testing::Return;

class MockMiddlewareHandle : public LeaseManager::MiddlewareHandle {
 public:
  MOCK_METHOD(
      (void), createClaimLeasesService,
      (const std::string& serviceName,
       std::function<void(const std::shared_ptr<Trigger::Request>, std::shared_ptr<Trigger::Response>)> callback),
      (override));

  MOCK_METHOD(
      (void), createAcquireLeaseService,
      (const std::string& serviceName,
       std::function<void(const std::shared_ptr<AcquireLease::Request>, std::shared_ptr<AcquireLease::Response>)>
           callback),
      (override));

  MOCK_METHOD((void), createReturnLeaseService,
              (const std::string& serviceName,
               std::function<void(const std::shared_ptr<ReturnLease::Request>, std::shared_ptr<ReturnLease::Response>)>
                   callback),
              (override));

  MOCK_METHOD(
      (void), createReleaseLeasesService,
      (const std::string& serviceName,
       std::function<void(const std::shared_ptr<Trigger::Request>, std::shared_ptr<Trigger::Response>)> callback),
      (override));

  MOCK_METHOD(std::shared_ptr<LeaseManager::MiddlewareHandle::Bond>, createBond,
              (const std::string& nodeName, std::function<void()> break_callback), (override));

  MOCK_METHOD((void), publishLeaseArray, (std::unique_ptr<LeaseArray> message), (override));
};

/**
 * Test lease manager initialization.
 */
TEST(TestLeaseManager, initializationIsComplete) {
  auto lease_client = std::make_unique<spot_ros2::test::MockLeaseClient>();
  auto timer_interface = std::make_unique<spot_ros2::test::MockTimerInterface>();
  auto logger_interface = std::make_unique<spot_ros2::test::MockLoggerInterface>();
  auto middleware_handle = std::make_unique<MockMiddlewareHandle>();
  EXPECT_CALL(*middleware_handle, createClaimLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createAcquireLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReturnLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReleaseLeasesService(_, _)).Times(1);
  EXPECT_CALL(*timer_interface, setTimer(_, _)).Times(1);
  constexpr double kLeaseCheckRate = 1.0;  // Hz
  auto lease_manager =
      std::make_unique<LeaseManager>(std::move(lease_client), std::move(logger_interface), std::move(timer_interface),
                                     std::move(middleware_handle), kLeaseCheckRate);
  lease_manager->initialize();
}

/**
 * Test a single body sublease succeeds.
 */
TEST(TestLeaseManager, simpleLeasingSucceeds) {
  // GIVEN body lease acquisition succeeds the first time it is called
  auto lease_client = std::make_unique<spot_ros2::test::MockLeaseClient>();

  auto lease_wallet = std::make_shared<::bosdyn::client::LeaseWallet>("test_manager");
  EXPECT_CALL(*lease_client, getLeaseWallet()).WillOnce(Return(lease_wallet));

  EXPECT_CALL(*lease_client, getResourceHierarchy()).WillOnce([]() -> const ::bosdyn::client::ResourceHierarchy& {
    const auto requirement = ::bosdyn::client::LeaseHierarchyRequirements::ARM_AND_GRIPPER;
    return ::bosdyn::client::DefaultResourceHierarchy(requirement);
  });

  auto lease_proto = ::bosdyn::api::Lease();
  lease_proto.set_resource(kBodyResource);
  lease_proto.add_sequence(0);
  auto lease = ::bosdyn::client::Lease(lease_proto);

  EXPECT_CALL(*lease_client, acquireLease(kBodyResource, _)).WillOnce([&](const std::string&, LeaseUseCallback) {
    lease_wallet->AddLease(lease);
    return lease;
  });

  auto timer_interface = std::make_unique<spot_ros2::test::MockTimerInterface>();
  EXPECT_CALL(*timer_interface, setTimer(_, _)).Times(1);
  auto logger_interface = std::make_unique<spot_ros2::test::MockLoggerInterface>();
  EXPECT_CALL(*logger_interface, logDebug(_)).Times(AnyNumber());
  EXPECT_CALL(*logger_interface, logInfo(_)).Times(AnyNumber());
  auto middleware_handle = std::make_unique<MockMiddlewareHandle>();
  EXPECT_CALL(*middleware_handle, createClaimLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createAcquireLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReturnLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReleaseLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createBond(_, _)).Times(1);
  constexpr double kLeaseCheckRate = 1.0;  // Hz

  auto lease_manager =
      std::make_unique<LeaseManager>(std::move(lease_client), std::move(logger_interface), std::move(timer_interface),
                                     std::move(middleware_handle), kLeaseCheckRate);
  lease_manager->initialize();

  // WHEN a sublease request is placed
  const std::string kClientNodeName = "dummy_node";
  auto request = std::make_shared<AcquireLease::Request>();
  request->resource_name = kBodyResource;
  request->client_name = kClientNodeName;
  auto response = std::make_shared<AcquireLease::Response>();
  lease_manager->acquireLease(request, response);

  // THEN the response should bear a valid sublease
  ASSERT_TRUE(response->success);
  auto sublease_proto = ::bosdyn::api::Lease();
  bosdyn_api_msgs::conversions::Convert(response->lease, &sublease_proto);
  const auto sublease = ::bosdyn::client::Lease(sublease_proto);
  ASSERT_TRUE(sublease.IsValid());
  ASSERT_EQ(sublease.GetResource(), kBodyResource);
  using CompareResult = ::bosdyn::client::Lease::CompareResult;
  ASSERT_EQ(lease.Compare(sublease), CompareResult::SUPER_LEASE);
  ASSERT_GT(sublease_proto.client_names_size(), 0);
  ASSERT_EQ(*sublease_proto.client_names().rbegin(), kClientNodeName);
}

/**
 * Test body can be subleased multiple times in sequence.
 */
TEST(TestLeaseManager, sequentialLeasingSucceeds) {
  const std::string kClientNodeName = "dummy_node";

  // GIVEN body lease acquisition succeeds the first time it is called
  auto lease_client = std::make_unique<spot_ros2::test::MockLeaseClient>();

  auto lease_wallet = std::make_shared<::bosdyn::client::LeaseWallet>("test_manager");
  EXPECT_CALL(*lease_client, getLeaseWallet()).WillRepeatedly(Return(lease_wallet));

  EXPECT_CALL(*lease_client, getResourceHierarchy()).WillRepeatedly([]() -> const ::bosdyn::client::ResourceHierarchy& {
    const auto requirement = ::bosdyn::client::LeaseHierarchyRequirements::ARM_AND_GRIPPER;
    return ::bosdyn::client::DefaultResourceHierarchy(requirement);
  });

  auto lease_proto = ::bosdyn::api::Lease();
  lease_proto.set_resource(kBodyResource);
  lease_proto.add_sequence(0);
  auto lease = ::bosdyn::client::Lease(lease_proto);

  EXPECT_CALL(*lease_client, acquireLease(kBodyResource, _)).WillOnce([&](const std::string&, LeaseUseCallback) {
    lease_wallet->AddLease(lease);
    return lease;
  });

  auto timer_interface = std::make_unique<spot_ros2::test::MockTimerInterface>();
  EXPECT_CALL(*timer_interface, setTimer(_, _)).Times(1);
  auto logger_interface = std::make_unique<spot_ros2::test::MockLoggerInterface>();
  EXPECT_CALL(*logger_interface, logDebug(_)).Times(AnyNumber());
  EXPECT_CALL(*logger_interface, logInfo(_)).Times(AnyNumber());
  auto middleware_handle = std::make_unique<MockMiddlewareHandle>();
  EXPECT_CALL(*middleware_handle, createClaimLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createAcquireLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReturnLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReleaseLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createBond(_, _)).Times(2);
  constexpr double kLeaseCheckRate = 1.0;  // Hz

  auto lease_manager =
      std::make_unique<LeaseManager>(std::move(lease_client), std::move(logger_interface), std::move(timer_interface),
                                     std::move(middleware_handle), kLeaseCheckRate);
  lease_manager->initialize();

  ::bosdyn::client::Lease first_sublease;
  {
    // WHEN a sublease acquisition request is placed
    auto request = std::make_shared<AcquireLease::Request>();
    request->resource_name = kBodyResource;
    request->client_name = kClientNodeName;
    auto response = std::make_shared<AcquireLease::Response>();
    lease_manager->acquireLease(request, response);

    // THEN the response should bear a valid sublease
    ASSERT_TRUE(response->success);
    auto sublease_proto = ::bosdyn::api::Lease();
    bosdyn_api_msgs::conversions::Convert(response->lease, &sublease_proto);
    first_sublease = ::bosdyn::client::Lease(sublease_proto);
    ASSERT_TRUE(first_sublease.IsValid());
    ASSERT_EQ(first_sublease.GetResource(), kBodyResource);
    using CompareResult = ::bosdyn::client::Lease::CompareResult;
    ASSERT_EQ(lease.Compare(first_sublease), CompareResult::SUPER_LEASE);
    ASSERT_GT(sublease_proto.client_names_size(), 0);
    ASSERT_EQ(*sublease_proto.client_names().rbegin(), kClientNodeName);
  }

  {
    // WHEN a sublease return request is placed
    auto request = std::make_shared<ReturnLease::Request>();
    bosdyn_api_msgs::conversions::Convert(first_sublease.GetProto(), &request->lease);
    auto response = std::make_shared<ReturnLease::Response>();
    lease_manager->returnLease(request, response);

    // THEN the response should indicate success
    ASSERT_TRUE(response->success) << response->message;
  }

  ::bosdyn::client::Lease second_sublease;
  {
    // WHEN a second sublease acquisition request is placed
    auto request = std::make_shared<AcquireLease::Request>();
    request->resource_name = kBodyResource;
    request->client_name = kClientNodeName;
    auto response = std::make_shared<AcquireLease::Response>();
    lease_manager->acquireLease(request, response);

    // THEN the response should bear a valid sublease
    ASSERT_TRUE(response->success);
    auto sublease_proto = ::bosdyn::api::Lease();
    bosdyn_api_msgs::conversions::Convert(response->lease, &sublease_proto);
    second_sublease = ::bosdyn::client::Lease(sublease_proto);
    ASSERT_TRUE(second_sublease.IsValid());
    ASSERT_EQ(second_sublease.GetResource(), kBodyResource);
    using CompareResult = ::bosdyn::client::Lease::CompareResult;
    ASSERT_EQ(lease.Compare(second_sublease), CompareResult::SUPER_LEASE);
    ASSERT_GT(sublease_proto.client_names_size(), 0);
    ASSERT_EQ(*sublease_proto.client_names().rbegin(), kClientNodeName);
  }

  using CompareResult = ::bosdyn::client::Lease::CompareResult;
  ASSERT_EQ(first_sublease.Compare(second_sublease), CompareResult::OLDER);
}

/**
 * Test body may not be subleased twice at the same time.
 */
TEST(TestLeaseManager, doubleLeasingFails) {
  // GIVEN a body lease is held by the lease manager
  auto lease_client = std::make_unique<spot_ros2::test::MockLeaseClient>();
  auto lease_wallet = std::make_shared<::bosdyn::client::LeaseWallet>("test_manager");
  EXPECT_CALL(*lease_client, getLeaseWallet()).WillRepeatedly(Return(lease_wallet));

  EXPECT_CALL(*lease_client, getResourceHierarchy()).WillRepeatedly([]() -> const ::bosdyn::client::ResourceHierarchy& {
    const auto requirement = ::bosdyn::client::LeaseHierarchyRequirements::ARM_AND_GRIPPER;
    return ::bosdyn::client::DefaultResourceHierarchy(requirement);
  });

  auto timer_interface = std::make_unique<spot_ros2::test::MockTimerInterface>();
  EXPECT_CALL(*timer_interface, setTimer(_, _)).Times(1);
  auto logger_interface = std::make_unique<spot_ros2::test::MockLoggerInterface>();
  EXPECT_CALL(*logger_interface, logDebug(_)).Times(AnyNumber());
  EXPECT_CALL(*logger_interface, logInfo(_)).Times(AnyNumber());
  auto middleware_handle = std::make_unique<MockMiddlewareHandle>();
  EXPECT_CALL(*middleware_handle, createClaimLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createAcquireLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReturnLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReleaseLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createBond(_, _)).Times(1);
  constexpr double kLeaseCheckRate = 1.0;  // Hz

  auto lease_manager =
      std::make_unique<LeaseManager>(std::move(lease_client), std::move(logger_interface), std::move(timer_interface),
                                     std::move(middleware_handle), kLeaseCheckRate);
  lease_manager->initialize();

  auto lease_proto = ::bosdyn::api::Lease();
  lease_proto.set_resource(kBodyResource);
  lease_proto.add_sequence(0);
  auto lease = ::bosdyn::client::Lease(lease_proto);

  lease_wallet->AddLease(lease);

  {
    // WHEN a first sublease request is placed
    const std::string kClientNodeName = "dummy_node";
    auto request = std::make_shared<AcquireLease::Request>();
    request->resource_name = kBodyResource;
    request->client_name = kClientNodeName;
    auto response = std::make_shared<AcquireLease::Response>();
    lease_manager->acquireLease(request, response);
    // THEN the response should bear a valid sublease
    ASSERT_TRUE(response->success);
  }

  {
    // WHEN a second lease request is placed
    const std::string kClientNodeName = "other_dummy_node";
    auto request = std::make_shared<AcquireLease::Request>();
    request->resource_name = kBodyResource;
    request->client_name = kClientNodeName;
    auto response = std::make_shared<AcquireLease::Response>();
    lease_manager->acquireLease(request, response);
    // THEN the response should indicate failure
    ASSERT_FALSE(response->success);
  }
}

/**
 * Test a body sublease prevents an arm sublease
 */
TEST(TestLeaseManager, rootLeaseBlocksLeafLeasing) {
  // GIVEN a body lease is held by the lease manager
  auto lease_client = std::make_unique<spot_ros2::test::MockLeaseClient>();
  auto lease_wallet = std::make_shared<::bosdyn::client::LeaseWallet>("test_manager");
  EXPECT_CALL(*lease_client, getLeaseWallet()).WillRepeatedly(Return(lease_wallet));

  EXPECT_CALL(*lease_client, getResourceHierarchy()).WillRepeatedly([]() -> const ::bosdyn::client::ResourceHierarchy& {
    const auto requirement = ::bosdyn::client::LeaseHierarchyRequirements::ARM_AND_GRIPPER;
    return ::bosdyn::client::DefaultResourceHierarchy(requirement);
  });

  auto timer_interface = std::make_unique<spot_ros2::test::MockTimerInterface>();
  EXPECT_CALL(*timer_interface, setTimer(_, _)).Times(1);
  auto logger_interface = std::make_unique<spot_ros2::test::MockLoggerInterface>();
  EXPECT_CALL(*logger_interface, logDebug(_)).Times(AnyNumber());
  EXPECT_CALL(*logger_interface, logInfo(_)).Times(AnyNumber());
  auto middleware_handle = std::make_unique<MockMiddlewareHandle>();
  EXPECT_CALL(*middleware_handle, createClaimLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createAcquireLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReturnLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReleaseLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createBond(_, _)).Times(1);
  constexpr double kLeaseCheckRate = 1.0;  // Hz

  auto lease_manager =
      std::make_unique<LeaseManager>(std::move(lease_client), std::move(logger_interface), std::move(timer_interface),
                                     std::move(middleware_handle), kLeaseCheckRate);
  lease_manager->initialize();

  auto body_lease_proto = ::bosdyn::api::Lease();
  body_lease_proto.set_resource(kBodyResource);
  body_lease_proto.add_sequence(0);
  auto body_lease = ::bosdyn::client::Lease(body_lease_proto);
  lease_wallet->AddLease(body_lease);

  {
    // WHEN a body sublease request is placed
    const std::string kClientNodeName = "dummy_node";
    auto request = std::make_shared<AcquireLease::Request>();
    request->resource_name = kBodyResource;
    request->client_name = kClientNodeName;
    auto response = std::make_shared<AcquireLease::Response>();
    lease_manager->acquireLease(request, response);
    // THEN the response should bear a valid sublease
    ASSERT_TRUE(response->success);
  }

  {
    // WHEN an arm sublease request is placed
    const std::string kClientNodeName = "other_dummy_node";
    auto request = std::make_shared<AcquireLease::Request>();
    request->resource_name = kArmResource;
    request->client_name = kClientNodeName;
    auto response = std::make_shared<AcquireLease::Response>();
    lease_manager->acquireLease(request, response);
    // THEN the response should indicate failure
    ASSERT_FALSE(response->success);
  }
}

/**
 * Test an arm lease prevents a body lease
 */
TEST(TestLeaseManager, leafLeaseBlocksRootLeasing) {
  // GIVEN an arm lease is held by the lease manager
  auto lease_client = std::make_unique<spot_ros2::test::MockLeaseClient>();
  auto lease_wallet = std::make_shared<::bosdyn::client::LeaseWallet>("test_manager");
  EXPECT_CALL(*lease_client, getLeaseWallet()).WillRepeatedly(Return(lease_wallet));

  EXPECT_CALL(*lease_client, getResourceHierarchy()).WillRepeatedly([]() -> const ::bosdyn::client::ResourceHierarchy& {
    const auto requirement = ::bosdyn::client::LeaseHierarchyRequirements::ARM_AND_GRIPPER;
    return ::bosdyn::client::DefaultResourceHierarchy(requirement);
  });

  auto timer_interface = std::make_unique<spot_ros2::test::MockTimerInterface>();
  EXPECT_CALL(*timer_interface, setTimer(_, _)).Times(1);
  auto logger_interface = std::make_unique<spot_ros2::test::MockLoggerInterface>();
  EXPECT_CALL(*logger_interface, logDebug(_)).Times(AnyNumber());
  EXPECT_CALL(*logger_interface, logInfo(_)).Times(AnyNumber());
  auto middleware_handle = std::make_unique<MockMiddlewareHandle>();
  EXPECT_CALL(*middleware_handle, createClaimLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createAcquireLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReturnLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReleaseLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createBond(_, _)).Times(1);
  constexpr double kLeaseCheckRate = 1.0;  // Hz

  auto lease_manager =
      std::make_unique<LeaseManager>(std::move(lease_client), std::move(logger_interface), std::move(timer_interface),
                                     std::move(middleware_handle), kLeaseCheckRate);
  lease_manager->initialize();

  auto arm_lease_proto = ::bosdyn::api::Lease();
  arm_lease_proto.set_resource(kArmResource);
  arm_lease_proto.add_sequence(0);
  auto arm_lease = ::bosdyn::client::Lease(arm_lease_proto);
  lease_wallet->AddLease(arm_lease);

  {
    // WHEN an arm sublease request is placed
    const std::string kClientNodeName = "dummy_node";
    auto request = std::make_shared<AcquireLease::Request>();
    request->resource_name = kArmResource;
    request->client_name = kClientNodeName;
    auto response = std::make_shared<AcquireLease::Response>();
    lease_manager->acquireLease(request, response);
    // THEN the response should bear a valid sublease
    ASSERT_TRUE(response->success);
  }

  {
    // WHEN a body sublease request is placed
    const std::string kClientNodeName = "other_dummy_node";
    auto request = std::make_shared<AcquireLease::Request>();
    request->resource_name = kBodyResource;
    request->client_name = kClientNodeName;
    auto response = std::make_shared<AcquireLease::Response>();
    lease_manager->acquireLease(request, response);
    // THEN the response should indicate failure
    ASSERT_FALSE(response->success);
  }
}

/**
 * Test lease retention failure leads to sublease revocation
 */
TEST(TestLeaseManager, revocationOnLeaseRetentionFailure) {
  // GIVEN body sublease acquisition succeeds the first time it is called
  auto lease_client = std::make_unique<spot_ros2::test::MockLeaseClient>();
  auto lease_wallet = std::make_shared<::bosdyn::client::LeaseWallet>("test_manager");
  EXPECT_CALL(*lease_client, getLeaseWallet()).WillRepeatedly(Return(lease_wallet));

  EXPECT_CALL(*lease_client, getResourceHierarchy()).WillRepeatedly([]() -> const ::bosdyn::client::ResourceHierarchy& {
    const auto requirement = ::bosdyn::client::LeaseHierarchyRequirements::ARM_AND_GRIPPER;
    return ::bosdyn::client::DefaultResourceHierarchy(requirement);
  });

  auto lease_proto = ::bosdyn::api::Lease();
  lease_proto.set_resource(kBodyResource);
  lease_proto.add_sequence(0);
  auto lease = ::bosdyn::client::Lease(lease_proto);

  LeaseUseCallback retention_failure_callback;
  EXPECT_CALL(*lease_client, acquireLease(kBodyResource, _))
      .WillOnce([&](const std::string&, LeaseUseCallback callback) {
        lease_wallet->AddLease(lease);
        retention_failure_callback = std::move(callback);
        return lease;
      });

  auto timer_interface = std::make_unique<spot_ros2::test::MockTimerInterface>();
  EXPECT_CALL(*timer_interface, setTimer(_, _)).Times(1);
  auto logger_interface = std::make_unique<spot_ros2::test::MockLoggerInterface>();
  EXPECT_CALL(*logger_interface, logDebug(_)).Times(AnyNumber());
  EXPECT_CALL(*logger_interface, logInfo(_)).Times(AnyNumber());
  auto middleware_handle = std::make_unique<MockMiddlewareHandle>();
  EXPECT_CALL(*middleware_handle, createClaimLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createAcquireLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReturnLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReleaseLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createBond(_, _)).Times(1);
  constexpr double kLeaseCheckRate = 1.0;  // Hz

  auto lease_manager =
      std::make_unique<LeaseManager>(std::move(lease_client), std::move(logger_interface), std::move(timer_interface),
                                     std::move(middleware_handle), kLeaseCheckRate);
  lease_manager->initialize();

  {
    const std::string kClientNodeName = "dummy_node";
    auto request = std::make_shared<AcquireLease::Request>();
    request->resource_name = kBodyResource;
    request->client_name = kClientNodeName;
    auto response = std::make_shared<AcquireLease::Response>();
    lease_manager->acquireLease(request, response);
    ASSERT_TRUE(response->success);
  }
  ASSERT_TRUE(lease_wallet->GetLease(kBodyResource));

  // WHEN body lease retention fails
  auto lease_use_result = ::bosdyn::api::LeaseUseResult();
  *lease_use_result.mutable_attempted_lease() = lease_proto;
  retention_failure_callback(lease_use_result);

  // THEN both lease and sublease are revoked
  {
    auto request = std::make_shared<ReturnLease::Request>();
    bosdyn_api_msgs::conversions::Convert(lease.GetProto(), &request->lease);
    auto response = std::make_shared<ReturnLease::Response>();
    lease_manager->returnLease(request, response);
    ASSERT_FALSE(response->success) << response->message;
  }
  ASSERT_FALSE(lease_wallet->GetLease(kBodyResource));
}

/**
 * Test client bond breakage leads to sublease revocation
 */
TEST(TestLeaseManager, revocationOnBondBreakage) {
  // GIVEN body sublease acquisition succeeds the first time it is called
  auto lease_client = std::make_unique<spot_ros2::test::MockLeaseClient>();
  auto lease_wallet = std::make_shared<::bosdyn::client::LeaseWallet>("test_manager");
  EXPECT_CALL(*lease_client, getLeaseWallet()).WillRepeatedly(Return(lease_wallet));

  EXPECT_CALL(*lease_client, getResourceHierarchy()).WillRepeatedly([]() -> const ::bosdyn::client::ResourceHierarchy& {
    const auto requirement = ::bosdyn::client::LeaseHierarchyRequirements::ARM_AND_GRIPPER;
    return ::bosdyn::client::DefaultResourceHierarchy(requirement);
  });

  {
    auto initial_lease_proto = ::bosdyn::api::Lease();
    initial_lease_proto.set_resource(kBodyResource);
    initial_lease_proto.add_sequence(0);
    auto initial_lease = ::bosdyn::client::Lease(initial_lease_proto);
    lease_wallet->AddLease(initial_lease);
  }

  EXPECT_CALL(*lease_client, takeLease(kBodyResource, _)).WillOnce([&](const std::string&, LeaseUseCallback) {
    auto lease_proto = ::bosdyn::api::Lease();
    lease_proto.set_resource(kBodyResource);
    lease_proto.add_sequence(1);
    auto lease = ::bosdyn::client::Lease(lease_proto);
    lease_wallet->AddLease(lease);
    return lease;
  });

  auto timer_interface = std::make_unique<spot_ros2::test::MockTimerInterface>();
  EXPECT_CALL(*timer_interface, setTimer(_, _)).Times(1);
  auto logger_interface = std::make_unique<spot_ros2::test::MockLoggerInterface>();
  EXPECT_CALL(*logger_interface, logDebug(_)).Times(AnyNumber());
  EXPECT_CALL(*logger_interface, logInfo(_)).Times(AnyNumber());
  EXPECT_CALL(*logger_interface, logError(_)).Times(AnyNumber());
  auto middleware_handle = std::make_unique<MockMiddlewareHandle>();
  EXPECT_CALL(*middleware_handle, createClaimLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createAcquireLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReturnLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReleaseLeasesService(_, _)).Times(1);

  const std::string kClientNodeName = "dummy_node";
  std::function<void()> bond_break_callback;
  EXPECT_CALL(*middleware_handle, createBond(kClientNodeName, _))
      .WillOnce([&](const std::string&, std::function<void()> callback) {
        bond_break_callback = std::move(callback);
        return nullptr;
      });

  constexpr double kLeaseCheckRate = 1.0;  // Hz

  auto lease_manager =
      std::make_unique<LeaseManager>(std::move(lease_client), std::move(logger_interface), std::move(timer_interface),
                                     std::move(middleware_handle), kLeaseCheckRate);
  lease_manager->initialize();

  auto request = std::make_shared<AcquireLease::Request>();
  request->resource_name = kBodyResource;
  request->client_name = kClientNodeName;
  auto response = std::make_shared<AcquireLease::Response>();
  lease_manager->acquireLease(request, response);
  ASSERT_TRUE(response->success);
  auto sublease_proto = ::bosdyn::api::Lease();
  bosdyn_api_msgs::conversions::Convert(response->lease, &sublease_proto);
  const auto sublease = ::bosdyn::client::Lease(sublease_proto);
  ASSERT_TRUE(sublease.IsValid());

  // WHEN client bond breaks
  bond_break_callback();

  // THEN sublease is revoked
  auto lease = lease_wallet->GetLease(kBodyResource).move();
  using CompareResult = ::bosdyn::client::Lease::CompareResult;
  ASSERT_EQ(lease.Compare(sublease), CompareResult::NEWER);
}

/**
 * Test leasing status reports
 */
TEST(TestLeaseManager, leasingStatusReport) {
  // GIVEN lease listing succeeds the first time it is called
  auto lease_client = std::make_unique<spot_ros2::test::MockLeaseClient>();

  std::vector<::bosdyn::api::LeaseResource> leased_resources;
  auto& resource_proto = leased_resources.emplace_back();
  resource_proto.set_resource(kArmResource);
  auto& lease_proto = *resource_proto.mutable_lease();
  lease_proto.set_resource(kBodyResource);
  const std::string kLeaseEpoch = "unix";
  lease_proto.set_epoch(kLeaseEpoch);
  lease_proto.add_sequence(0);
  auto& lease_owner_proto = *resource_proto.mutable_lease_owner();
  const std::string kClientName = "dummy_client";
  lease_owner_proto.set_client_name(kClientName);
  const std::string kUserName = "dummy_user";
  lease_owner_proto.set_user_name(kUserName);

  EXPECT_CALL(*lease_client, listLeases).WillOnce(Return(leased_resources));
  auto timer_interface = std::make_unique<spot_ros2::test::MockTimerInterface>();
  auto logger_interface = std::make_unique<spot_ros2::test::MockLoggerInterface>();
  EXPECT_CALL(*logger_interface, logDebug(_)).Times(AnyNumber());
  EXPECT_CALL(*logger_interface, logInfo(_)).Times(AnyNumber());
  auto middleware_handle = std::make_unique<MockMiddlewareHandle>();
  EXPECT_CALL(*middleware_handle, createClaimLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createAcquireLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReturnLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReleaseLeasesService(_, _)).Times(1);
  std::unique_ptr<LeaseArray> lease_array_message;
  EXPECT_CALL(*middleware_handle, publishLeaseArray(_)).WillOnce([&](std::unique_ptr<LeaseArray> message) {
    lease_array_message = std::move(message);
  });

  auto* timer_interface_ptr = timer_interface.get();
  EXPECT_CALL(*timer_interface, setTimer(_, _))
      .WillOnce([&](const std::chrono::duration<double>&, const std::function<void()>& callback) {
        timer_interface_ptr->onSetTimer(callback);
      });
  constexpr double kLeaseCheckRate = 1.0;  // Hz
  auto lease_manager =
      std::make_unique<LeaseManager>(std::move(lease_client), std::move(logger_interface), std::move(timer_interface),
                                     std::move(middleware_handle), kLeaseCheckRate);
  lease_manager->initialize();

  // WHEN the timer callback is invoked
  timer_interface_ptr->trigger();

  // THEN leasing status is published
  ASSERT_NE(lease_array_message, nullptr);
  ASSERT_EQ(lease_array_message->resources.size(), 1);
  auto& resource_message = lease_array_message->resources[0];
  EXPECT_EQ(resource_message.resource, kArmResource);
  EXPECT_EQ(resource_message.lease.resource, kBodyResource);
  EXPECT_EQ(resource_message.lease.epoch, kLeaseEpoch);
  ASSERT_EQ(resource_message.lease.sequence.size(), 1);
  EXPECT_EQ(resource_message.lease.sequence[0], 0);
  EXPECT_EQ(resource_message.lease_owner.client_name, kClientName);
  EXPECT_EQ(resource_message.lease_owner.user_name, kUserName);
}

/**
 * Test leases can be proactively claimed
 */
TEST(TestLeaseManager, proactiveClaim) {
  // GIVEN lease acquisition succeeds the first time it is called on an empty wallet
  auto lease_client = std::make_unique<spot_ros2::test::MockLeaseClient>();
  auto lease_wallet = std::make_shared<::bosdyn::client::LeaseWallet>("test_manager");
  EXPECT_CALL(*lease_client, getLeaseWallet()).WillRepeatedly(Return(lease_wallet));

  EXPECT_CALL(*lease_client, getResourceHierarchy()).WillRepeatedly([]() -> const ::bosdyn::client::ResourceHierarchy& {
    const auto requirement = ::bosdyn::client::LeaseHierarchyRequirements::ARM_AND_GRIPPER;
    return ::bosdyn::client::DefaultResourceHierarchy(requirement);
  });

  auto lease_proto = ::bosdyn::api::Lease();
  lease_proto.set_resource(kBodyResource);
  lease_proto.add_sequence(0);
  auto lease = ::bosdyn::client::Lease(lease_proto);
  EXPECT_CALL(*lease_client, acquireLease(kBodyResource, _)).WillOnce([&](const std::string&, LeaseUseCallback) {
    lease_wallet->AddLease(lease);
    return lease;
  });

  auto timer_interface = std::make_unique<spot_ros2::test::MockTimerInterface>();
  EXPECT_CALL(*timer_interface, setTimer(_, _)).Times(1);
  auto logger_interface = std::make_unique<spot_ros2::test::MockLoggerInterface>();
  EXPECT_CALL(*logger_interface, logDebug(_)).Times(AnyNumber());
  EXPECT_CALL(*logger_interface, logInfo(_)).Times(AnyNumber());
  auto middleware_handle = std::make_unique<MockMiddlewareHandle>();
  EXPECT_CALL(*middleware_handle, createClaimLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createAcquireLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReturnLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReleaseLeasesService(_, _)).Times(1);

  constexpr double kLeaseCheckRate = 1.0;  // Hz

  auto lease_manager =
      std::make_unique<LeaseManager>(std::move(lease_client), std::move(logger_interface), std::move(timer_interface),
                                     std::move(middleware_handle), kLeaseCheckRate);
  lease_manager->initialize();

  // WHEN leases are claimed
  auto request = std::make_shared<Trigger::Request>();
  auto response = std::make_shared<Trigger::Response>();
  lease_manager->claimLeases(request, response);

  // THEN claim succeeds.
  EXPECT_TRUE(response->success);
  EXPECT_TRUE(lease_wallet->GetLease(kBodyResource));
}

/**
 * Test proactive lease claims are idempotent
 */
TEST(TestLeaseManager, twiceProactiveClaim) {
  // GIVEN lease wallet contains leases
  auto lease_client = std::make_unique<spot_ros2::test::MockLeaseClient>();
  auto lease_wallet = std::make_shared<::bosdyn::client::LeaseWallet>("test_manager");

  {
    auto lease_proto = ::bosdyn::api::Lease();
    lease_proto.set_resource(kArmResource);
    lease_proto.add_sequence(0);
    auto lease = ::bosdyn::client::Lease(lease_proto);
    lease_wallet->AddLease(lease);
  }

  EXPECT_CALL(*lease_client, getLeaseWallet()).WillRepeatedly(Return(lease_wallet));

  EXPECT_CALL(*lease_client, getResourceHierarchy()).WillRepeatedly([]() -> const ::bosdyn::client::ResourceHierarchy& {
    const auto requirement = ::bosdyn::client::LeaseHierarchyRequirements::ARM_AND_GRIPPER;
    return ::bosdyn::client::DefaultResourceHierarchy(requirement);
  });

  auto timer_interface = std::make_unique<spot_ros2::test::MockTimerInterface>();
  EXPECT_CALL(*timer_interface, setTimer(_, _)).Times(1);
  auto logger_interface = std::make_unique<spot_ros2::test::MockLoggerInterface>();
  EXPECT_CALL(*logger_interface, logDebug(_)).Times(AnyNumber());
  EXPECT_CALL(*logger_interface, logInfo(_)).Times(AnyNumber());
  auto middleware_handle = std::make_unique<MockMiddlewareHandle>();
  EXPECT_CALL(*middleware_handle, createClaimLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createAcquireLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReturnLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReleaseLeasesService(_, _)).Times(1);
  constexpr double kLeaseCheckRate = 1.0;  // Hz

  auto lease_manager =
      std::make_unique<LeaseManager>(std::move(lease_client), std::move(logger_interface), std::move(timer_interface),
                                     std::move(middleware_handle), kLeaseCheckRate);
  lease_manager->initialize();

  // WHEN leases are claimed
  auto request = std::make_shared<Trigger::Request>();
  auto response = std::make_shared<Trigger::Response>();
  lease_manager->claimLeases(request, response);

  // THEN claim succeeds.
  EXPECT_TRUE(response->success);
}

/**
 * Test leases can be forcibly released
 */
TEST(TestLeaseManager, forcedRelease) {
  // GIVEN lease wallet contains leases
  auto lease_client = std::make_unique<spot_ros2::test::MockLeaseClient>();
  auto lease_wallet = std::make_shared<::bosdyn::client::LeaseWallet>("test_manager");

  {
    auto lease_proto = ::bosdyn::api::Lease();
    lease_proto.set_resource(kBodyResource);
    lease_proto.add_sequence(0);
    auto lease = ::bosdyn::client::Lease(lease_proto);
    lease_wallet->AddLease(lease);
  }

  EXPECT_CALL(*lease_client, getLeaseWallet()).WillRepeatedly(Return(lease_wallet));

  EXPECT_CALL(*lease_client, returnLease(_)).WillOnce([&](const ::bosdyn::client::Lease& lease) {
    lease_wallet->RemoveLease(lease.GetResource());
    return true;
  });

  EXPECT_CALL(*lease_client, getResourceHierarchy()).WillRepeatedly([]() -> const ::bosdyn::client::ResourceHierarchy& {
    const auto requirement = ::bosdyn::client::LeaseHierarchyRequirements::ARM_AND_GRIPPER;
    return ::bosdyn::client::DefaultResourceHierarchy(requirement);
  });

  auto timer_interface = std::make_unique<spot_ros2::test::MockTimerInterface>();
  EXPECT_CALL(*timer_interface, setTimer(_, _)).Times(1);
  auto logger_interface = std::make_unique<spot_ros2::test::MockLoggerInterface>();
  EXPECT_CALL(*logger_interface, logDebug(_)).Times(AnyNumber());
  EXPECT_CALL(*logger_interface, logInfo(_)).Times(AnyNumber());
  auto middleware_handle = std::make_unique<MockMiddlewareHandle>();
  EXPECT_CALL(*middleware_handle, createClaimLeasesService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createAcquireLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReturnLeaseService(_, _)).Times(1);
  EXPECT_CALL(*middleware_handle, createReleaseLeasesService(_, _)).Times(1);
  constexpr double kLeaseCheckRate = 1.0;  // Hz

  auto lease_manager =
      std::make_unique<LeaseManager>(std::move(lease_client), std::move(logger_interface), std::move(timer_interface),
                                     std::move(middleware_handle), kLeaseCheckRate);
  lease_manager->initialize();

  // WHEN leases are released
  auto request = std::make_shared<Trigger::Request>();
  auto response = std::make_shared<Trigger::Response>();
  lease_manager->releaseLeases(request, response);

  // THEN release succeeds.
  EXPECT_TRUE(response->success);

  EXPECT_THAT(lease_wallet->GetAllLeases(), IsEmpty());
}

}  // namespace spot_ros2::lease::test
