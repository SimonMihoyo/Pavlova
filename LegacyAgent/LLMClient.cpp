//
//  LLMClient.cpp
//  LegacyAgent
//
//  Created by SimonMihoyo on 2026/8/22.
//

#include "headers/LLMClient.h"
#include "headers/SessionStore.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CFNetwork/CFNetwork.h>
#include <thread>
#include <map>
#include <chrono>
#include <random>

// ---- JSON 编解码辅助 ----

namespace {

std::string getStringField(const JSONObject& obj, const std::string& key) {
    auto it = obj.find(key);
    if (it != obj.end() && it->second.asString()) return *it->second.asString();
    return "";
}

bool isRetryableStatus(int code) {
    return code == 429 || code == 500 || code == 502 || code == 503 || code == 504;
}

int computeRetryDelay(int attempt, const std::string& retryAfter) {
    // 优先使用 Retry-After 头（秒数）
    if (!retryAfter.empty()) {
        try {
            int sec = std::stoi(retryAfter);
            if (sec > 0 && sec <= 120) return sec * 1000;
        } catch (...) {}
    }
    // 指数退避：1s, 2s, 4s + 随机抖动
    static std::mt19937 rng(std::random_device{}());
    int base = 1000 * (1 << attempt); // 1000, 2000, 4000
    std::uniform_int_distribution<int> dist(0, base / 2);
    return base + dist(rng);
}

} // namespace

LLMClient::LLMClient(const AgentConfig& config) : config_(config) {
    std::string base = config.baseURL;
    if (!base.empty() && base.back() == '/') base.pop_back();
    endpoint_ = base + "/chat/completions";
}

std::string LLMClient::buildRequestBody(const std::vector<ChatMessage>& messages,
                                        const std::vector<ToolDefinition>& tools,
                                        const std::string& toolChoice,
                                        bool stream) const {
    JSONObject body;
    body["model"] = JSONValue(config_.model);

    JSONArray msgArr;
    for (const auto& m : messages) {
        msgArr.push_back(chatMessageToJSON(m));
    }
    body["messages"] = JSONValue(msgArr);

    if (!tools.empty()) {
        JSONArray toolArr;
        for (const auto& t : tools) {
            JSONObject def;
            def["type"] = JSONValue(t.type);
            JSONObject func;
            func["name"] = JSONValue(t.function.name);
            func["description"] = JSONValue(t.function.description);
            if (t.function.parameters) {
                func["parameters"] = *t.function.parameters;
            }
            def["function"] = JSONValue(func);
            toolArr.push_back(JSONValue(def));
        }
        body["tools"] = JSONValue(toolArr);
        if (!toolChoice.empty()) {
            body["tool_choice"] = JSONValue(toolChoice);
        }
    }

    if (stream) {
        body["stream"] = JSONValue(true);
    }

    return JSON::serialize(JSONValue(body));
}

std::string LLMClient::post(const std::string& body,
                            const std::vector<std::pair<std::string, std::string>>& headers,
                            std::string& responseBody,
                            std::string& error) const {
    return httpRequestStream(body, headers, nullptr,
        [&](const char* d, size_t n) { responseBody.append(d, n); return true; },
        error);
}

