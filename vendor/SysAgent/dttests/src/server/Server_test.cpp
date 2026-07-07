#include <gtest/gtest.h>
#include "mockcpp/mokc.h"
#include "libwebsockets/libwebsockets.h"
#include "message/ServerEntity.h"
#include "mockcpp/mockcpp.hpp"

#include <thread>
#include "server/Server.h"
#include "event2/thread.h"
#include "message/ServerEntity.h"
#include "queue/QueueManager.h"
#include "aisf_platform/common_define.h"

using namespace SYSAGENT;

struct lws {
    int dummy;
};



TEST(CallBackCtrTest, WsServerCallback_Established_SessionNull) {
    // Given
    struct lws *wsi = nullptr;
    enum lws_callback_reasons reason = LWS_CALLBACK_ESTABLISHED;
    void *user = nullptr;
    void *in = nullptr;
    size_t len = 0;

    // When
    int ret = CallBackCtr::WsServerCallback(wsi, reason, user, in, len);

    // Then
    EXPECT_EQ(ret, 0);

    // Clean
    GlobalMockObject::verify();
}

TEST(CallBackCtrTest, WsServerCallback_Established_SessionNotNull) {
    // Given
    struct lws *wsi = (struct lws *)malloc(sizeof(struct lws));
    ASSERT_NE(wsi, nullptr);
    (void)memset_s(wsi, sizeof(struct lws), 0, sizeof(struct lws));
    enum lws_callback_reasons reason = LWS_CALLBACK_ESTABLISHED;
    PerSession *session = (PerSession *)malloc(sizeof(PerSession));
    ASSERT_NE(session, nullptr);
    (void)memset_s(session, sizeof(PerSession), 0, sizeof(PerSession));
    void *user = (void *)session;
    void *in = nullptr;
    size_t len = 0;

    // When
    int ret = CallBackCtr::WsServerCallback(wsi, reason, user, in, len);

    // Then
    EXPECT_EQ(ret, 0);
    EXPECT_STREQ(session->session_id, "session_1");
    EXPECT_EQ(session->wsi, wsi);

    // Clean
    GlobalMockObject::verify();
    free(wsi);
    free(session);
}

TEST(CallBackCtrTest, WsServerCallback_Receive_SessionNull) {
    // Given
    struct lws *wsi = nullptr;
    enum lws_callback_reasons reason = LWS_CALLBACK_RECEIVE;
    void *user = nullptr;
    void *in = nullptr;
    size_t len = 0;

    // When
    int ret = CallBackCtr::WsServerCallback(wsi, reason, user, in, len);

    // Then
    EXPECT_EQ(ret, 0);

    // Clean
    GlobalMockObject::verify();
}

TEST(CallBackCtrTest, WsServerCallback_Receive_IsBinary) {
    // Given
    struct lws *wsi = (struct lws *)malloc(sizeof(struct lws));
    ASSERT_NE(wsi, nullptr);
    (void)memset_s(wsi, sizeof(struct lws), 0, sizeof(struct lws));
    enum lws_callback_reasons reason = LWS_CALLBACK_RECEIVE;
    PerSession *session = (PerSession *)malloc(sizeof(PerSession));
    ASSERT_NE(session, nullptr);
    (void)memset_s(session, sizeof(PerSession), 0, sizeof(PerSession));
    void *user = (void *)session;
    void *in = nullptr;
    size_t len = 0;
    MOCKER(lws_frame_is_binary).stubs().will(returnValue(1));

    // When
    int ret = CallBackCtr::WsServerCallback(wsi, reason, user, in, len);

    // Then
    EXPECT_EQ(ret, 0);

    // Clean
    GlobalMockObject::verify();
    free(wsi);
    free(session);
}

TEST(CallBackCtrTest, WsServerCallback_Receive_ParseError) {
    // Given
    struct lws *wsi = (struct lws *)malloc(sizeof(struct lws));
    ASSERT_NE(wsi, nullptr);
    (void)memset_s(wsi, sizeof(struct lws), 0, sizeof(struct lws));
    enum lws_callback_reasons reason = LWS_CALLBACK_RECEIVE;
    PerSession *session = (PerSession *)malloc(sizeof(PerSession));
    ASSERT_NE(session, nullptr);
    (void)memset_s(session, sizeof(PerSession), 0, sizeof(PerSession));
    void *user = (void *)session;
    char *in = (char *)"invalid";
    size_t len = strlen(in);
    MOCKER(lws_frame_is_binary).stubs().will(returnValue(0));
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    MOCKER_CPP(&SYSAGENT::AccessCtrl::Parse, std::string (SYSAGENT::AccessCtrl::*)(const char*, size_t)).stubs().will(returnValue(std::string("parse error")));

    // When
    int ret = CallBackCtr::WsServerCallback(wsi, reason, user, in, len);

    // Then
    EXPECT_EQ(ret, 0);

    // Clean
    GlobalMockObject::verify();
    free(wsi);
    free(session);
}

TEST(CallBackCtrTest, WsServerCallback_Receive_Success) {
    // Given
    struct lws *wsi = (struct lws *)malloc(sizeof(struct lws));
    ASSERT_NE(wsi, nullptr);
    (void)memset_s(wsi, sizeof(struct lws), 0, sizeof(struct lws));
    enum lws_callback_reasons reason = LWS_CALLBACK_RECEIVE;
    PerSession *session = (PerSession *)malloc(sizeof(PerSession));
    ASSERT_NE(session, nullptr);
    (void)memset_s(session, sizeof(PerSession), 0, sizeof(PerSession));
    void *user = (void *)session;
    char *in = (char *)"valid";
    size_t len = strlen(in);
    MOCKER(lws_frame_is_binary).stubs().will(returnValue(0));
    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
    MOCKER_CPP(&SYSAGENT::AccessCtrl::Parse, std::string (SYSAGENT::AccessCtrl::*)(const char*, size_t)).stubs().will(returnValue(std::string("")));

    // When
    int ret = CallBackCtr::WsServerCallback(wsi, reason, user, in, len);

    // Then
    EXPECT_EQ(ret, 0);

    // Clean
    GlobalMockObject::verify();
    free(wsi);
    free(session);
}

TEST(CallBackCtrTest, WsServerCallback_Closed) {
    // Given
    struct lws *wsi = nullptr;
    enum lws_callback_reasons reason = LWS_CALLBACK_CLOSED;
    void *user = nullptr;
    void *in = nullptr;
    size_t len = 0;

    // When
    int ret = CallBackCtr::WsServerCallback(wsi, reason, user, in, len);

    // Then
    EXPECT_EQ(ret, 0);

    // Clean
    GlobalMockObject::verify();
}

TEST(CallBackCtrTest, WsServerCallback_Default) {
    // Given
    struct lws *wsi = nullptr;
    enum lws_callback_reasons reason = (enum lws_callback_reasons)999;
    void *user = nullptr;
    void *in = nullptr;
    size_t len = 0;

    // When
    int ret = CallBackCtr::WsServerCallback(wsi, reason, user, in, len);

    // Then
    EXPECT_EQ(ret, 0);

    // Clean
    GlobalMockObject::verify();
}