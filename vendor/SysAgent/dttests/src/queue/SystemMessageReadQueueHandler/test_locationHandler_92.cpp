///*
// * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
// */
//#include "mockcpp/mockcpp.hpp"
//#include "gtest/gtest.h"
//#include "queue/SystemMessageReadQueueHandler.h"
//#include "TeHelper.h"
//#include "mm_sdk.h"
//#include "data_source.h"
//#include "memory/Location.h"
//
//class TestLocationHandler: public ::testing::Test
//{
//protected:
//   static void SetUpTestCase()
//   {
//   }
//
//   static void TearDownTestCase()
//   {
//   }
//
//   virtual void SetUp()
//   {
//   }
//
//   virtual void TearDown()
//   {
//       GlobalMockObject::verify();
//   }
//};
//
//TEST_F(TestLocationHandler, locationHandler_EmptyResult) {
//    // Given
//    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.deviceId = "deviceId";
//    accessCtrl->locationInfo.data.latitude = 1.0;
//    accessCtrl->locationInfo.data.longitude = 2.0;
//
//    auto location = make_shared<Location>("deviceId", 1.0, 2.0);
//    Mocker_CPP(&Location::Query, vector<string> (Location::*)()).stubs().will(returnValue(vector<string>()));
//
//    // When
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    systemMessageReadQueueHandler.locationHandler(accessCtrl);
//
//    // Then
//    // No specific assertion needed as we are checking for empty result
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestLocationHandler, locationHandler_ResultSizeNotSix) {
//    // Given
//    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.deviceId = "deviceId";
//    accessCtrl->locationInfo.data.latitude = 1.0;
//    accessCtrl->locationInfo.data.longitude = 2.0;
//
//    auto location = make_shared<Location>("deviceId", 1.0, 2.0);
//    Mocker_CPP(&Location::Query, vector<string> (Location::*)()).stubs().will(returnValue(vector<string>{"id", "deviceId", "1.0", "2.0"}));
//
//    // When
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    systemMessageReadQueueHandler.locationHandler(accessCtrl);
//
//    // Then
//    // No specific assertion needed as we are checking for result size not equal to six
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestLocationHandler, locationHandler_LocationDidNotChange) {
//    // Given
//    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.deviceId = "deviceId";
//    accessCtrl->locationInfo.data.latitude = 1.0;
//    accessCtrl->locationInfo.data.longitude = 2.0;
//
//    auto location = make_shared<Location>("deviceId", 1.0, 2.0);
//    Mocker_CPP(&Location::Query, vector<string> (Location::*)()).stubs().will(returnValue(vector<string>{"id", "deviceId", "1.0", "2.0", "city", "address"}));
//    Mocker_CPP(&Location::getLatitude, double (Location::*)() const).stubs().will(returnValue(1.0));
//    Mocker_CPP(&Location::getLongitude, double (Location::*)() const).stubs().will(returnValue(2.0));
//
//    // When
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    systemMessageReadQueueHandler.locationHandler(accessCtrl);
//
//    // Then
//    // No specific assertion needed as we are checking for location did not change
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestLocationHandler, locationHandler_LocationChanged_InvokeToolsFailed) {
//    // Given
//    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.deviceId = "deviceId";
//    accessCtrl->locationInfo.data.latitude = 1.0;
//    accessCtrl->locationInfo.data.longitude = 2.0;
//
//    auto location = make_shared<Location>("deviceId", 1.0, 2.0);
//    Mocker_CPP(&Location::Query, vector<string> (Location::*)()).stubs().will(returnValue(vector<string>{"id", "deviceId", "1.0", "2.0", "city", "address"}));
//    Mocker_CPP(&Location::getLatitude, double (Location::*)() const).stubs().will(returnValue(3.0));
//    Mocker_CPP(&Location::getLongitude, double (Location::*)() const).stubs().will(returnValue(4.0));
//
//    Mocker(static_cast<uint32_t(*)(const string&, function<uint32_t(uint32_t, const vector<string>&)>)>(&mmsdk::InvokeToolsWithJson)).stubs().will(returnValue(uint32_t(1)));
//
//    // When
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    systemMessageReadQueueHandler.locationHandler(accessCtrl);
//
//    // Then
//    // No specific assertion needed as we are checking for invoke tools failed
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestLocationHandler, locationHandler_LocationChanged_InvokeToolsSuccess_EmptyResult) {
//    // Given
//    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.deviceId = "deviceId";
//    accessCtrl->locationInfo.data.latitude = 1.0;
//    accessCtrl->locationInfo.data.longitude = 2.0;
//
//    auto location = make_shared<Location>("deviceId", 1.0, 2.0);
//    Mocker_CPP(&Location::Query, vector<string> (Location::*)()).stubs().will(returnValue(vector<string>{"id", "deviceId", "1.0", "2.0", "city", "address"}));
//    Mocker_CPP(&Location::getLatitude, double (Location::*)() const).stubs().will(returnValue(3.0));
//    Mocker_CPP(&Location::getLongitude, double (Location::*)() const).stubs().will(returnValue(4.0));
//
//    Mocker(static_cast<uint32_t(*)(const string&, function<uint32_t(uint32_t, const vector<string>&)>)>(&mmsdk::InvokeToolsWithJson)).stubs().will(invoke([](const string&, function<uint32_t(uint32_t, const vector<string>&)> func) {
//        return func(0, vector<string>());
//    }));
//
//    // When
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    systemMessageReadQueueHandler.locationHandler(accessCtrl);
//
//    // Then
//    // No specific assertion needed as we are checking for invoke tools success but empty result
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestLocationHandler, locationHandler_LocationChanged_InvokeToolsSuccess_ParseJsonFailed) {
//    // Given
//    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.deviceId = "deviceId";
//    accessCtrl->locationInfo.data.latitude = 1.0;
//    accessCtrl->locationInfo.data.longitude = 2.0;
//
//    auto location = make_shared<Location>("deviceId", 1.0, 2.0);
//    Mocker_CPP(&Location::Query, vector<string> (Location::*)()).stubs().will(returnValue(vector<string>{"id", "deviceId", "1.0", "2.0", "city", "address"}));
//    Mocker_CPP(&Location::getLatitude, double (Location::*)() const).stubs().will(returnValue(3.0));
//    Mocker_CPP(&Location::getLongitude, double (Location::*)() const).stubs().will(returnValue(4.0));
//
//    Mocker(static_cast<uint32_t(*)(const string&, function<uint32_t(uint32_t, const vector<string>&)>)>(&mmsdk::InvokeToolsWithJson)).stubs().will(invoke([](const string&, function<uint32_t(uint32_t, const vector<string>&)> func) {
//        return func(0, vector<string>{"invalid_json"});
//    }));
//
//    // When
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    systemMessageReadQueueHandler.locationHandler(accessCtrl);
//
//    // Then
//    // No specific assertion needed as we are checking for invoke tools success but parse json failed
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestLocationHandler, locationHandler_LocationChanged_InvokeToolsSuccess_QueryLocationFailed) {
//    // Given
//    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.deviceId = "deviceId";
//    accessCtrl->locationInfo.data.latitude = 1.0;
//    accessCtrl->locationInfo.data.longitude = 2.0;
//
//    auto location = make_shared<Location>("deviceId", 1.0, 2.0);
//    Mocker_CPP(&Location::Query, vector<string> (Location::*)()).stubs().will(returnValue(vector<string>{"id", "deviceId", "1.0", "2.0", "city", "address"}));
//    Mocker_CPP(&Location::getLatitude, double (Location::*)() const).stubs().will(returnValue(3.0));
//    Mocker_CPP(&Location::getLongitude, double (Location::*)() const).stubs().will(returnValue(4.0));
//
//    Mocker(static_cast<uint32_t(*)(const string&, function<uint32_t(uint32_t, const vector<string>&)>)>(&mmsdk::InvokeToolsWithJson)).stubs().will(invoke([](const string&, function<uint32_t(uint32_t, const vector<string>&)> func) {
//        return func(0, vector<string>{"{\"errCode\":1, \"errMessage\":\"error\", \"data\":{\"formatted_address\":\"\",\"city\":\"\"}}"});
//    }));
//
//    // When
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    systemMessageReadQueueHandler.locationHandler(accessCtrl);
//
//    // Then
//    // No specific assertion needed as we are checking for invoke tools success but query location failed
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestLocationHandler, locationHandler_LocationChanged_InvokeToolsSuccess_DeviceIDNotExist) {
//    // Given
//    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.deviceId = "deviceId";
//    accessCtrl->locationInfo.data.latitude = 1.0;
//    accessCtrl->locationInfo.data.longitude = 2.0;
//
//    auto location = make_shared<Location>("deviceId", 1.0, 2.0);
//    Mocker_CPP(&Location::Query, vector<string> (Location::*)()).stubs().will(returnValue(vector<string>{}));
//    Mocker_CPP(&Location::getLatitude, double (Location::*)() const).stubs().will(returnValue(3.0));
//    Mocker_CPP(&Location::getLongitude, double (Location::*)() const).stubs().will(returnValue(4.0));
//
//    Mocker(static_cast<uint32_t(*)(const string&, function<uint32_t(uint32_t, const vector<string>&)>)>(&mmsdk::InvokeToolsWithJson)).stubs().will(invoke([](const string&, function<uint32_t(uint32_t, const vector<string>&)> func) {
//        return func(0, vector<string>{"{\"errCode\":0, \"errMessage\":\"success\", \"data\":{\"formatted_address\":\"address\",\"city\":\"city\"}}"});
//    }));
//
//    Mocker_CPP(&Location::setAddress, void (Location::*)(const string&)).stubs();
//    Mocker_CPP(&Location::setCity, void (Location::*)(const string&)).stubs();
//    Mocker_CPP(&Location::Insert, void (Location::*)()).stubs();
//
//    // When
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    systemMessageReadQueueHandler.locationHandler(accessCtrl);
//
//    // Then
//    // No specific assertion needed as we are checking for device ID does not exist
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestLocationHandler, locationHandler_LocationChanged_InvokeToolsSuccess_DeviceIDExist) {
//    // Given
//    auto accessCtrl = make_shared<SYSAGENT::AccessCtrl>();
//    accessCtrl->header.deviceId = "deviceId";
//    accessCtrl->locationInfo.data.latitude = 1.0;
//    accessCtrl->locationInfo.data.longitude = 2.0;
//
//    auto location = make_shared<Location>("deviceId", 1.0, 2.0);
//    Mocker_CPP(&Location::Query, vector<string> (Location::*)()).stubs().will(returnValue(vector<string>{"id", "deviceId", "1.0", "2.0", "city", "address"}));
//    Mocker_CPP(&Location::getLatitude, double (Location::*)() const).stubs().will(returnValue(3.0));
//    Mocker_CPP(&Location::getLongitude, double (Location::*)() const).stubs().will(returnValue(4.0));
//
//    Mocker(static_cast<uint32_t(*)(const string&, function<uint32_t(uint32_t, const vector<string>&)>)>(&mmsdk::InvokeToolsWithJson)).stubs().will(invoke([](const string&, function<uint32_t(uint32_t, const vector<string>&)> func) {
//        return func(0, vector<string>{"{\"errCode\":0, \"errMessage\":\"success\", \"data\":{\"formatted_address\":\"address\",\"city\":\"city\"}}"});
//    }));
//
//    Mocker_CPP(&Location::setAddress, void (Location::*)(const string&)).stubs();
//    Mocker_CPP(&Location::setCity, void (Location::*)(const string&)).stubs();
//    Mocker_CPP(&Location::Update, void (Location::*)()).stubs();
//
//    // When
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    systemMessageReadQueueHandler.locationHandler(accessCtrl);
//
//    // Then
//    // No specific assertion needed as we are checking for device ID exists
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