std::string LLMClient::httpRequestStream(
    const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::function<void(int statusCode, const std::string& contentType)>& onHeaders,
    const std::function<bool(const char* data, size_t len)>& onChunk,
    std::string& error,
    std::string* retryAfter) const {

    CFURLRef url = CFURLCreateWithString(kCFAllocatorDefault,
                                         CFStringCreateWithCString(kCFAllocatorDefault, endpoint_.c_str(), kCFStringEncodingUTF8),
                                         nullptr);
    if (!url) {
        error = "无效的 baseURL：" + config_.baseURL;
        return "";
    }

    CFHTTPMessageRef request = CFHTTPMessageCreateRequest(kCFAllocatorDefault,
                                                          CFSTR("POST"),
                                                          url,
                                                          kCFHTTPVersion1_1);
    CFRelease(url);

    for (const auto& h : headers) {
        CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, h.first.c_str(), kCFStringEncodingUTF8);
        CFStringRef value = CFStringCreateWithCString(kCFAllocatorDefault, h.second.c_str(), kCFStringEncodingUTF8);
        CFHTTPMessageSetHeaderFieldValue(request, key, value);
        CFRelease(key);
        CFRelease(value);
    }

    CFDataRef bodyData = CFDataCreate(kCFAllocatorDefault,
                                      reinterpret_cast<const UInt8*>(body.data()),
                                      body.size());
    CFHTTPMessageSetBody(request, bodyData);
    CFRelease(bodyData);

    CFReadStreamRef stream = CFReadStreamCreateForHTTPRequest(kCFAllocatorDefault, request);
    bool opened = CFReadStreamOpen(stream);
    if (!opened) {
        CFRelease(stream);
        CFRelease(request);
        error = "无法打开 HTTP 流";
        return "";
    }

    // 等待响应头可用（CFNetwork 异步接收响应）
    int waitCount = 0;
    while (!CFReadStreamHasBytesAvailable(stream) && waitCount < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waitCount++;
    }

    // 获取响应头和状态码
    std::string statusCode;
    std::string contentType;
    CFHTTPMessageRef resp = (CFHTTPMessageRef)(
        CFReadStreamCopyProperty(stream, kCFStreamPropertyHTTPResponseHeader));
    if (resp) {
        statusCode = std::to_string(CFHTTPMessageGetResponseStatusCode(resp));
        CFStringRef ct = CFHTTPMessageCopyHeaderFieldValue(resp, CFSTR("Content-Type"));
        if (ct) {
            const char* ctStr = CFStringGetCStringPtr(ct, kCFStringEncodingUTF8);
            if (ctStr) contentType = ctStr;
            CFRelease(ct);
        }
        if (retryAfter) {
            CFStringRef ra = CFHTTPMessageCopyHeaderFieldValue(resp, CFSTR("Retry-After"));
            if (ra) {
                const char* raStr = CFStringGetCStringPtr(ra, kCFStringEncodingUTF8);
                if (raStr) *retryAfter = raStr;
                CFRelease(ra);
            }
        }
        CFRelease(resp);
    }

    if (onHeaders) {
        int code = 0;
        try { code = std::stoi(statusCode); } catch (...) {}
        onHeaders(code, contentType);
    }

    // 读取响应体，逐 chunk 回调
    std::vector<uint8_t> buf(65536);
    CFIndex n;
    bool abort = false;
    while (!abort && (n = CFReadStreamRead(stream, buf.data(), buf.size())) > 0) {
        abort = !onChunk(reinterpret_cast<char*>(buf.data()), n);
    }

    CFErrorRef streamError = CFReadStreamCopyError(stream);
    if (streamError) {
        CFStringRef desc = CFErrorCopyDescription(streamError);
        error = CFStringGetCStringPtr(desc, kCFStringEncodingUTF8)
                ? std::string(CFStringGetCStringPtr(desc, kCFStringEncodingUTF8))
                : "网络错误";
        CFRelease(desc);
        CFRelease(streamError);
    }

    CFReadStreamClose(stream);
    CFRelease(stream);
    CFRelease(request);

    return statusCode;
}

