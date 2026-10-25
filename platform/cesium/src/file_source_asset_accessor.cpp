#include <mln/cesium/file_source_asset_accessor.hpp>

#include <mln/storage/file_source.hpp>
#include <mln/storage/file_source_manager.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/storage/response.hpp>
#include <mln/util/async_request.hpp>
#include <mln/util/client_options.hpp>
#include <mln/util/logging.hpp>

#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/HttpHeaders.h>
#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/IAssetResponse.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace mln {
namespace cesium {

namespace {

class Response final : public CesiumAsync::IAssetResponse {
public:
    Response(uint16_t status, std::string contentType, std::shared_ptr<const std::string> body)
        : status_(status),
          contentType_(std::move(contentType)),
          body_(std::move(body)) {
        if (!contentType_.empty()) headers_["Content-Type"] = contentType_;
    }

    uint16_t statusCode() const override { return status_; }
    std::string contentType() const override { return contentType_; }
    const CesiumAsync::HttpHeaders& headers() const override { return headers_; }
    std::span<const std::byte> data() const override {
        if (!body_) return {};
        return {reinterpret_cast<const std::byte*>(body_->data()), body_->size()};
    }

private:
    uint16_t status_;
    std::string contentType_;
    std::shared_ptr<const std::string> body_;
    CesiumAsync::HttpHeaders headers_;
};

class Request final : public CesiumAsync::IAssetRequest {
public:
    Request(std::string method, std::string url, CesiumAsync::HttpHeaders headers, std::unique_ptr<Response> response)
        : method_(std::move(method)),
          url_(std::move(url)),
          headers_(std::move(headers)),
          response_(std::move(response)) {}

    const std::string& method() const override { return method_; }
    const std::string& url() const override { return url_; }
    const CesiumAsync::HttpHeaders& headers() const override { return headers_; }
    const CesiumAsync::IAssetResponse* response() const override { return response_.get(); }

private:
    std::string method_;
    std::string url_;
    CesiumAsync::HttpHeaders headers_;
    std::unique_ptr<Response> response_;
};

// What the file source cannot tell us, the URL can: cesium-native sniffs binary tile
// formats by their magic bytes and only needs a hint for JSON.
std::string contentTypeFor(const std::string& url) {
    const std::string path = url.substr(0, url.find('?'));
    const auto dot = path.rfind('.');
    const auto slash = path.rfind('/');
    const std::string ext = (dot == std::string::npos || (slash != std::string::npos && dot < slash)) ? ""
                                                                                                       : path.substr(dot + 1);
    if (ext == "json") return "application/json";
    if (ext == "gltf") return "model/gltf+json";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "ktx2") return "image/ktx2";
    if (ext == "webp") return "image/webp";
    return "application/octet-stream";
}

constexpr int maxAttempts = 3;

bool isRetried(const mln::Response& response) {
    return response.error && (response.error->reason == mln::Response::Error::Reason::Server ||
                              response.error->reason == mln::Response::Error::Reason::Connection);
}

uint16_t statusFor(const mln::Response& response) {
    if (!response.error) return response.noContent ? 204 : 200;
    // The platform HTTP sources word a non-2xx answer as "HTTP status code N".
    const std::string prefix = "HTTP status code ";
    const auto& message = response.error->message;
    if (message.rfind(prefix, 0) == 0) {
        const int code = std::atoi(message.c_str() + prefix.size());
        if (code > 0 && code < 1000) return static_cast<uint16_t>(code);
    }
    switch (response.error->reason) {
        case mln::Response::Error::Reason::NotFound:
            return 404;
        case mln::Response::Error::Reason::RateLimit:
            return 429;
        case mln::Response::Error::Reason::Connection:
            return 0; // no HTTP exchange happened, as cesium-native's own accessors report it
        default:
            return 500;
    }
}

CesiumAsync::HttpHeaders toHttpHeaders(const std::vector<CesiumAsync::IAssetAccessor::THeader>& headers) {
    CesiumAsync::HttpHeaders out;
    for (const auto& [name, value] : headers) out[name] = value;
    return out;
}

} // namespace

// Lives on its own thread with a run loop, which is what FileSource::request needs for
// its callback. Requests are kept alive until they answer.
class FileSourceAssetAccessor::Fetcher {
public:
    using Promise = CesiumAsync::Promise<std::shared_ptr<CesiumAsync::IAssetRequest>>;

    Fetcher()
        : fileSource(FileSourceManager::get()->getFileSource(FileSourceType::Network, ResourceOptions::Default())) {}

    void fetch(std::string url, CesiumAsync::HttpHeaders headers, Promise promise) {
        if (!fileSource) {
            promise.resolve(
                std::make_shared<Request>("GET", url, std::move(headers), std::make_unique<Response>(0, "", nullptr)));
            return;
        }
        const uint64_t id = nextId++;
        const Resource resource(Resource::Unknown, url);
        // The file source retries timeouts and 5xx answers by calling back again, so a
        // request stays open for a few attempts before the tile is reported as failed.
        inFlight[id] = fileSource->request(
            resource,
            [this, id, url, headers = std::move(headers), promise, attempts = 0](
                const mln::Response& response) mutable {
                if (isRetried(response) && ++attempts < maxAttempts) return;
                if (response.error) {
                    Log::Warning(Event::HttpRequest,
                                 "Cesium tiles: " + url + " failed: " + response.error->message);
                }
                auto answer = std::make_unique<Response>(statusFor(response), contentTypeFor(url), response.data);
                promise.resolve(std::make_shared<Request>("GET", url, std::move(headers), std::move(answer)));
                inFlight.erase(id);
            });
    }

private:
    std::shared_ptr<FileSource> fileSource;
    std::map<uint64_t, std::unique_ptr<AsyncRequest>> inFlight;
    uint64_t nextId = 1;
};

FileSourceAssetAccessor::FileSourceAssetAccessor()
    : thread(std::make_unique<util::Thread<Fetcher>>("CesiumTiles")) {}

FileSourceAssetAccessor::~FileSourceAssetAccessor() = default;

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> FileSourceAssetAccessor::get(
    const CesiumAsync::AsyncSystem& asyncSystem, const std::string& url, const std::vector<THeader>& headers) {
    auto promise = asyncSystem.createPromise<std::shared_ptr<CesiumAsync::IAssetRequest>>();
    thread->actor().invoke(&Fetcher::fetch, url, toHttpHeaders(headers), promise);
    return promise.getFuture();
}

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> FileSourceAssetAccessor::request(
    const CesiumAsync::AsyncSystem& asyncSystem,
    const std::string& verb,
    const std::string& url,
    const std::vector<THeader>& headers,
    const std::span<const std::byte>&) {
    if (verb == "GET") return get(asyncSystem, url, headers);
    // Only ion and iTwin content needs anything but GET; the map's file source has no POST.
    return asyncSystem.createResolvedFuture<std::shared_ptr<CesiumAsync::IAssetRequest>>(
        std::make_shared<Request>(verb, url, toHttpHeaders(headers), std::make_unique<Response>(405, "", nullptr)));
}

} // namespace cesium
} // namespace mln
