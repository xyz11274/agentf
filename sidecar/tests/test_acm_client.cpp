#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "acm_client.h"

using namespace sidecar;

// ═══════════════════════════════════════════════════════════
// URL 构建 (I-acm-url)
// ═══════════════════════════════════════════════════════════

TEST(TestAcmUrl, TC_ACM_001_DownloadUrlConcat) {
    EXPECT_EQ(acm_build_download_url("http://127.0.0.1:23768", "look.png"),
              "http://127.0.0.1:23768/acm/v1/downloadfile/look.png");
}

TEST(TestAcmUrl, TC_ACM_002_UploadUrlConcat) {
    EXPECT_EQ(acm_build_upload_url("http://127.0.0.1:23768"),
              "http://127.0.0.1:23768/acm/v1/uploadfile");
}

// ═══════════════════════════════════════════════════════════
// 失败信号识别 (I-error-signal)
// ═══════════════════════════════════════════════════════════

TEST(TestAcmFailure, TC_ACM_003_RetCode500Detected) {
    const std::string body = R"({"Version":"1.0","RetCode":500,"RetMsg":"error"})";
    EXPECT_TRUE(acm_is_failure_body(body));
    EXPECT_EQ(acm_failure_error(body), "RetCode:500");
}

TEST(TestAcmFailure, TC_ACM_004_FileNotExistDetected) {
    const std::string body = R"({"Version":"1.0","RetCode":500,"RetMsg":"request file not exist"})";
    EXPECT_TRUE(acm_is_failure_body(body));
    EXPECT_EQ(acm_failure_error(body), "request file not exist");
}

TEST(TestAcmFailure, TC_ACM_005_SuccessNotFailure) {
    const std::string body = R"({"Version":"1.0","RetCode":200,"RetMsg":"OK","Data":{"fileUUID":"a.jpg"}})";
    EXPECT_FALSE(acm_is_failure_body(body));
}

// ═══════════════════════════════════════════════════════════
// 扩展名白名单 (I-upload-format)
// ═══════════════════════════════════════════════════════════

TEST(TestAcmFileType, TC_ACM_006_AllowedJpg) {
    EXPECT_TRUE(acm_is_allowed_file_type("look.jpg"));
    EXPECT_TRUE(acm_is_allowed_file_type("look.JPG"));  // 大小写不敏感
}

TEST(TestAcmFileType, TC_ACM_007_AllowedList) {
    for (const auto& ext : {"jpg", "log", "zip", "txt", "html", "wav"}) {
        EXPECT_TRUE(acm_is_allowed_file_type(std::string("f.") + ext));
    }
}

TEST(TestAcmFileType, TC_ACM_008_DisallowedWebp) {
    EXPECT_FALSE(acm_is_allowed_file_type("bad.webp"));
    EXPECT_FALSE(acm_is_allowed_file_type("noext"));
}

// ═══════════════════════════════════════════════════════════
// multipart 构造 (I-upload-format)
// ═══════════════════════════════════════════════════════════

TEST(TestAcmMultipart, TC_ACM_009_MultipartBodyFormat) {
    const std::vector<uint8_t> content = {'A', 'B', 'C'};
    const std::string body = acm_build_multipart_body("f.jpg", content);

    // 对齐服务端 DoHandleUploadFile 解析：filename="..." + Content-Length + \r\n\r\n + payload
    EXPECT_EQ(body.find("filename=\"f.jpg\""), 0u);
    EXPECT_NE(body.find("Content-Length: 3"), std::string::npos);
    const auto marker = body.find("\r\n\r\n");
    ASSERT_NE(marker, std::string::npos);
    EXPECT_EQ(body.substr(marker + 4), "ABC");
}

TEST(TestAcmMultipart, TC_ACM_010_MultipartBinaryPayload) {
    // 二进制 payload（含 \0）经 multipart 构造后必须原样保留（I-binary-safe）
    const std::vector<uint8_t> content = {0xFF, 0xD8, 0x00, 0x01, 0xFE};
    const std::string body = acm_build_multipart_body("bin.jpg", content);
    const auto marker = body.find("\r\n\r\n");
    ASSERT_NE(marker, std::string::npos);
    const std::string payload = body.substr(marker + 4);
    ASSERT_EQ(payload.size(), content.size());
    for (size_t i = 0; i < content.size(); ++i) {
        EXPECT_EQ(static_cast<uint8_t>(payload[i]), content[i]);
    }
}

// ═══════════════════════════════════════════════════════════
// base64 解码 (I-binary-safe)
// ═══════════════════════════════════════════════════════════

TEST(TestAcmBase64, TC_ACM_011_DecodeBasic) {
    // "Hello" → SGVsbG8=
    const auto out = acm_base64_decode("SGVsbG8=");
    const std::string s(out.begin(), out.end());
    EXPECT_EQ(s, "Hello");
}

TEST(TestAcmBase64, TC_ACM_012_DecodeBinary) {
    // 0xFF 0xD8 0xFF 0xE0 → /9j/4A==
    const auto out = acm_base64_decode("/9j/4A==");
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], 0xFF);
    EXPECT_EQ(out[1], 0xD8);
    EXPECT_EQ(out[2], 0xFF);
    EXPECT_EQ(out[3], 0xE0);
}

TEST(TestAcmBase64, TC_ACM_013_DecodeWithPaddingAndWhitespace) {
    // 含换行/空白的 base64 也应解码（服务端 JSON 中可能格式化）
    const auto out = acm_base64_decode("SGVs\nbG8=");
    const std::string s(out.begin(), out.end());
    EXPECT_EQ(s, "Hello");
}

// ═══════════════════════════════════════════════════════════
// 下载响应解析 (I-error-signal / I-base64-trigger)
// ═══════════════════════════════════════════════════════════

TEST(TestAcmDownloadParse, TC_ACM_014_BinaryModeBodyAsContent) {
    // 二进制模式：响应体即文件 bytes
    const std::string body = "\xFF\xD8\xFF\xE0";
    const auto r = acm_parse_download_body(body);
    EXPECT_TRUE(r.success);
    ASSERT_EQ(r.content.size(), 4u);
    EXPECT_EQ(r.content[0], 0xFF);
    EXPECT_EQ(r.content[1], 0xD8);
}

TEST(TestAcmDownloadParse, TC_ACM_015_FailureBodyNotContent) {
    // 失败信号（RetCode:500）不得当成功内容返回
    const std::string body = R"({"Version":"1.0","RetCode":500,"RetMsg":"request file not exist"})";
    const auto r = acm_parse_download_body(body);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error, "request file not exist");
}