bool LLMClient::parseFullChatResponse(const std::string& responseBody, const std::string& status,
                                      ChatResponse& out, std::string& error) const {
    int code = 0;
    try { code = std::stoi(status); } catch (...) {}

    if (code >= 400) {
        JSONValue parsed;
        std::string perr;
        if (JSON::parse(responseBody, parsed, perr)) {
            const JSONObject* obj = parsed.asObject();
            if (obj) {
                auto it = obj->find("error");
                if (it != obj->end()) {
                    const JSONObject* errObj = it->second.asObject();
                    if (errObj) {
                        std::string msg = getStringField(*errObj, "message");
                        if (!msg.empty()) {
                            error = msg;
                            return false;
                        }
                    }
                }
            }
        }
        error = "HTTP " + status;
        return false;
    }

    JSONValue parsed;
    std::string perr;
    if (!JSON::parse(responseBody, parsed, perr)) {
        error = "解析响应失败：" + perr;
        return false;
    }

    const JSONObject* root = parsed.asObject();
    if (!root) {
        error = "响应不是 JSON 对象";
        return false;
    }

    auto it = root->find("error");
    if (it != root->end()) {
        const JSONObject* errObj = it->second.asObject();
        if (errObj) {
            out.apiError = getStringField(*errObj, "message");
        }
        error = out.apiError.empty() ? "API 错误" : out.apiError;
        return false;
    }

    it = root->find("choices");
    if (it != root->end()) {
        const JSONArray* arr = it->second.asArray();
        if (arr) {
            for (const auto& cv : *arr) {
                ChatResponse::Choice choice;
                const JSONObject* cObj = cv.asObject();
                if (!cObj) continue;
                auto cit = cObj->find("message");
                if (cit != cObj->end()) {
                    choice.message = chatMessageFromJSON(cit->second);
                }
                cit = cObj->find("finish_reason");
                if (cit != cObj->end() && cit->second.asString()) {
                    choice.finishReason = *cit->second.asString();
                }
                out.choices.push_back(choice);
            }
        }
    }

    return true;
}

void LLMClient::chat(const std::vector<ChatMessage>& messages,
                     const std::vector<ToolDefinition>& tools,
                     const std::string& toolChoice,
                     std::function<void(bool, ChatResponse, std::string)> completion) {
    std::string body = buildRequestBody(messages, tools, toolChoice);

    std::vector<std::pair<std::string, std::string>> headers;
    headers.push_back({"Content-Type", "application/json"});
    headers.push_back({"Authorization", "Bearer " + config_.apiKey});

    std::thread([this, body, headers, completion]() {
        const int maxRetries = 3;
        ChatResponse result;

        for (int attempt = 0; attempt <= maxRetries; attempt++) {
            std::string responseBody;
            std::string error;
            std::string retryAfterHeader;
            std::string status = post(body, headers, responseBody, error);

            // 网络错误
            if (!error.empty()) {
                if (attempt < maxRetries) {
                    int delay = computeRetryDelay(attempt, retryAfterHeader);
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    continue;
                }
                completion(false, result, error);
                return;
            }

            int code = 0;
            try { code = std::stoi(status); } catch (...) {}

            // 可重试的 HTTP 错误
            if (isRetryableStatus(code) && attempt < maxRetries) {
                int delay = computeRetryDelay(attempt, retryAfterHeader);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                continue;
            }

            if (code >= 400) {
                // 尝试解析错误信息
                JSONValue parsed;
                std::string perr;
                if (JSON::parse(responseBody, parsed, perr)) {
                    const JSONObject* obj = parsed.asObject();
                    if (obj) {
                        auto it = obj->find("error");
                        if (it != obj->end()) {
                            const JSONObject* errObj = it->second.asObject();
                            if (errObj) {
                                std::string msg = getStringField(*errObj, "message");
                                if (!msg.empty()) {
                                    completion(false, result, msg);
                                    return;
                                }
                            }
                        }
                    }
                }
                completion(false, result, "HTTP " + status);
                return;
            }

            JSONValue parsed;
            std::string perr;
            if (!JSON::parse(responseBody, parsed, perr)) {
                completion(false, result, "解析响应失败：" + perr);
                return;
            }

            const JSONObject* root = parsed.asObject();
            if (!root) {
                completion(false, result, "响应不是 JSON 对象");
                return;
            }

            auto it = root->find("error");
            if (it != root->end()) {
                const JSONObject* errObj = it->second.asObject();
                if (errObj) {
                    result.apiError = getStringField(*errObj, "message");
                }
                completion(false, result, result.apiError.empty() ? "API 错误" : result.apiError);
                return;
            }

            it = root->find("choices");
            if (it != root->end()) {
                const JSONArray* arr = it->second.asArray();
                if (arr) {
                    for (const auto& cv : *arr) {
                        ChatResponse::Choice choice;
                        const JSONObject* cObj = cv.asObject();
                        if (!cObj) continue;
                        auto cit = cObj->find("message");
                        if (cit != cObj->end()) {
                            choice.message = chatMessageFromJSON(cit->second);
                        }
                        cit = cObj->find("finish_reason");
                        if (cit != cObj->end() && cit->second.asString()) {
                            choice.finishReason = *cit->second.asString();
                        }
                        result.choices.push_back(choice);
                    }
                }
            }

            completion(true, result, "");
            return;
        }

        // 所有重试都失败
        completion(false, result, "达到最大重试次数");
    }).detach();
}

void LLMClient::chatStream(const std::vector<ChatMessage>& messages,
                           const std::vector<ToolDefinition>& tools,
                           const std::string& toolChoice,
                           std::function<void(const std::string& contentDelta)> onContentDelta,
                           std::function<void(const std::string& reasoningDelta)> onReasoningDelta,
                           std::function<void(bool ok, ChatResponse response, std::string error)> completion) {
    std::string body = buildRequestBody(messages, tools, toolChoice, /*stream=*/true);

    std::vector<std::pair<std::string, std::string>> headers;
    headers.push_back({"Content-Type", "application/json"});
    headers.push_back({"Authorization", "Bearer " + config_.apiKey});
    headers.push_back({"Accept", "text/event-stream"});

    std::thread([this, body, headers, onContentDelta, onReasoningDelta, completion]() {
        // 累积器
        std::string accContent;
        std::string accReasoning;
        std::string accFinishReason;
        std::map<int, ChatMessage::ToolCall> accToolCalls;
        bool deltaSent = false;  // 一旦 delta 回调触发，不可重试（TUI 已显示部分内容）

        std::string sseBuffer;
        bool isSSE = true;       // 若服务端返回 JSON 而非 SSE 则降级
        bool fallbackMode = false;
        std::string fallbackBody;
        std::string httpStatus;
        std::string netError;

        // 处理一行 SSE data
        auto handleDataLine = [&](const std::string& payload) {
            if (payload == "[DONE]") return;

            JSONValue parsed;
            std::string perr;
            if (!JSON::parse(payload, parsed, perr)) return;

            const JSONObject* root = parsed.asObject();
            if (!root) return;

            // 检查顶层 error
            auto errIt = root->find("error");
            if (errIt != root->end()) {
                const JSONObject* errObj = errIt->second.asObject();
                if (errObj) {
                    netError = getStringField(*errObj, "message");
                }
                return;
            }

            auto choicesIt = root->find("choices");
            const JSONArray* arr = choicesIt != root->end() ? choicesIt->second.asArray() : nullptr;
            if (!arr || arr->empty()) return;

            const JSONObject* choice = (*arr)[0].asObject();
            if (!choice) return;

            auto deltaIt = choice->find("delta");
            const JSONObject* delta = deltaIt != choice->end() ? deltaIt->second.asObject() : nullptr;
            if (!delta) {
                // 可能是 finish_reason 在 choice 层
                auto frIt = choice->find("finish_reason");
                if (frIt != choice->end() && frIt->second.asString()) {
                    accFinishReason = *frIt->second.asString();
                }
                return;
            }

            // content delta
            auto contentIt = delta->find("content");
            if (contentIt != delta->end() && contentIt->second.asString()) {
                const std::string& chunk = *contentIt->second.asString();
                if (!chunk.empty()) {
                    accContent += chunk;
                    deltaSent = true;
                    if (onContentDelta) onContentDelta(chunk);
                }
            }

            // reasoning_content delta (DeepSeek-R1)
            auto reasonIt = delta->find("reasoning_content");
            if (reasonIt != delta->end() && reasonIt->second.asString()) {
                const std::string& chunk = *reasonIt->second.asString();
                if (!chunk.empty()) {
                    accReasoning += chunk;
                    deltaSent = true;
                    if (onReasoningDelta) onReasoningDelta(chunk);
                }
            }

            // tool_calls delta
            auto tcIt = delta->find("tool_calls");
            if (tcIt != delta->end()) {
                const JSONArray* tcArr = tcIt->second.asArray();
                if (tcArr) {
                    for (const auto& tcVal : *tcArr) {
                        const JSONObject* tcObj = tcVal.asObject();
                        if (!tcObj) continue;

                        int idx = 0;
                        auto idxIt = tcObj->find("index");
                        if (idxIt != tcObj->end() && idxIt->second.asNumber()) {
                            idx = static_cast<int>(*idxIt->second.asNumber());
                        }

                        auto& tc = accToolCalls[idx];
                        // 首次出现时设置 id/type
                        auto idIt = tcObj->find("id");
                        if (idIt != tcObj->end() && idIt->second.asString()) {
                            tc.id = *idIt->second.asString();
                        }
                        auto typeIt = tcObj->find("type");
                        if (typeIt != tcObj->end() && typeIt->second.asString()) {
                            tc.type = *typeIt->second.asString();
                        }
                        auto funcIt = tcObj->find("function");
                        const JSONObject* funcObj = funcIt != tcObj->end() ? funcIt->second.asObject() : nullptr;
                        if (funcObj) {
                            auto nameIt = funcObj->find("name");
                            if (nameIt != funcObj->end() && nameIt->second.asString()) {
                                tc.function.name += *nameIt->second.asString();
                            }
                            auto argIt = funcObj->find("arguments");
                            if (argIt != funcObj->end() && argIt->second.asString()) {
                                tc.function.arguments += *argIt->second.asString();
                            }
                        }
                    }
                }
            }

            // finish_reason 可能在 delta 或 choice 层
            auto frIt = choice->find("finish_reason");
            if (frIt != choice->end() && frIt->second.asString()) {
                const std::string& fr = *frIt->second.asString();
                if (!fr.empty() && fr != "null") {
                    accFinishReason = fr;
                }
            }
        };

        // 处理 SSE buffer：按行分割，完整行处理后保留不完整部分
        auto processSSEBuffer = [&]() {
            size_t pos;
            while ((pos = sseBuffer.find('\n')) != std::string::npos) {
                std::string line = sseBuffer.substr(0, pos);
                sseBuffer.erase(0, pos + 1);
                // 去掉 \r
                if (!line.empty() && line.back() == '\r') line.pop_back();

                if (line.empty()) continue;           // SSE 空行是分隔符
                if (line.rfind(":sse_", 0) == 0) continue; // 注释/心跳

                if (line.rfind("data: ", 0) == 0) {
                    handleDataLine(line.substr(6));
                } else if (line.rfind("data:", 0) == 0) {
                    handleDataLine(line.substr(5));
                }
                // 其他 event:/id:/retry: 行忽略
            }
        };

        const int maxRetries = 3;

        for (int attempt = 0; attempt <= maxRetries; attempt++) {
            // 重置累积器（重试时清空上一轮的数据）
            accContent.clear();
            accReasoning.clear();
            accFinishReason.clear();
            accToolCalls.clear();
            sseBuffer.clear();
            fallbackBody.clear();
            httpStatus.clear();
            netError.clear();
            isSSE = true;
            fallbackMode = false;

            std::string retryAfterHeader;

            httpStatus = httpRequestStream(body, headers,
                [&](int statusCode, const std::string& contentType) {
                    if (statusCode >= 400) {
                        fallbackMode = true;
                        isSSE = false;
                    } else if (contentType.find("event-stream") == std::string::npos &&
                               contentType.find("text/event") == std::string::npos) {
                        if (contentType.find("json") != std::string::npos) {
                            fallbackMode = true;
                            isSSE = false;
                        }
                    }
                },
                [&](const char* data, size_t len) -> bool {
                    if (fallbackMode || !isSSE) {
                        fallbackBody.append(data, len);
                        return true;
                    }
                    sseBuffer.append(data, len);
                    processSSEBuffer();
                    return true;
                },
                netError,
                &retryAfterHeader);

            // 处理尾部残留（没有尾换行的最后一行）
            if (isSSE && !sseBuffer.empty()) {
                if (!sseBuffer.empty() && sseBuffer.back() == '\r') sseBuffer.pop_back();
                if (sseBuffer.rfind("data: ", 0) == 0) {
                    handleDataLine(sseBuffer.substr(6));
                } else if (sseBuffer.rfind("data:", 0) == 0) {
                    handleDataLine(sseBuffer.substr(5));
                }
            }

            // 网络错误：仅在尚未向 TUI 发送数据时重试
            if (!netError.empty()) {
                if (!deltaSent && attempt < maxRetries) {
                    int delay = computeRetryDelay(attempt, retryAfterHeader);
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    continue;
                }
                ChatResponse result;
                completion(false, result, netError);
                return;
            }

            // 检查 HTTP 状态码
            int code = 0;
            try { code = std::stoi(httpStatus); } catch (...) {}

            // 可重试的 HTTP 错误（429, 5xx）
            if (isRetryableStatus(code) && attempt < maxRetries) {
                int delay = computeRetryDelay(attempt, retryAfterHeader);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                continue;
            }

            // Fallback：完整 JSON 响应
            if (fallbackMode || !isSSE) {
                ChatResponse result;
                if (code >= 400) {
                    // 尝试解析错误信息
                    JSONValue parsed;
                    std::string perr;
                    if (JSON::parse(fallbackBody, parsed, perr)) {
                        const JSONObject* obj = parsed.asObject();
                        if (obj) {
                            auto it = obj->find("error");
                            if (it != obj->end()) {
                                const JSONObject* errObj = it->second.asObject();
                                if (errObj) {
                                    std::string msg = getStringField(*errObj, "message");
                                    if (!msg.empty()) {
                                        completion(false, result, msg);
                                        return;
                                    }
                                }
                            }
                        }
                    }
                    completion(false, result, "HTTP " + httpStatus);
                    return;
                }
                bool ok = parseFullChatResponse(fallbackBody, httpStatus, result, netError);
                if (!ok) {
                    completion(false, result, netError);
                    return;
                }
                if (!result.choices.empty()) {
                    const auto& msg = result.choices[0].message;
                    if (onContentDelta && !msg.content.empty()) onContentDelta(msg.content);
                    if (onReasoningDelta && !msg.reasoningContent.empty()) onReasoningDelta(msg.reasoningContent);
                }
                completion(true, result, "");
                return;
            }

            // SSE 完成
            if (code >= 400) {
                ChatResponse result;
                completion(false, result, "HTTP " + httpStatus);
                return;
            }

            // 构建完整 ChatResponse
            ChatResponse result;
            ChatResponse::Choice choice;
            choice.message.role = "assistant";
            choice.message.content = accContent;
            choice.message.hasContent = !accContent.empty();
            choice.message.reasoningContent = accReasoning;
            choice.finishReason = accFinishReason;

            for (auto& kv : accToolCalls) {
                auto& tc = kv.second;
                if (tc.id.empty()) {
                    tc.id = "call_" + std::to_string(kv.first);
                }
                if (tc.type.empty()) tc.type = "function";
                choice.message.toolCalls.push_back(std::move(tc));
            }

            result.choices.push_back(std::move(choice));
            completion(true, result, "");
            return;
        }

        // 所有重试都失败
        ChatResponse result;
        completion(false, result, "达到最大重试次数");
    }).detach();
}
